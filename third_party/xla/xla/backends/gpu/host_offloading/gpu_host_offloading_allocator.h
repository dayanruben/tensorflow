/* Copyright 2025 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#ifndef XLA_BACKENDS_GPU_HOST_OFFLOADING_GPU_HOST_OFFLOADING_ALLOCATOR_H_
#define XLA_BACKENDS_GPU_HOST_OFFLOADING_GPU_HOST_OFFLOADING_ALLOCATOR_H_

#include <memory>

#include "xla/core/host_offloading/host_offloading_allocator.h"
#include "xla/stream_executor/stream_executor.h"

namespace xla::gpu {

std::unique_ptr<HostOffloadingAllocator> CreateGpuHostOffloadingAllocator(
    stream_executor::StreamExecutor* executor);

}  // namespace xla::gpu

#endif  // XLA_BACKENDS_GPU_HOST_OFFLOADING_GPU_HOST_OFFLOADING_ALLOCATOR_H_
