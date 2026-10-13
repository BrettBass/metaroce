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

#include "rdma_gen/memory/RocmMemory.h"

#ifndef NO_ROCM
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <mutex>
#endif

namespace facebook::neteng::ai::rdma_gen {

#ifndef NO_ROCM

// Track whether HIP runtime is still valid for cleanup operations.
// During process exit, the HIP runtime's global state may be destroyed before
// our destructor runs, causing "pure virtual method called" errors.
// We use atexit() to mark when exit handlers start running.
static std::atomic<bool> g_hipRuntimeValid{true};

static void markHipRuntimeInvalid() {
  g_hipRuntimeValid.store(false, std::memory_order_release);
}

// Register the atexit handler once
static std::once_flag g_atexitOnce;
static void registerAtexitHandler() {
  std::call_once(g_atexitOnce, []() { std::atexit(markHipRuntimeInvalid); });
}

RocmMemory::~RocmMemory() {
  // Close all dmabuf file descriptors first (before HIP cleanup)
  for (int fd : dmabufFds_) {
    if (fd >= 0) {
      close(fd);
    }
  }
  dmabufFds_.clear();

  // Only attempt HIP cleanup if the runtime is still valid (not during exit)
  if (g_hipRuntimeValid.load(std::memory_order_acquire)) {
    // Clean up VM allocations in reverse order
    for (size_t i = vmPtrs_.size(); i > 0; --i) {
      size_t idx = i - 1;
      if (vmPtrs_[idx] != nullptr && vmSizes_[idx] > 0) {
        hipDeviceptr_t dptr = reinterpret_cast<hipDeviceptr_t>(vmPtrs_[idx]);
        (void)hipMemUnmap(dptr, vmSizes_[idx]);
        (void)hipMemAddressFree(dptr, vmSizes_[idx]);
      }
    }
    vmPtrs_.clear();
    vmSizes_.clear();

    // Release allocation handles
    for (auto& handle : vmHandles_) {
      (void)hipMemRelease(handle);
    }
  }
  vmHandles_.clear();
}
#else
RocmMemory::~RocmMemory() = default;
#endif

void RocmMemory::initRocm() {
#ifndef NO_ROCM
  // Register atexit handler to mark HIP runtime as invalid during process exit
  registerAtexitHandler();

  hipError_t error;
  int deviceCount = 0;

  error = hipGetDeviceCount(&deviceCount);
  if (error != hipSuccess) {
    LOG(FATAL) << fmt::format(
        "hipGetDeviceCount() failed with error: {} ({})",
        hipGetErrorString(error),
        static_cast<int>(error));
  }

  CHECK_NE(deviceCount, 0)
      << "There are no available device(s) that support ROCm/HIP";
  CHECK_LT(params_.gpu_idx, deviceCount) << fmt::format(
      "No such device ID ({}) exists in system. Available devices: {}",
      params_.gpu_idx,
      deviceCount);

  LOG(INFO) << "Listing all ROCm/HIP devices in system";

  for (int index = 0; index < deviceCount; index++) {
    hipDeviceProp_t prop;
    error = hipGetDeviceProperties(&prop, index);
    if (error != hipSuccess) {
      LOG(WARNING) << fmt::format(
          "hipGetDeviceProperties() failed for device {}: {}",
          index,
          hipGetErrorString(error));
      continue;
    }
    LOG(INFO) << fmt::format(
        "ROCm device {}: {} (PCIe {}:{}:{})",
        index,
        prop.name,
        prop.pciBusID,
        prop.pciDeviceID,
        prop.pciDomainID);
  }

  LOG(INFO) << fmt::format("Picking ROCm device: {}", params_.gpu_idx);
  hipDeviceId_ = params_.gpu_idx;

  error = hipSetDevice(hipDeviceId_);
  if (error != hipSuccess) {
    LOG(FATAL) << fmt::format(
        "hipSetDevice({}) failed with error: {} ({})",
        hipDeviceId_,
        hipGetErrorString(error),
        static_cast<int>(error));
  }

  hipDeviceProp_t prop;
  error = hipGetDeviceProperties(&prop, hipDeviceId_);
  if (error == hipSuccess) {
    LOG(INFO) << fmt::format(
        "[pid: {}, dev: {}], device name: [{}]",
        getpid(),
        hipDeviceId_,
        prop.name);
  }

  // Get allocation granularity for VM APIs
  hipMemAllocationProp allocProp = {};
  allocProp.type = hipMemAllocationTypePinned;
  allocProp.location.type = hipMemLocationTypeDevice;
  allocProp.location.id = hipDeviceId_;
  allocProp.requestedHandleTypes = hipMemHandleTypePosixFileDescriptor;

  error = hipMemGetAllocationGranularity(
      &granularity_, &allocProp, hipMemAllocationGranularityMinimum);
  if (error != hipSuccess) {
    LOG(WARNING) << fmt::format(
        "hipMemGetAllocationGranularity failed: {} ({}). "
        "Will fall back to non-dmabuf allocation.",
        hipGetErrorString(error),
        static_cast<int>(error));
    granularity_ = 0;
  } else {
    LOG(INFO) << fmt::format(
        "HIP VM allocation granularity: {} bytes", granularity_);
  }
#else
  LOG(FATAL) << "rdma_gen compiled without ROCm support. "
             << "Cannot use --rdma_mode=rocm without ROCm/HIP libraries.";
#endif // ifndef NO_ROCM
}

#ifndef NO_ROCM
void* RocmMemory::allocateVMMemory(
    size_t size,
    hipMemGenericAllocationHandle_t& handle) {
  hipError_t error;

  // Round up size to granularity
  size_t alignedSize =
      ((size + granularity_ - 1) / granularity_) * granularity_;

  // Set up allocation properties for exportable memory
  hipMemAllocationProp allocProp = {};
  allocProp.type = hipMemAllocationTypePinned;
  allocProp.location.type = hipMemLocationTypeDevice;
  allocProp.location.id = hipDeviceId_;
  allocProp.requestedHandleTypes = hipMemHandleTypePosixFileDescriptor;

  // Reserve virtual address range
  hipDeviceptr_t dptr = 0;
  error = hipMemAddressReserve(&dptr, alignedSize, 0, 0, 0);
  if (error != hipSuccess) {
    LOG(ERROR) << fmt::format(
        "hipMemAddressReserve({}) failed: {} ({})",
        alignedSize,
        hipGetErrorString(error),
        static_cast<int>(error));
    return nullptr;
  }

  // Create physical memory allocation
  error = hipMemCreate(&handle, alignedSize, &allocProp, 0);
  if (error != hipSuccess) {
    LOG(ERROR) << fmt::format(
        "hipMemCreate({}) failed: {} ({})",
        alignedSize,
        hipGetErrorString(error),
        static_cast<int>(error));
    (void)hipMemAddressFree(dptr, alignedSize);
    return nullptr;
  }

  // Map physical memory to virtual address
  error = hipMemMap(dptr, alignedSize, 0, handle, 0);
  if (error != hipSuccess) {
    LOG(ERROR) << fmt::format(
        "hipMemMap failed: {} ({})",
        hipGetErrorString(error),
        static_cast<int>(error));
    (void)hipMemRelease(handle);
    (void)hipMemAddressFree(dptr, alignedSize);
    return nullptr;
  }

  // Set access permissions
  hipMemAccessDesc accessDesc = {};
  accessDesc.location.type = hipMemLocationTypeDevice;
  accessDesc.location.id = hipDeviceId_;
  accessDesc.flags = hipMemAccessFlagsProtReadWrite;

  error = hipMemSetAccess(dptr, alignedSize, &accessDesc, 1);
  if (error != hipSuccess) {
    LOG(ERROR) << fmt::format(
        "hipMemSetAccess failed: {} ({})",
        hipGetErrorString(error),
        static_cast<int>(error));
    (void)hipMemUnmap(dptr, alignedSize);
    (void)hipMemRelease(handle);
    (void)hipMemAddressFree(dptr, alignedSize);
    return nullptr;
  }

  // Track allocation for cleanup
  vmPtrs_.push_back(reinterpret_cast<void*>(dptr));
  vmSizes_.push_back(alignedSize);

  LOG(INFO) << fmt::format(
      "Allocated ROCm VM memory: ptr={}, size={} (aligned from {})",
      reinterpret_cast<void*>(dptr),
      alignedSize,
      size);

  return reinterpret_cast<void*>(dptr);
}

int RocmMemory::exportHandleAsDmabuf(hipMemGenericAllocationHandle_t handle) {
  hipError_t error;
  int dmabufFd = -1;

  error = hipMemExportToShareableHandle(
      &dmabufFd, handle, hipMemHandleTypePosixFileDescriptor, 0);
  if (error != hipSuccess) {
    LOG(ERROR) << fmt::format(
        "hipMemExportToShareableHandle failed: {} ({})",
        hipGetErrorString(error),
        static_cast<int>(error));
    return -1;
  }

  LOG(INFO) << fmt::format("Exported GPU memory as dmabuf fd {}", dmabufFd);
  return dmabufFd;
}
#endif // ifndef NO_ROCM

void RocmMemory::pushBlock(int buffer_cap) {
#ifndef NO_ROCM
  auto len = getLen(buffer_cap);

  // If VM APIs are available (granularity_ > 0), use them for dmabuf support
  // Otherwise fall back to regular hipMalloc
  void* d_A = nullptr;
  bool usingVMAllocation = false;

  if (granularity_ > 0) {
    hipMemGenericAllocationHandle_t handle;
    d_A = allocateVMMemory(len, handle);
    if (d_A != nullptr) {
      vmHandles_.push_back(handle);
      usingVMAllocation = true;
    }
  }

  // Fall back to regular hipMalloc if VM allocation failed or not available
  if (d_A == nullptr) {
    hipError_t error = hipMalloc(&d_A, len);
    if (error != hipSuccess) {
      LOG(FATAL) << fmt::format(
          "hipMalloc({}) failed with error: {} ({})",
          len,
          hipGetErrorString(error),
          static_cast<int>(error));
    }
    LOG(INFO) << fmt::format(
        "Allocated ROCm GPU buffer (regular) address at: {}, size: {}",
        d_A,
        len);
  }

  unsigned long base_offset = 0;
  for (int i = 0; i < params_.qp_cap; i++) {
    for (int j = 0; j < buffer_cap; j++) {
      auto c = static_cast<char*>(d_A);
      ctx()->gpu_buffers.push_back(
          static_cast<void*>(c + base_offset + (j * params_.message_size[i])));
    }
    base_offset += buffer_cap * params_.message_size[i];
  }

  // Only set freeBuffers for regular hipMalloc allocations
  // VM allocations are cleaned up in RocmMemory destructor
  if (!usingVMAllocation) {
    ctx()->freeBuffers = [](RcQpContext* ctx) {
      hipError_t err = hipFree(ctx->gpu_buffers[0]);
      if (err != hipSuccess) {
        LOG(WARNING) << fmt::format(
            "hipFree failed with error: {}", hipGetErrorString(err));
      }
    };
  } else {
    // No-op for VM allocations - cleanup happens in destructor
    ctx()->freeBuffers = [](RcQpContext*) {};
  }
#else
  (void)buffer_cap;
  LOG(FATAL) << "rdma_gen compiled without ROCm support";
#endif // ifndef NO_ROCM
}

void RocmMemory::pushBuffers(int buffer_cap) {
#ifndef NO_ROCM
  bool usingVMAllocation = (granularity_ > 0);

  for (int i = 0; i < params_.qp_cap; i++) {
    for (int j = 0; j < buffer_cap; j++) {
      const auto len = params_.message_size[i];
      void* d_A = nullptr;

      // If VM APIs are available, use them for dmabuf support
      if (granularity_ > 0) {
        hipMemGenericAllocationHandle_t handle;
        d_A = allocateVMMemory(len, handle);
        if (d_A != nullptr) {
          vmHandles_.push_back(handle);
        } else {
          usingVMAllocation = false;
        }
      }

      // Fall back to regular hipMalloc if VM allocation failed or not available
      if (d_A == nullptr) {
        hipError_t error = hipMalloc(&d_A, len);
        if (error != hipSuccess) {
          LOG(FATAL) << fmt::format(
              "hipMalloc({}) failed with error: {} ({})",
              len,
              hipGetErrorString(error),
              static_cast<int>(error));
        }
        LOG(INFO) << fmt::format(
            "Allocated ROCm GPU buffer (regular) address at: {}, size: {}",
            d_A,
            len);
      }

      ctx()->gpu_buffers.push_back(d_A);
    }
  }

  // Only set freeBuffers for regular hipMalloc allocations
  // VM allocations are cleaned up in RocmMemory destructor
  if (!usingVMAllocation) {
    ctx()->freeBuffers = [](RcQpContext* ctx) {
      for (auto* b : ctx->gpu_buffers) {
        hipError_t err = hipFree(b);
        if (err != hipSuccess) {
          LOG(WARNING) << fmt::format(
              "hipFree failed with error: {}", hipGetErrorString(err));
        }
      }
    };
  } else {
    // No-op for VM allocations - cleanup happens in destructor
    ctx()->freeBuffers = [](RcQpContext*) {};
  }
#else
  (void)buffer_cap;
  LOG(FATAL) << "rdma_gen compiled without ROCm support";
#endif // ifndef NO_ROCM
}

void RocmMemory::regBlock(int buffer_cap) {
#ifndef NO_ROCM
  auto len = getLen(buffer_cap);

  // Check if we have a VM handle for this allocation (dmabuf-capable)
  int dmabufFd = -1;
  if (!vmHandles_.empty()) {
    dmabufFd = exportHandleAsDmabuf(vmHandles_[0]);
  }

  if (dmabufFd >= 0) {
    dmabufFds_.push_back(dmabufFd);

    // Register the dmabuf with RDMA
    auto mr = ctx()->pd_adapter->registerDmabuf(
        0, // offset
        len, // length
        0, // iova (let the driver choose)
        dmabufFd,
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
            IBV_ACCESS_REMOTE_READ);

    LOG(INFO) << fmt::format(
        "Registered ROCm GPU memory block via dmabuf: fd={}, len={}",
        dmabufFd,
        len);

    ctx()->mr_list.emplace_back(std::move(mr));
  } else {
    // Fallback to direct ibv_reg_mr
    LOG(WARNING) << "dmabuf export not available, falling back to ibv_reg_mr";
    auto mr = ctx()->pd_adapter->registerMemory(
        ctx()->gpu_buffers[0], len, accessFlags_);
    ctx()->mr_list.emplace_back(std::move(mr));
  }
#else
  (void)buffer_cap;
  LOG(FATAL) << "rdma_gen compiled without ROCm support";
#endif
}

void RocmMemory::regBuffers(int buffer_cap) {
#ifndef NO_ROCM
  size_t vmHandleIdx = 0;

  for (int i = 0; i < params_.qp_cap; i++) {
    for (int j = 0; j < buffer_cap; j++) {
      auto idx = i * buffer_cap + j;
      size_t len = params_.message_size[i];

      // Check if we have a VM handle for this allocation (dmabuf-capable)
      int dmabufFd = -1;
      if (vmHandleIdx < vmHandles_.size()) {
        dmabufFd = exportHandleAsDmabuf(vmHandles_[vmHandleIdx]);
        vmHandleIdx++;
      }

      if (dmabufFd >= 0) {
        dmabufFds_.push_back(dmabufFd);

        // Register the dmabuf with RDMA
        auto mr = ctx()->pd_adapter->registerDmabuf(
            0, // offset
            len, // length
            0, // iova (let the driver choose)
            dmabufFd,
            IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                IBV_ACCESS_REMOTE_READ);

        LOG(INFO) << fmt::format(
            "Registered ROCm GPU buffer {} via dmabuf: fd={}, len={}",
            idx,
            dmabufFd,
            len);

        ctx()->mr_list.emplace_back(std::move(mr));
      } else {
        // Fallback to direct ibv_reg_mr
        LOG(WARNING) << "dmabuf export not available for buffer " << idx
                     << ", falling back to ibv_reg_mr";
        auto mr = ctx()->pd_adapter->registerMemory(
            ctx()->gpu_buffers[idx], len, accessFlags_);
        ctx()->mr_list.emplace_back(std::move(mr));
      }
    }
  }
#else
  (void)buffer_cap;
  LOG(FATAL) << "rdma_gen compiled without ROCm support";
#endif
}

} // namespace facebook::neteng::ai::rdma_gen
