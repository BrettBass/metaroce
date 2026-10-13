/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#ifndef NO_CUDA
#include <cuda.h>
#endif

namespace facebook::neteng::ai::rdma_gen {

#ifndef NO_CUDA
class CudaWrapper {
 public:
  virtual ~CudaWrapper() = default;
  virtual int cuMemAlloc(CUdeviceptr* dptr, size_t size) {
    return ::cuMemAlloc(dptr, size);
  }
};
#else
class CudaWrapper {};
#endif

} // namespace facebook::neteng::ai::rdma_gen
