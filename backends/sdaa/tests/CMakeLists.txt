# Copyright (c) 2022 PaddlePaddle Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License. You may obtain a copy of
# the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
# License for the specific language governing permissions and limitations under
# the License

function(py_test_modules TARGET_NAME)
  set(options SERIAL)
  set(oneValueArgs "")
  set(multiValueArgs MODULES DEPS ENVS)
  cmake_parse_arguments(py_test_modules "${options}" "${oneValueArgs}"
                        "${multiValueArgs}" ${ARGN})

  add_test(
    NAME ${TARGET_NAME}
    COMMAND
      ${CMAKE_COMMAND} -E env ${TEST_CUSTOM_DEVICE_ROOT}
      PYTHONPATH=${PYTHON_SOURCE_DIR}/tests:${CMAKE_CURRENT_SOURCE_DIR}:$ENV{PYTHONPATH}
      ${py_test_modules_ENVS} python ${PYTHON_SOURCE_DIR}/tools/test_runner.py
      ${py_test_modules_MODULES}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

  if(py_test_modules_SERIAL)
    set_property(TEST ${TARGET_NAME} PROPERTY RUN_SERIAL 1)
  endif()
endfunction()

set(PYTHON_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../python")

enable_testing()

add_test(
  NAME test_MNIST_model
  COMMAND
    ${CMAKE_COMMAND} -E env
    CUSTOM_DEVICE_ROOT=${CMAKE_BINARY_DIR}/python/paddle_custom_device/
    PYTHONPATH=${PYTHON_SOURCE_DIR}:${PYTHON_SOURCE_DIR}/tests:$ENV{PYTHONPATH}
    python test_MNIST_model.py
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

add_subdirectory(unittests)
add_subdirectory(runtime)
add_subdirectory(distribution)
