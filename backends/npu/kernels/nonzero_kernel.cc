// Copyright (c) 2022 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "kernels/funcs/npu_funcs.h"
#include "kernels/funcs/npu_op_runner.h"

namespace custom_kernel {

template <typename T, typename Context>
void CastKernel(const Context& dev_ctx,
                const phi::DenseTensor& x,
                phi::DataType dtype,
                phi::DenseTensor* out);

template <typename T, typename Context>
void SumKernel(const Context& dev_ctx,
               const phi::DenseTensor& x,
               const phi::IntArray& axes,
               phi::DataType out_dtype,
               bool keep_dim,
               phi::DenseTensor* out);

template <typename T, typename Context>
void StridedSliceKernel(const Context& dev_ctx,
                        const phi::DenseTensor& x,
                        const std::vector<int>& axes,
                        const phi::IntArray& starts,
                        const phi::IntArray& ends,
                        const phi::IntArray& strides,
                        phi::DenseTensor* out);

template <typename T, typename Context>
void AclopNonZeroKernel(const Context& dev_ctx,
                        const phi::DenseTensor& condition,
                        phi::DenseTensor* out) {
  auto dims = condition.dims();
  const int rank = dims.size();

  const aclrtStream& stream = dev_ctx.stream();

  // Run Cast and ReduceSum to get 0 dim of Out
  phi::DenseTensor booled_cond;
  if (condition.dtype() != phi::DataType::BOOL) {
    booled_cond.Resize(dims);
    auto bool_type = ConvertToNpuDtype(phi::DataType::BOOL);
    booled_cond.Resize(dims);
    dev_ctx.template Alloc<bool>(&booled_cond);
    const auto& booled_runner =
        NpuOpRunner("Cast",
                    {condition},
                    {booled_cond},
                    {{"dst_type", static_cast<int32_t>(bool_type)}});
    booled_runner.Run(stream, true);
  } else {
    booled_cond = condition;
  }

  phi::DenseTensor casted_cond;
  auto dst_dtype = ConvertToNpuDtype(phi::DataType::FLOAT32);
  casted_cond.Resize(dims);
  dev_ctx.template Alloc<float>(&casted_cond);
  const auto& cast_runner =
      NpuOpRunner("Cast",
                  {booled_cond},
                  {casted_cond},
                  {{"dst_type", static_cast<int>(dst_dtype)}});
  cast_runner.Run(stream);

  phi::DenseTensor sumed_true_num;
  sumed_true_num.Resize({1});
  dev_ctx.template Alloc<float>(&sumed_true_num);

  std::vector<int> axes_vec;
  for (int i = 0; i < dims.size(); ++i) {
    axes_vec.push_back(i);
  }
  NpuOpRunner sum_runner;
  sum_runner.SetType("ReduceSum");
  sum_runner.AddInput(casted_cond);
  sum_runner.AddInput(dev_ctx, std::move(axes_vec));
  sum_runner.AddOutput(sumed_true_num);
  sum_runner.AddAttr("keep_dims", false);
  sum_runner.Run(stream);
  phi::DenseTensor local_true_num;
  TensorCopy(dev_ctx, sumed_true_num, true, &local_true_num, phi::CPUPlace());
  int true_num = static_cast<int32_t>(*local_true_num.data<float>());
  out->Resize(phi::make_ddim({true_num, rank}));
  dev_ctx.template Alloc<int64_t>(out);

  if (true_num == 0) {
    return;
  }

  phi::DenseTensorMeta out_meta = {out->dtype(), out->dims(), out->layout()};
  out->set_meta(out_meta);
  NpuOpRunner runner{"Where", {condition}, {*out}};
  runner.Run(stream);
}

template <typename T, typename Context>
void NonZeroKernel(const Context& dev_ctx,
                   const phi::DenseTensor& condition,
                   phi::DenseTensor* out) {
  DO_COMPATIBILITY(
      aclnnNonzero,
      (custom_kernel::AclopNonZeroKernel<T, Context>(dev_ctx, condition, out)));
  auto dims = condition.dims();
  const int rank = dims.size();

  // Run Cast and ReduceSum to get 0 dim of Out
  phi::DenseTensor booled_cond;
  if (condition.dtype() != phi::DataType::BOOL) {
    booled_cond.Resize(dims);
    custom_kernel::CastKernel<T, Context>(
        dev_ctx, condition, phi::DataType::BOOL, &booled_cond);
  } else {
    booled_cond = condition;
  }

  phi::DenseTensor casted_cond;
  casted_cond.Resize(dims);
  custom_kernel::CastKernel<T, Context>(
      dev_ctx, booled_cond, phi::DataType::FLOAT32, &casted_cond);

  phi::DenseTensor sumed_true_num;
  sumed_true_num.Resize({1});
  dev_ctx.template Alloc<float>(&sumed_true_num);

  std::vector<int64_t> axes_vec;
  for (int i = 0; i < dims.size(); ++i) {
    axes_vec.push_back(i);
  }
  bool keep_dim = false;
  phi::IntArray reduce_dims_arry(axes_vec);
  custom_kernel::SumKernel<T, Context>(dev_ctx,
                                       casted_cond,
                                       reduce_dims_arry,
                                       phi::DataType::FLOAT32,
                                       keep_dim,
                                       &sumed_true_num);
  phi::DenseTensor local_true_num;
  TensorCopy(dev_ctx, sumed_true_num, true, &local_true_num, phi::CPUPlace());
  int true_num = static_cast<int32_t>(*local_true_num.data<float>());
  out->Resize(phi::make_ddim({true_num, rank}));

  if (true_num == 0) {
    dev_ctx.template Alloc<int64_t>(out);
    return;
  }

  phi::DenseTensor out_full;
  out_full.Resize(phi::make_ddim({condition.numel(), rank}));
  dev_ctx.template Alloc<int64_t>(&out_full);
  EXEC_NPU_CMD(aclnnNonzero, dev_ctx, condition, out_full);

  std::vector<int> axis = {0};
  phi::IntArray start = {0};
  phi::IntArray end = {true_num};
  phi::IntArray strid = {1};
  custom_kernel::StridedSliceKernel<int64_t, Context>(
      dev_ctx, out_full, axis, start, end, strid, out);
}

}  // namespace custom_kernel

PD_REGISTER_PLUGIN_KERNEL(nonzero,
                          npu,
                          ALL_LAYOUT,
                          custom_kernel::NonZeroKernel,
                          bool,
                          int,
                          int64_t,
                          float,
                          double) {
  kernel->OutputAt(0).SetDataType(phi::DataType::INT64);
}
