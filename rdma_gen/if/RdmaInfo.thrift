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

namespace cpp2 facebook.neteng.ai.rdma_gen.thrift
namespace py3 facebook.neteng.ai.rdma_gen.thrift
include "thrift/annotation/cpp.thrift"
include "thrift/annotation/thrift.thrift"

@thrift.AllowLegacyMissingUris
package;

@cpp.Type{name = "uint64_t"}
typedef i64 ui64

@cpp.Type{name = "uint32_t"}
typedef i32 ui32

@cpp.Type{name = "uint16_t"}
typedef i16 ui16

// Identifies the interface/port;
// subnetPrefix is usually MAC address;
// intefaceId is usually set to 0
struct GlobalId {
  1: ui64 subnetPrefix; // union ibv_gid.subnet_prefix
  2: ui64 interfaceId; // union ibv_gid.interface_id
}

// Not shared for send/receive
struct MemoryRegionInfo {
  1: ui64 addr; // struct ibv_mr addr
  2: ui64 length; // struct ibv_mr addr
  3: ui32 rkey; // struct ibv_mr.rkey (remote access key) -- verbs backends only
  4: ui32 lkey; // struct ibv_mr.lkey (local access key)  -- verbs backends only
  // The fields below carry 64-bit MR keys for libfabric backends, where
  // fi_mr_key()/fi_mr_desc() return values that do not fit in 32 bits.
  // Verbs builds leave them at zero; libfabric builds populate them
  // alongside the legacy 32-bit fields above (which are zero in libfabric mode).
  5: ui64 fabric_rkey;
  6: ui64 fabric_lkey;
}

struct QpInfo {
  1: ui32 qpNum; // struct ibv_qp.qp_num
  2: ui32 psn; // Packet serial number (24bit unsigned value)
  3: ui32 customId; // Application-specific identifier for this QP
}

// See `enum ibv_mtu` in third-party/rdma-core/stablev53/libibverbs/verbs.h
enum Mtu {
  UNKNOWN = 0,
  MTU_256 = 1,
  MTU_512 = 2,
  MTU_1024 = 3,
  MTU_2048 = 4,
  MTU_4096 = 5,
  MTU_8192 = 6,
}

struct PortInfo {
  1: ui16 portNum; // (8-bit unsigned value)
  2: GlobalId gid; // Global identifier: ibv_query_gid()
  3: Mtu activeMtu; // struct ibv_port_attr.active_mtu
  4: ui16 lid; // Local identifier: struct ibv_port_attr.lid
  @cpp.Type{template = "std::vector"}
  5: list<QpInfo> qpInfos;
}

struct RdmaDeviceInfo {
  1: string deviceName;
  2: list<MemoryRegionInfo> mrInfos;
  @cpp.Type{template = "std::vector"}
  3: list<PortInfo> portInfos;
  4: ui32 customId;
}

struct RdmaInfo {
  @cpp.Type{template = "std::vector"}
  1: list<RdmaDeviceInfo> deviceInfos; # Map between device name and DeviceInfo
  2: string hostname;
}

struct MemoryRange {
  1: ui64 address;
  2: ui64 size;
  3: bool isCacheable;
}

@thrift.DeprecatedUnvalidatedAnnotations{
  items = {
    "cpp.adapter": "::facebook::neteng::ai::rdma_gen::AllocationRequirementsAdapter",
  },
}
typedef map<ui32, list<MemoryRange>> AllocationRequirements

@thrift.DeprecatedUnvalidatedAnnotations{
  items = {
    "cpp.adapter": "::facebook::neteng::ai::rdma_gen::AllocationResultAdapter",
  },
}
typedef map<ui64, MemoryRange> AllocationResult

struct EroceQpCfg {
  1: binary eroce_hw_cfg;
}

struct RcQpInfo {
  1: ui16 lid = 0;
  2: i32 qpn = 0;
  3: ui32 psn = 0;
  @cpp.Type{template = "std::vector"}
  4: list<GlobalId> gids;
  5: optional EroceQpCfg eroceQpCfg;
}

// Request/Response structures for ExchangeQpInfo
struct ExchangeQpInfoRequest {
  1: RcQpInfo qpInfo;
}

struct ExchangeQpInfoResponse {
  1: bool status = false;
  2: RcQpInfo qpInfo;
}

// Request/Response structures for Termination
struct TerminationRequest {
  1: i32 qpn = 0;
}

struct TerminationResponse {
  1: bool status = false;
}

// Request/Response structures for CompletionBarrier.
// The client blocks on completionBarrier after its own TX completions are done,
// and the server only responds once it has drained all expected RX completions.
// This prevents the asymmetric fire-and-forget teardown race where the server's
// RX poll thread is stopped by terminate while the last completions are still
// un-polled.
struct CompletionBarrierRequest {
  1: i32 qpn;
  2: ui64 clientCompleted;
}

struct CompletionBarrierResponse {
  1: bool status;
  2: ui64 serverCompleted;
}

// Request/Response structures for Write
struct WriteRequest {
  1: RcQpInfo qpInfo;
  2: ui32 writeLength;
}

struct WriteResponse {
  1: bool status = false;
  2: RcQpInfo qpInfo;
  3: ui64 writeAddr = 0;
  4: ui32 rkey = 0;
}

// Request/Response structures for Read
struct ReadRequest {
  1: RcQpInfo qpInfo;
  2: ui32 readLength;
}

struct ReadResponse {
  1: bool status = false;
  2: RcQpInfo qpInfo;
  3: ui64 readAddr = 0;
  4: ui32 rkey = 0;
}

// Request/Response structures for MetadataExchange
struct MetadataExchangeRequest {
  1: map<string, string> metadata;
}

struct MetadataExchangeResponse {
  1: bool status = false;
  2: map<string, string> metadata;
}

// Service interface for RdmaGen control operations
service RdmaGenControlService {
  ExchangeQpInfoResponse exchangeQpInfo(1: ExchangeQpInfoRequest request);
  TerminationResponse terminate(1: TerminationRequest request);
  CompletionBarrierResponse completionBarrier(
    1: CompletionBarrierRequest request,
  );
  WriteResponse write(1: WriteRequest request);
  ReadResponse read(1: ReadRequest request);
  MetadataExchangeResponse exchangeMetadata(1: MetadataExchangeRequest request);
}
