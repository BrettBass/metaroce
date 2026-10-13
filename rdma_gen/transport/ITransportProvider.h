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

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifdef RDMA_GEN_OSS
#include "rdma_gen/transport/IRdmaVerbs.h"
#include "rdma_gen/transport/RdmaGenVerbsMode.h"
#else
#include "neteng/ai/common/IRdmaVerbs.h"
#include "rdma_gen/transport/RdmaGenVerbsMode.h"
#endif

#include <folly/CppAttributes.h>

namespace facebook::neteng::ai::rdma_gen {

struct ContextCreationParams {
  std::vector<uint8_t> ib_ports = {1}; // only valid for verbs devices
  std::vector<int32_t> preferred_gid_idx; // only valid for verbs devices
  std::vector<std::string> preferred_gid_addresses; // IPv6 addresses to resolve
  bool enable_muti_port{false};

  uint8_t max_rd{0};
  uint8_t min_rnr_timer{0};
  uint8_t rnr_retry{7};
  uint8_t retry_cnt{7};
  uint8_t service_level{0};

  // Packed transport sequence value. MetaRoCE interprets this as
  // CSN[31:16] | MSN[15:0].
  std::optional<uint32_t> psn;

  bool skip_ibv_query_qp_verification{false};
  bool allow_incorrect_queried_qp_state{false};

  VerbsMode::Value verbs_mode{VerbsMode::RdmaCore};
};

struct PortInfoAdapter {
  std::vector<int> gid_idx;
  std::vector<ibv_gid> gids;
  ibv_mtu active_mtu;
  uint8_t lid;
};

// Only the attributes the code uses are exposed.
struct DeviceAttributesAdapter {
  int max_qp_wr;
  int max_cqe;
  int max_mr;
};

struct PeerStateAdapter {
  ibv_qp_state qp_state;
};

enum class CompletionOpcode : uint8_t {
  Send,
  RdmaWrite,
  RdmaRead,
  CompSwap,
  FetchAdd,
  Recv,
  RecvRdmaWithImm,
};

struct WorkCompletionAdapter {
  uint64_t wr_id{0};
  uint32_t qp_num{0};
  uint32_t byte_len{0};
  CompletionOpcode opcode{};
  int status{0};
  const char* status_str{nullptr};
  std::optional<uint32_t> imm_data;
};

enum class SendOpcode : uint8_t {
  Invalid = 0,
  Send,
  SendWithImm,
  RdmaRead,
  RdmaWrite,
  RdmaWriteWithImm,
  AtomicCmpAndSwp,
  AtomicFetchAndAdd,
};

enum SendFlag : uint32_t {
  kNone = 0,
  kSignaled = 1 << 0,
  kInline = 1 << 1,
};

SendFlag convertVerbsSendFlags(uint32_t verbsFlags);

struct RdmaTarget {
  uint64_t remote_addr{0};
  uint64_t rkey{0};
};

struct AtomicTarget {
  uint64_t remote_addr{0};
  uint64_t rkey{0};
  uint64_t compare_add{0};
  uint64_t swap{0};
};

struct SendWorkRequestAdapter {
  uint64_t wr_id{0};
  SendOpcode opcode{SendOpcode::Invalid};
  SendFlag send_flags{SendFlag::kNone};

  uint64_t local_addr{0};
  uint32_t length{0};
  uint64_t lkey{0};

  std::optional<uint32_t> imm_data;

  union {
    RdmaTarget rdma;
    AtomicTarget atomic;
  } target{};
};

struct RecvWorkRequestAdapter {
  uint64_t wr_id{0};

  uint64_t local_addr{0};
  uint32_t length{0};
  uint64_t lkey{0};
};

struct PeerCreationParams {
  uint32_t scatter_gather_max{};
  bool use_qpt_driver{};
  bool use_selective_signaling{};
  uint32_t max_send_wr{};
  uint32_t max_recv_wr{};
  uint32_t max_inline_data{};
  unsigned int access_flags{};
  uint8_t sqp_per_qp = 1;
  uint8_t tx_path_count = 1;
  uint8_t rx_path_count = 1;
  std::optional<uint32_t> local_psn;
};

struct EroceQpCfg {
  std::string hw_cfg;
};

struct PeerConnectData {
  // Packed local starting sequence value advertised to the peer.
  uint32_t psn{};
  uint32_t qp_num{};

  std::optional<EroceQpCfg> eroceQpCfg;

  // these two are not exchanged between peers but
  // are passed in modifyRcQpsRts (when finishConnect is called)
  ibv_mtu mtu{IBV_MTU_4096};
  int traffic_class{0};
};

struct PortConnectData {
  uint16_t lid{};
  std::vector<ibv_gid> gids;
};

class ITransportPeer {
 public:
  ITransportPeer() = default;
  virtual ~ITransportPeer() = 0;

  ITransportPeer(const ITransportPeer&) = delete;
  ITransportPeer& operator=(const ITransportPeer&) = delete;
  ITransportPeer(ITransportPeer&&) = delete;
  ITransportPeer& operator=(ITransportPeer&&) = delete;

  virtual uint32_t getId() const = 0;
  virtual ibv_qp* getIbQpPtr() const = 0;

  virtual bool isConnected() const = 0;
  virtual PeerConnectData getConnectionData() const = 0;
  virtual bool finishConnect(
      const PeerConnectData& data,
      const PortConnectData& port_data,
      ibv_qp_state targetQpState) = 0;

  virtual PeerCreationParams queryCreationParams() const = 0;
  virtual PeerStateAdapter queryState() const = 0;

  virtual int postRecv(const RecvWorkRequestAdapter& wr) = 0;
  virtual int postSend(const SendWorkRequestAdapter& wr) = 0;

  virtual bool modifyRcQpInit(ibv_qp_state /*targetQpState*/) {
    return true;
  }
};

class ITransportMr {
 public:
  ITransportMr() = default;
  virtual ~ITransportMr() = 0;

  ITransportMr(const ITransportMr&) = delete;
  ITransportMr& operator=(const ITransportMr&) = delete;
  ITransportMr(ITransportMr&&) = delete;
  ITransportMr& operator=(ITransportMr&&) = delete;

  virtual void* addr() const = 0;
  virtual size_t length() const = 0;
  virtual uint64_t rkey() const = 0;
  virtual uint64_t lkey() const = 0;

  virtual ibv_mr* getIbMrPtr() const = 0;
  virtual IRdmaVerbs* getVerbs() const = 0;
};

class ITransportCq {
 public:
  ITransportCq() = default;
  virtual ~ITransportCq() = 0;

  ITransportCq(const ITransportCq&) = delete;
  ITransportCq& operator=(const ITransportCq&) = delete;
  ITransportCq(ITransportCq&&) = delete;
  ITransportCq& operator=(ITransportCq&&) = delete;

  virtual int poll(WorkCompletionAdapter* wcs, int max_entries) = 0;

  virtual ibv_cq* getIbCqPtr() const = 0;
  virtual IRdmaVerbs* getVerbs() const = 0;
};

class ITransportDomain {
 public:
  ITransportDomain() = default;
  virtual ~ITransportDomain() = 0;

  ITransportDomain(const ITransportDomain&) = delete;
  ITransportDomain& operator=(const ITransportDomain&) = delete;
  ITransportDomain(ITransportDomain&&) = delete;
  ITransportDomain& operator=(ITransportDomain&&) = delete;

  // access_flags currently using Verbs flags
  virtual std::unique_ptr<ITransportMr>
  registerMemory(void* buffer, size_t length, int access_flags) = 0;

  virtual std::unique_ptr<ITransportMr> registerDmabuf(
      uint64_t offset,
      size_t length,
      uint64_t iova,
      int fd,
      int access_flags) = 0;
  virtual std::unique_ptr<ITransportCq> createCq(
      int depth,
      ibv_comp_channel* channel) = 0;

  virtual std::unique_ptr<ITransportPeer> createPeer(
      const PeerCreationParams& attrs,
      ITransportCq* cq,
      ibv_qp_state targetQpState) = 0;

  virtual std::unique_ptr<ITransportPeer> createChildPeer(
      const PeerCreationParams& attrs,
      ITransportCq* cq,
      uint32_t parent_qpn,
      uint8_t sqp_id) = 0;

  // This is a temporary hack while we migrate the code
  virtual ibv_pd* getIbPdPtr() const = 0;
  virtual IRdmaVerbs* getVerbs() const = 0;
  virtual PortInfoAdapter getPortInfo() const = 0;
};

// A single asynchronous transport event surfaced to the async-event handler.
struct AsyncEventInfo {
  int eventType{0}; // provider-native event type code
  std::string description; // human-readable event name
  // Whether this event signals an error. Classified by the provider, since
  // eventType is drawn from a provider-native enum that consumers cannot
  // interpret.
  bool isError{false};
};

class ITransportContext {
 public:
  ITransportContext() = default;
  virtual ~ITransportContext() = 0;

  ITransportContext(const ITransportContext&) = delete;
  ITransportContext& operator=(const ITransportContext&) = delete;
  ITransportContext(ITransportContext&&) = delete;
  ITransportContext& operator=(ITransportContext&&) = delete;

  virtual bool isPortActive(uint8_t portNum) = 0;
  virtual DeviceAttributesAdapter queryAttributes() = 0;
  virtual std::unique_ptr<ITransportDomain> createDomain() = 0;

  // Async transport-event support. Providers that expose a native async event
  // channel (e.g. a verbs async fd) override these; providers without one keep
  // the defaults so callers simply skip async-event polling.
  virtual bool hasAsyncEvents() const {
    return false;
  }
  // Poll up to timeoutMs for the next async event. Returns std::nullopt on
  // timeout or when unsupported.
  virtual std::optional<AsyncEventInfo> pollAsyncEvent(int /*timeoutMs*/) {
    return std::nullopt;
  }

  // This is a temporary hack while we migrate the code
  virtual ibv_context* getIbContextPtr() const = 0;
  virtual IRdmaVerbs* getVerbs() const = 0;
};

class ITransportDevice {
 public:
  ITransportDevice() = default;
  virtual ~ITransportDevice() = 0;

  ITransportDevice(const ITransportDevice&) = delete;
  ITransportDevice& operator=(const ITransportDevice&) = delete;
  ITransportDevice(ITransportDevice&&) = delete;
  ITransportDevice& operator=(ITransportDevice&&) = delete;
  virtual std::string_view getName() const = 0;
  virtual bool isFabricDevice() const = 0;

  // This is a temporary hack while we migrate the code
  virtual ibv_device* getIbDevicePtr() const = 0;
  virtual IRdmaVerbs* getVerbs() const = 0;
  virtual std::unique_ptr<ITransportContext> createContext(
      const ContextCreationParams& settings) = 0;
};

class ITransportProvider {
 public:
  ITransportProvider() = default;
  virtual ~ITransportProvider() = 0;

  ITransportProvider(const ITransportProvider&) = delete;
  ITransportProvider& operator=(const ITransportProvider&) = delete;
  ITransportProvider(ITransportProvider&&) = delete;
  ITransportProvider& operator=(ITransportProvider&&) = delete;

  virtual std::vector<std::unique_ptr<ITransportDevice>> getDevices() = 0;
  virtual std::size_t getDeviceCount() = 0;
  virtual IRdmaVerbs* getVerbs() const = 0;
};

} // namespace facebook::neteng::ai::rdma_gen
