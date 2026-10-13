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

#include <memory>
#include <optional>
#include <vector>
#include "rdma_gen/transport/ITransportProvider.h"

namespace facebook::neteng::ai::rdma_gen {

class VerbsProvider;
class VerbsDevice;
class VerbsContext;
class VerbsDomain;
class VerbsPeer;
class VerbsPeerManager;

// Device-specific steps around a verbs peer's connection. One instance serves
// every peer of a provider, and its calls may come from different threads.
class VerbsPeerPolicy {
 public:
  virtual ~VerbsPeerPolicy() = default;
  // Runs once the QP reaches RTR; returning false fails the connection.
  virtual bool afterRtr(
      ibv_qp* qp,
      const PortInfoAdapter& local_port_info,
      const PortConnectData& remote_port_info,
      const ContextCreationParams& settings) = 0;
  // Runs before the QP is freed, whether or not it connected.
  virtual void beforeDestroy(ibv_qp* qp) noexcept = 0;
};

// VerbsPeer wraps an ibv_qp* as a peer.
// The unique ID is the QP number (qp_num) assigned by the driver.
class VerbsPeer : public ITransportPeer {
  ibv_qp* qp_;
  IRdmaVerbs* verbs_;
  bool is_connected_{false};
  bool is_child_sqp_{false};
  PeerConnectData conn_data_;
  int creation_index_;
  unsigned int access_flags_;
  PortInfoAdapter port_info_;
  ContextCreationParams ctx_settings_;
  std::shared_ptr<VerbsPeerPolicy> policy_;

  void generateConnectData();
  ibv_ah_attr buildAddressHandle(
      const union ibv_gid& remote_gid,
      uint16_t remote_lid,
      int local_gid_idx,
      int tc);
  bool addMultiPortPaths(const PortConnectData& remote_port_info, int tc);
  bool modifyRcQpErr();
  bool confirmQpModifiedToState(
      int modifyQpRetval,
      enum ibv_qp_state desiredState,
      std::string desiredStateName);

 public:
  explicit VerbsPeer(
      ibv_qp* qp,
      const PortInfoAdapter& port_info,
      const ContextCreationParams& ctx_settings_,
      int creation_index,
      unsigned int access_flags,
      IRdmaVerbs* verbs,
      std::shared_ptr<VerbsPeerPolicy> policy = nullptr);
  ~VerbsPeer() override;

  VerbsPeer(const VerbsPeer&) = delete;
  VerbsPeer& operator=(const VerbsPeer&) = delete;
  VerbsPeer(VerbsPeer&&) = delete;
  VerbsPeer& operator=(VerbsPeer&&) = delete;

  bool isConnected() const override;
  PeerConnectData getConnectionData() const override;
  bool finishConnect(
      const PeerConnectData& data,
      const PortConnectData& port_data,
      ibv_qp_state targetQpState) override;
  PeerCreationParams queryCreationParams() const override;
  PeerStateAdapter queryState() const override;
  int postRecv(const RecvWorkRequestAdapter& wr) override;
  int postSend(const SendWorkRequestAdapter& wr) override;

  uint32_t getId() const override;
  ibv_qp* getIbQpPtr() const override;
  void setChildSqp() {
    is_child_sqp_ = true;
  }
  bool isChildSqp() const {
    return is_child_sqp_;
  }

  bool modifyRcQpsRts(
      const PeerConnectData& remote_peer_info,
      const PortConnectData& remote_port_info,
      int qp_cos,
      enum ibv_mtu mtu,
      enum ibv_qp_state targetQpState);
  bool modifyRcQpRTR(
      const PeerConnectData& remote_peer_info,
      const PortConnectData& remote_port_info,
      int tc,
      enum ibv_mtu mtu);

  bool modifyRcQpRTS();
  bool modifyRcQpInit(ibv_qp_state targetQpState) override;
};

class VerbsMr : public ITransportMr {
  ibv_mr* mr_;
  VerbsDomain* domain_;

 public:
  explicit VerbsMr(ibv_mr* mr, VerbsDomain* domain);
  ~VerbsMr() override;

  VerbsMr(const VerbsMr&) = delete;
  VerbsMr& operator=(const VerbsMr&) = delete;
  VerbsMr(VerbsMr&&) = delete;
  VerbsMr& operator=(VerbsMr&&) = delete;

  void* addr() const override;
  size_t length() const override;
  uint64_t lkey() const override;
  uint64_t rkey() const override;

  ibv_mr* getIbMrPtr() const override;
  IRdmaVerbs* getVerbs() const override;
};

class VerbsCq : public ITransportCq {
  ibv_cq* cq_;
  IRdmaVerbs* verbs_;

 public:
  explicit VerbsCq(ibv_cq* cq, IRdmaVerbs* verbs);
  ~VerbsCq() override;

  VerbsCq(const VerbsCq&) = delete;
  VerbsCq& operator=(const VerbsCq&) = delete;
  VerbsCq(VerbsCq&&) = delete;
  VerbsCq& operator=(VerbsCq&&) = delete;

  int poll(WorkCompletionAdapter* wcs, int max_entries) override;
  ibv_cq* getIbCqPtr() const override;
  IRdmaVerbs* getVerbs() const override;
};

class VerbsDomain : public ITransportDomain {
  ibv_pd* pd_;
  ibv_context* context_;
  IRdmaVerbs* verbs_;
  PortInfoAdapter port_info_;
  ContextCreationParams ctx_settings_;
  std::shared_ptr<VerbsPeerPolicy> peer_policy_;
  int peer_creation_idx_{0};

  ibv_qp* createTaggedQp(
      const PeerCreationParams& peerAttributes,
      ITransportCq* cq,
      uint32_t source_qpn);

 public:
  explicit VerbsDomain(ibv_pd* pd, VerbsContext* context);
  ~VerbsDomain() override;

  VerbsDomain(const VerbsDomain&) = delete;
  VerbsDomain& operator=(const VerbsDomain&) = delete;
  VerbsDomain(VerbsDomain&&) = delete;
  VerbsDomain& operator=(VerbsDomain&&) = delete;

  std::unique_ptr<ITransportCq> createCq(int depth, ibv_comp_channel* channel)
      override;

  std::unique_ptr<ITransportMr>
  registerMemory(void* addr, size_t length, int access_flags) override;

  std::unique_ptr<ITransportMr> registerDmabuf(
      uint64_t offset,
      size_t length,
      uint64_t iova,
      int fd,
      int access_flags) override;

  std::unique_ptr<ITransportPeer> createPeer(
      const PeerCreationParams& attrs,
      ITransportCq* cq,
      ibv_qp_state targetQpState) override;

  std::unique_ptr<ITransportPeer> createChildPeer(
      const PeerCreationParams& attrs,
      ITransportCq* cq,
      uint32_t parent_qpn,
      uint8_t sqp_id) override;

  ibv_pd* getIbPdPtr() const override;
  IRdmaVerbs* getVerbs() const override;
  PortInfoAdapter getPortInfo() const override;
};

class VerbsContext : public ITransportContext {
  ibv_context* context_;
  VerbsDevice* device_;
  IRdmaVerbs* verbs_;
  PortInfoAdapter port_info_{};
  ContextCreationParams ctx_settings_;
  bool asyncFdNonBlocking_{false};
  bool asyncFdSetupFailed_{false};

  uint32_t findBestGid(uint8_t ib_port);
  std::vector<int32_t> resolveGidAddresses(
      uint8_t ib_port,
      const std::vector<std::string>& addresses);

 public:
  explicit VerbsContext(
      ibv_context* context,
      const ContextCreationParams& settings,
      VerbsDevice* device);
  ~VerbsContext() override;

  VerbsContext(const VerbsContext&) = delete;
  VerbsContext& operator=(const VerbsContext&) = delete;
  VerbsContext(VerbsContext&&) = delete;
  VerbsContext& operator=(VerbsContext&&) = delete;

  bool isPortActive(uint8_t portNum) override;
  DeviceAttributesAdapter queryAttributes() override;

  bool hasAsyncEvents() const override;
  std::optional<AsyncEventInfo> pollAsyncEvent(int timeoutMs) override;

  ibv_context* getIbContextPtr() const override;
  IRdmaVerbs* getVerbs() const override;
  std::unique_ptr<ITransportDomain> createDomain() override;

  const ContextCreationParams& getCreationParams() const;
  const PortInfoAdapter& getPortInfo() const;
  const std::shared_ptr<VerbsPeerPolicy>& getPeerPolicy() const;
};

class VerbsDevice : public ITransportDevice {
  ibv_device* device_;
  VerbsProvider* adapter_;
  IRdmaVerbs* verbs_;
  std::string name_;

 public:
  explicit VerbsDevice(ibv_device* device, VerbsProvider* adapter);
  ~VerbsDevice() override;

  VerbsDevice(const VerbsDevice&) = delete;
  VerbsDevice& operator=(const VerbsDevice&) = delete;
  VerbsDevice(VerbsDevice&&) = delete;
  VerbsDevice& operator=(VerbsDevice&&) = delete;

  std::string_view getName() const override;
  std::unique_ptr<ITransportContext> createContext(
      const ContextCreationParams& settings) override;
  bool isFabricDevice() const override;

  ibv_device* getIbDevicePtr() const override;
  IRdmaVerbs* getVerbs() const override;
  const std::shared_ptr<VerbsPeerPolicy>& getPeerPolicy() const;
};

class VerbsProvider : public ITransportProvider {
  std::shared_ptr<IRdmaVerbs> verbs_;
  std::shared_ptr<VerbsPeerPolicy> peer_policy_;
  int device_count_ = -1;
  ibv_device** ibv_device_ptr_ = nullptr;

 public:
  explicit VerbsProvider(
      std::shared_ptr<IRdmaVerbs> verbs,
      std::shared_ptr<VerbsPeerPolicy> peer_policy = nullptr);
  ~VerbsProvider() override;

  VerbsProvider(const VerbsProvider&) = delete;
  VerbsProvider& operator=(const VerbsProvider&) = delete;
  VerbsProvider(VerbsProvider&&) = delete;
  VerbsProvider& operator=(VerbsProvider&&) = delete;

  std::vector<std::unique_ptr<ITransportDevice>> getDevices() override;
  std::size_t getDeviceCount() override;

  IRdmaVerbs* getVerbs() const override;
  const std::shared_ptr<VerbsPeerPolicy>& getPeerPolicy() const;
};

} // namespace facebook::neteng::ai::rdma_gen
