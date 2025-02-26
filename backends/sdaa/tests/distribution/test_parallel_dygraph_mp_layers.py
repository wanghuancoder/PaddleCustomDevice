# Copyright (c) 2023 PaddlePaddle Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import unittest
import copy
import os
import subprocess
import time
import sys
import unittest

import paddle
from paddle.distributed.utils.launch_utils import (
    TrainerProc,
    find_free_ports,
    get_cluster,
    watch_local_trainers,
)


def get_gpus(selected_gpus):
    selected_gpus = [x.strip() for x in selected_gpus.split(",")]
    return selected_gpus


def get_cluster_from_args(selected_gpus):
    cluster_node_ips = "127.0.0.1"
    node_ip = "127.0.0.1"

    node_ips = [x.strip() for x in cluster_node_ips.split(",")]

    node_ips.index(node_ip)

    free_ports = None

    free_ports = find_free_ports(len(selected_gpus))
    if free_ports is not None:
        free_ports = list(free_ports)

    trainer_endpoints = []
    for ip in node_ips:
        trainer_endpoints.append(["%s:%d" % (ip, port) for port in free_ports])
    return get_cluster(node_ips, node_ip, trainer_endpoints, selected_gpus)


def start_local_trainers(
    cluster,
    pod,
    training_script,
    training_script_args,
    device_type,
    allocator_strategy="auto_growth",
    log_dir=None,
):
    current_env = copy.copy(os.environ.copy())
    # paddle broadcast ncclUniqueId use socket, and
    # proxy maybe make trainers unreachable, so delete them.
    # if we set them to "", grpc will log error message "bad uri"
    # so just delete them.
    current_env.pop("http_proxy", None)
    current_env.pop("https_proxy", None)

    procs = []
    for t in pod.trainers:
        proc_env = {
            f"FLAGS_selected_{device_type}s": "%s" % ",".join([str(g) for g in t.gpus]),
            "PADDLE_DISTRI_BACKEND": "xccl",
            "PADDLE_XCCL_BACKEND": device_type,
            "PADDLE_TRAINER_ID": "%d" % t.rank,
            "PADDLE_CURRENT_ENDPOINT": "%s" % t.endpoint,
            "PADDLE_TRAINERS_NUM": "%d" % cluster.trainers_nranks(),
            "PADDLE_TRAINER_ENDPOINTS": ",".join(cluster.trainers_endpoints()),
        }

        proc_env["FLAGS_allocator_strategy"] = allocator_strategy
        if allocator_strategy == "auto_growth":
            proc_env["FLAGS_fraction_of_gpu_memory_to_use"] = "0.1"

        current_env.update(proc_env)

        print(f"trainer proc env:{current_env}")

        if os.getenv("WITH_COVERAGE", "OFF") == "ON":
            cmd = "python -m coverage run --branch -p " + training_script
        else:
            cmd = "python -u " + training_script
            print("cmd:", cmd)

        print(f"start trainer proc:{cmd} env:{proc_env}")

        fn = None

        proc = subprocess.Popen(cmd.split(" "), env=current_env)

        tp = TrainerProc()
        tp.proc = proc
        tp.rank = t.rank
        tp.log_fn = fn
        tp.cmd = cmd

        procs.append(tp)

    return procs


class TestMultipleCustomDevices(unittest.TestCase):
    def run_mnist_2_custom_devices(
        self,
        target_file_name,
        device_type,
        allocator_strategy="naive_best_fit",
        selected_gpus=["0", "1"],
    ):
        dev_cnt = [
            dev.split(":")[0] == device_type
            for dev in paddle.device.get_available_device()
        ].count(True)
        if dev_cnt < 2:
            return

        cluster = None
        pod = None

        cluster, pod = get_cluster_from_args(selected_gpus)

        procs = start_local_trainers(
            cluster,
            pod,
            allocator_strategy=allocator_strategy,
            training_script=target_file_name,
            training_script_args=[],
            device_type=device_type,
        )

        while True:
            alive = watch_local_trainers(procs, cluster.trainers_endpoints())

            if not alive:
                print(f"Local procs complete, POD info:{pod}")
                print(sys.exc_info())
                break
            time.sleep(3)


class TestModelParallelLayer(TestMultipleCustomDevices):
    @unittest.skip("column_parallel can not release resource correctly")
    def test_column_parallel(self):
        self.run_mnist_2_custom_devices("hybrid_column_parallel_mp_layers.py", "sdaa")

    def test_hybrid_parallel_mp_layer(self):
        self.run_mnist_2_custom_devices(
            "hybrid_cross_entropy_parallel_mp_layers.py", "sdaa"
        )
        self.run_mnist_2_custom_devices(
            "hybrid_embedding_parallel_mp_layers.py", "sdaa"
        )
        self.run_mnist_2_custom_devices("hybrid_row_parallel_mp_layers.py", "sdaa")


if __name__ == "__main__":
    unittest.main()
