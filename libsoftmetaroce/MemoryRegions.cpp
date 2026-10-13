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

#include <glog/logging.h>

#include <libsoftmetaroce/MemoryRegions.h>
#include <limits>

namespace libsoftmetaroce {
namespace {

// Called with the registry read-locked. Null where the key matches no region
// of pd, which an unknown key and another domain's key both come to.
const RegisteredMemoryRegion* FOLLY_NULLABLE regionUnder(
    const MemoryRegionsMap& regions,
    const struct ibv_pd* pd,
    uint32_t key) {
  const auto it = regions.find(key);
  if (it == regions.end()) {
    LOG(ERROR) << "Memory region validation failed key check; key:" << key
               << " is not among our registered memory regions";
    return nullptr;
  }
  if (it->second->mr.pd != pd) {
    LOG(ERROR) << "Memory region validation failed protection domain check. "
               << "key:" << key << " matches a region of another domain";
    return nullptr;
  }
  return it->second.get();
}

// A zero-based region is addressed from its own base, so the wire value is an
// offset into it. An offset that overflows when added to the base resolves
// to no address.
std::optional<uint64_t> resolvedFromWire(
    const RegisteredMemoryRegion& region,
    uint64_t addr) {
  if ((region.access & IBV_ACCESS_ZERO_BASED) == 0) {
    return addr;
  }
  const auto base = reinterpret_cast<uint64_t>(region.mr.addr);
  if (addr > std::numeric_limits<uint64_t>::max() - base) {
    LOG(ERROR) << "Memory region resolution failed zero-based check. key:"
               << region.mr.rkey << " offset:" << std::hex << addr
               << " cannot be added to base " << region.mr.addr << std::dec;
    return std::nullopt;
  }
  return base + addr;
}

bool regionPermits(
    const RegisteredMemoryRegion& region,
    uint32_t key,
    uint64_t address,
    uint32_t length,
    int requiredAccess) {
  const auto base = reinterpret_cast<uint64_t>(region.mr.addr);
  // The length comes off the region end, so an address near the top of the
  // space cannot wrap past it into the region.
  if (address < base || length > region.mr.length ||
      address > base + (region.mr.length - length)) {
    LOG(ERROR) << "Memory region validation failed range check. key:" << key
               << " destination addr:" << std::hex << address
               << " length:" << std::dec << length << " while range is "
               << std::hex << region.mr.addr << " len:" << std::dec
               << region.mr.length;
    return false;
  }
  if ((region.access & requiredAccess) != requiredAccess) {
    LOG(ERROR) << "Memory region validation failed access check. key:" << key
               << " needs access 0x" << std::hex << requiredAccess
               << " but is registered with 0x" << region.access << std::dec;
    return false;
  }
  return true;
}

} // namespace

MemoryRegions::RemoteAtomicTarget MemoryRegions::targetForRemoteAtomic(
    const struct ibv_pd* pd,
    uint64_t addr,
    uint32_t rkey) const {
  const auto lockedRegions = memoryRegions_.rlock();
  const auto* const region = regionUnder(*lockedRegions, pd, rkey);
  if (region == nullptr) {
    return {.address = addr, .permitted = false};
  }
  const auto resolved = resolvedFromWire(*region, addr);
  if (!resolved.has_value()) {
    return {.address = addr, .permitted = false};
  }
  return {
      .address = *resolved,
      .permitted = regionPermits(
          *region,
          rkey,
          *resolved,
          kAtomicOperationSizeBytes,
          IBV_ACCESS_REMOTE_ATOMIC)};
}

bool MemoryRegions::isValidLocal(
    const struct ibv_pd* pd,
    char* address,
    uint32_t length,
    uint32_t key,
    int requiredAccess) const {
  return checkedAddress(
             pd,
             reinterpret_cast<uint64_t>(address),
             length,
             key,
             requiredAccess,
             AddressKind::Virtual)
      .has_value();
}

std::optional<uint64_t> MemoryRegions::checkedAddress(
    const struct ibv_pd* pd,
    uint64_t addr,
    uint32_t length,
    uint32_t key,
    int requiredAccess,
    AddressKind kind) const {
  // A zero-length access touches no memory, so no region or key governs it.
  if (length == 0) {
    return addr;
  }
  const auto lockedRegions = memoryRegions_.rlock();
  const auto* const region = regionUnder(*lockedRegions, pd, key);
  if (region == nullptr) {
    return std::nullopt;
  }
  const std::optional<uint64_t> resolved = kind == AddressKind::FromTheWire
      ? resolvedFromWire(*region, addr)
      : std::optional<uint64_t>{addr};
  if (!resolved.has_value() ||
      !regionPermits(*region, key, *resolved, length, requiredAccess)) {
    return std::nullopt;
  }
  return resolved;
}

struct ibv_mr* FOLLY_NULLABLE MemoryRegions::reg_mr(
    struct ibv_pd* pd,
    void* addr,
    size_t length,
    int access) {
  // A region ending past the top of the space gives every range check below
  // no end to measure an address against.
  if (length >
      std::numeric_limits<uint64_t>::max() - reinterpret_cast<uint64_t>(addr)) {
    return nullptr;
  }
  // Remote write and remote atomic both write into the region, so either
  // requires local write beside it. A region registered without it could not
  // serve the access its own flags grant.
  const bool writesTheRegionRemotely =
      (access & (IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC)) != 0;
  if (writesTheRegionRemotely && (access & IBV_ACCESS_LOCAL_WRITE) == 0) {
    LOG(ERROR) << "reg_mr asks for remote write or remote atomic without local "
               << "write; access 0x" << std::hex << access << std::dec;
    return nullptr;
  }
  // start with key == 1 to separate from uninitialized 0.
  // One hold for the key and the insert that uses it, so two registrations
  // cannot be handed the same one.
  auto lockedRegions = memoryRegions_.wlock();
  const uint32_t key = ++last_key_;

  auto stored = std::make_unique<RegisteredMemoryRegion>(RegisteredMemoryRegion{
      .mr =
          {.context = nullptr,
           .pd = pd,
           .addr = addr,
           .length = length,
           .handle = key,
           .lkey = key,
           .rkey = key},
      .access = access});
  struct ibv_mr* ret = &stored->mr;
  (*lockedRegions)[key] = std::move(stored);
  return ret;
}
} // namespace libsoftmetaroce
