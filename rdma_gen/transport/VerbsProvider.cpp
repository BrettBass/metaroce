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

#include "rdma_gen/transport/VerbsProvider.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <fmt/format.h>
#include <folly/String.h>
#include <glog/logging.h>
#include <poll.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <set>
#include "rdma_gen/Common.h"

namespace facebook::neteng::ai::rdma_gen {

VerbsPeer::VerbsPeer(
    ibv_qp* qp,
    const PortInfoAdapter& port_info,
    const ContextCreationParams& ctx_settings,
    int creation_index,
    unsigned int access_flags,
    IRdmaVerbs* verbs,
    std::shared_ptr<VerbsPeerPolicy> policy)
    : qp_(qp),
      verbs_(verbs),
      conn_data_(),
      creation_index_(creation_index),
      access_flags_(access_flags),
      port_info_(port_info),
      ctx_settings_(ctx_settings),
      policy_(std::move(policy)) {
  generateConnectData();
}

VerbsPeer::~VerbsPeer() {
  if (qp_) {
    if (policy_) {
      policy_->beforeDestroy(qp_);
    }
    verbs_->ibv_destroy_qp(qp_);
  }
}

bool VerbsPeer::isConnected() const {
  return is_connected_;
}

PeerConnectData VerbsPeer::getConnectionData() const {
  return conn_data_;
}

bool VerbsPeer::finishConnect(
    const PeerConnectData& data,
    const PortConnectData& port_data,
    ibv_qp_state targetQpState) {
  if (is_child_sqp_) {
    return true;
  }
  return modifyRcQpsRts(
      data, port_data, data.traffic_class, data.mtu, targetQpState);
}

PeerCreationParams VerbsPeer::queryCreationParams() const {
  ibv_qp_init_attr attr{};
  struct ibv_qp_attr pre_attr{};

  int res = verbs_->ibv_query_qp(qp_, &pre_attr, IBV_QP_CAP, &attr);
  if (res != 0) {
    LOG(FATAL) << fmt::format(
        "Failed to query queue pair {} with error {}",
        qp_->qp_num,
        folly::errnoStr(errno));
  }
  return PeerCreationParams{
      .scatter_gather_max = attr.cap.max_send_sge,
      .use_qpt_driver = attr.qp_type == IBV_QPT_DRIVER,
      .use_selective_signaling = attr.sq_sig_all == 0,
      .max_send_wr = attr.cap.max_send_wr,
      .max_recv_wr = attr.cap.max_recv_wr,
      .max_inline_data = attr.cap.max_inline_data,
      .access_flags = access_flags_,
  };
}

PeerStateAdapter VerbsPeer::queryState() const {
  struct ibv_qp_attr attr{};
  struct ibv_qp_init_attr init_attr{};
  int res = verbs_->ibv_query_qp(qp_, &attr, IBV_QP_STATE, &init_attr);
  if (res != 0) {
    LOG(FATAL) << fmt::format(
        "Failed to query queue pair {} state with error {}",
        qp_->qp_num,
        folly::errnoStr(errno));
  }
  return PeerStateAdapter{.qp_state = attr.qp_state};
}

namespace {

uint32_t checkedNarrow(uint64_t value, const char* field_name) {
  if (value > std::numeric_limits<uint32_t>::max()) {
    LOG(FATAL) << fmt::format(
        "Value 0x{:x} for '{}' exceeds uint32_t range", value, field_name);
  }
  return static_cast<uint32_t>(value);
}

ibv_wr_opcode toVerbsOpcode(SendOpcode opcode) {
  switch (opcode) {
    case SendOpcode::Send:
      return IBV_WR_SEND;
    case SendOpcode::SendWithImm:
      return IBV_WR_SEND_WITH_IMM;
    case SendOpcode::RdmaRead:
      return IBV_WR_RDMA_READ;
    case SendOpcode::RdmaWrite:
      return IBV_WR_RDMA_WRITE;
    case SendOpcode::RdmaWriteWithImm:
      return IBV_WR_RDMA_WRITE_WITH_IMM;
    case SendOpcode::AtomicCmpAndSwp:
      return IBV_WR_ATOMIC_CMP_AND_SWP;
    case SendOpcode::AtomicFetchAndAdd:
      return IBV_WR_ATOMIC_FETCH_AND_ADD;
    case SendOpcode::Invalid:
      LOG(FATAL) << fmt::format(
          "Invalid SendOpcode ({})", static_cast<int>(opcode));
      __builtin_unreachable();
  }
  LOG(FATAL) << fmt::format(
      "Unrecognized SendOpcode ({})", static_cast<int>(opcode));
  __builtin_unreachable();
}

unsigned int toVerbsSendFlags(uint32_t flags) {
  constexpr uint32_t kKnownFlags = kSignaled | kInline;
  uint32_t unknown = flags & ~kKnownFlags;
  if (unknown != 0) {
    LOG(FATAL) << fmt::format("Unrecognized send flags: 0x{:x}", unknown);
  }

  unsigned int result = 0;
  if (flags & kSignaled) {
    result |= IBV_SEND_SIGNALED;
  }
  if (flags & kInline) {
    result |= IBV_SEND_INLINE;
  }
  return result;
}

CompletionOpcode fromVerbsWcOpcode(ibv_wc_opcode opcode) {
  switch (opcode) {
    case IBV_WC_SEND:
      return CompletionOpcode::Send;
    case IBV_WC_RDMA_WRITE:
      return CompletionOpcode::RdmaWrite;
    case IBV_WC_RDMA_READ:
      return CompletionOpcode::RdmaRead;
    case IBV_WC_COMP_SWAP:
      return CompletionOpcode::CompSwap;
    case IBV_WC_FETCH_ADD:
      return CompletionOpcode::FetchAdd;
    case IBV_WC_RECV:
      return CompletionOpcode::Recv;
    case IBV_WC_RECV_RDMA_WITH_IMM:
      return CompletionOpcode::RecvRdmaWithImm;
    default:
      LOG(FATAL) << fmt::format(
          "Unrecognized ibv_wc_opcode ({})", static_cast<int>(opcode));
      __builtin_unreachable();
  }
}

WorkCompletionAdapter fromVerbsWc(const ibv_wc& wc) {
  WorkCompletionAdapter adapter{};
  adapter.wr_id = wc.wr_id;
  adapter.qp_num = wc.qp_num;
  adapter.byte_len = wc.byte_len;
  adapter.opcode = fromVerbsWcOpcode(static_cast<ibv_wc_opcode>(wc.opcode));
  adapter.status = wc.status;
  adapter.status_str = ibv_wc_status_str(static_cast<ibv_wc_status>(wc.status));
  if (wc.opcode == IBV_WC_RECV_RDMA_WITH_IMM ||
      (wc.opcode == IBV_WC_RECV && (wc.wc_flags & IBV_WC_WITH_IMM))) {
    adapter.imm_data = ntohl(wc.imm_data);
  }
  return adapter;
}

} // namespace

int VerbsPeer::postRecv(const RecvWorkRequestAdapter& wr) {
  ibv_sge sge{
      .addr = wr.local_addr,
      .length = wr.length,
      .lkey = checkedNarrow(wr.lkey, "lkey"),
  };

  ibv_recv_wr recv_wr{};
  recv_wr.wr_id = wr.wr_id;
  recv_wr.next = nullptr;
  recv_wr.sg_list = &sge;
  recv_wr.num_sge = 1;

  ibv_recv_wr* bad_wr;
  return verbs_->ibv_post_recv(qp_, &recv_wr, &bad_wr);
}

int VerbsPeer::postSend(const SendWorkRequestAdapter& wr) {
  ibv_sge sge{
      .addr = wr.local_addr,
      .length = wr.length,
      .lkey = checkedNarrow(wr.lkey, "lkey"),
  };

  ibv_send_wr send_wr{};
  send_wr.wr_id = wr.wr_id;
  send_wr.next = nullptr;
  send_wr.sg_list = &sge;
  send_wr.num_sge = 1;
  send_wr.opcode = toVerbsOpcode(wr.opcode);
  send_wr.send_flags = toVerbsSendFlags(wr.send_flags);

  bool expectsImm = wr.opcode == SendOpcode::SendWithImm ||
      wr.opcode == SendOpcode::RdmaWriteWithImm;
  CHECK_EQ(wr.imm_data.has_value(), expectsImm) << fmt::format(
      "imm_data presence ({}) inconsistent with opcode ({})",
      wr.imm_data.has_value(),
      static_cast<int>(wr.opcode));

  if (wr.imm_data.has_value()) {
    send_wr.imm_data = htonl(wr.imm_data.value());
  }

  switch (wr.opcode) {
    case SendOpcode::RdmaRead:
    case SendOpcode::RdmaWrite:
    case SendOpcode::RdmaWriteWithImm:
      send_wr.wr.rdma.remote_addr = wr.target.rdma.remote_addr;
      send_wr.wr.rdma.rkey = checkedNarrow(wr.target.rdma.rkey, "rdma.rkey");
      break;
    case SendOpcode::AtomicCmpAndSwp:
    case SendOpcode::AtomicFetchAndAdd:
      send_wr.wr.atomic.remote_addr = wr.target.atomic.remote_addr;
      send_wr.wr.atomic.rkey =
          checkedNarrow(wr.target.atomic.rkey, "atomic.rkey");
      send_wr.wr.atomic.compare_add = wr.target.atomic.compare_add;
      send_wr.wr.atomic.swap = wr.target.atomic.swap;
      break;
    default:
      break;
  }

  ibv_send_wr* bad_wr;
  return verbs_->ibv_post_send(qp_, &send_wr, &bad_wr);
}

uint32_t VerbsPeer::getId() const {
  return qp_->qp_num;
}

ibv_qp* VerbsPeer::getIbQpPtr() const {
  return qp_;
}

void VerbsPeer::generateConnectData() {
  conn_data_.qp_num = qp_->qp_num;

  // Mask PSN to 24 bits to match rdma_core's modify_qp behavior
  conn_data_.psn = (ctx_settings_.psn.has_value()
                        ? ctx_settings_.psn.value() + creation_index_
                        : lrand48()) &
      0xFFFFFF;
}

bool VerbsPeer::modifyRcQpsRts(
    const PeerConnectData& remote_peer_info,
    const PortConnectData& remote_port_info,
    int qp_cos,
    enum ibv_mtu mtu,
    enum ibv_qp_state targetQpState) {
  if (targetQpState < IBV_QPS_RTR) {
    return true;
  }

  if (!modifyRcQpRTR(remote_peer_info, remote_port_info, qp_cos, mtu)) {
    return false;
  }
  if (targetQpState == IBV_QPS_RTR) {
    return true;
  }

  if (!modifyRcQpRTS()) {
    return false;
  }

  return true;
}

bool VerbsPeer::confirmQpModifiedToState(
    int modifyQpRetval,
    enum ibv_qp_state desiredState,
    std::string desiredStateName) {
  // ibv_modify_qp must succeed and returns 0 on success.
  // consolidate printing debug message for all state changes here.
  CHECK_EQ(modifyQpRetval, 0) << fmt::format(
      "Failed to modify QP: [{}] QP-NUM: {} to {}, ret: {} {}",
      creation_index_,
      qp_->qp_num,
      desiredStateName,
      modifyQpRetval,
      folly::errnoStr(modifyQpRetval));

  // For RTR, RTS, and ERROR states, optionally skip ibv_query_qp verification.
  // On some platforms, ibv_query_qp can take about a second
  // for these states. We trust the return value of ibv_modify_qp instead.
  if (ctx_settings_.skip_ibv_query_qp_verification &&
      (desiredState == IBV_QPS_RTR || desiredState == IBV_QPS_RTS ||
       desiredState == IBV_QPS_ERR)) {
    return true;
  }

  // Verify with ibv_query_qp that the QP state matches the desired state
  struct ibv_qp_init_attr init_attr = {};
  struct ibv_qp_attr attr = {};

  auto ret = verbs_->ibv_query_qp(qp_, &attr, IBV_QP_STATE, &init_attr);

  // Insist that ibv_query_qp succeeds if ibv_modify_qp succeeded.
  CHECK_EQ(ret, 0) << fmt::format(
      "Failed to query QP: {} QP-NUM: {}, ret: {} {}",
      creation_index_,
      qp_->qp_num,
      ret,
      folly::errnoStr(ret));

  // If there is a mismatch, log it, and unless told to permit inconsistency,
  // return false.
  if (attr.qp_state != desiredState) {
    LOG(ERROR) << fmt::format(
        "Incorrect QP state, QP: {}, QP-NUM: {} QP state: {}, expected qp state {} ({})",
        creation_index_,
        qp_->qp_num,
        (int)attr.qp_state,
        (int)desiredState,
        desiredStateName);
    if (ctx_settings_.allow_incorrect_queried_qp_state) {
      LOG(ERROR)
          << "Failed to confirm QP is in " << desiredStateName
          << " state, which should be fatal; proceeding as if it was successful";
    } else {
      LOG(ERROR) << "Failed to confirm QP is in " << desiredStateName
                 << " state, use --allow_incorrect_queried_qp_state to bypass";
      CHECK_EQ(attr.qp_state, desiredState);
    }
  }
  return true;
}

// Helper func to modify QP state to INIT state
bool VerbsPeer::modifyRcQpInit(ibv_qp_state targetQpState) {
  if (is_child_sqp_) {
    return true;
  }
  if (!confirmQpModifiedToState(0, IBV_QPS_RESET, "RESET")) {
    return false;
  }

  if (targetQpState == IBV_QPS_RESET) {
    return true;
  }

  struct ibv_qp_attr attr = {
      .qp_state = IBV_QPS_INIT,
      .qp_access_flags = access_flags_,
      .pkey_index = 0,
      .port_num = ctx_settings_.ib_ports[0]};

  auto ret = verbs_->ibv_modify_qp(
      qp_,
      &attr,
      IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
  return confirmQpModifiedToState(ret, IBV_QPS_INIT, "INIT");
}

// Helper to build address handle attributes from GID parameters.
// Used for both multi-path setup and RTR transition to ensure consistency.
ibv_ah_attr VerbsPeer::buildAddressHandle(
    const union ibv_gid& remote_gid,
    uint16_t remote_lid,
    int local_gid_idx,
    int tc) {
  return ibv_ah_attr{
      .grh =
          {.dgid = remote_gid,
           .flow_label = 0,
           .sgid_index = static_cast<uint8_t>(local_gid_idx),
           .hop_limit = 127,
           // bit shifted by two to account for the ECN bits
           .traffic_class = static_cast<uint8_t>(tc << 2)},
      .dlid = remote_lid,
      .sl = ctx_settings_.service_level,
      .src_path_bits = 0,
      .is_global = 1,
      .port_num = ctx_settings_.ib_ports[0],
  };
}

// Helper to add multi-port paths for libsoftmetaroce before RTR transition.
// Returns true on success, false on failure.
bool VerbsPeer::addMultiPortPaths(
    const PortConnectData& remote_port_info,
    int tc) {
  LOG(INFO) << fmt::format(
      "Setting up {} paths for QP {} QP-NUM {} before RTR (libsoftmetaroce multi-port)",
      remote_port_info.gids.size(),
      creation_index_,
      qp_->qp_num);

  for (size_t i = 0; i < remote_port_info.gids.size(); ++i) {
    size_t local_gid_idx_pos =
        (i < port_info_.gid_idx.size()) ? i : port_info_.gid_idx.size() - 1;

    struct ibv_qp_attr path_attr = {};
    path_attr.ah_attr = buildAddressHandle(
        remote_port_info.gids[i],
        remote_port_info.lid,
        port_info_.gid_idx[local_gid_idx_pos],
        tc);

    auto ret = verbs_->ibv_modify_qp(qp_, &path_attr, IBV_QP_AV);
    if (ret != 0) {
      LOG(ERROR) << fmt::format(
          "Failed to add path {} for QP: {} QP-NUM: {} ret={} {}",
          i,
          creation_index_,
          qp_->qp_num,
          ret,
          folly::errnoStr(ret));
      return false;
    }

    LOG(INFO) << fmt::format(
        "Added path {} for QP {}: QP-NUM: {} remote_gid={}, local_gid_idx={}",
        i,
        creation_index_,
        qp_->qp_num,
        formatGids({remote_port_info.gids[i]}),
        port_info_.gid_idx[local_gid_idx_pos]);
  }
  return true;
}

bool VerbsPeer::modifyRcQpRTR(
    const PeerConnectData& remote_peer_info,
    const PortConnectData& remote_port_info,
    int tc,
    enum ibv_mtu mtu) {
  // For libsoftmetaroce with multiple GIDs, add all paths BEFORE the RTR
  // transition. Once the QP transitions to RTR, setupPaths() is called and the
  // paths are fixed.
  if (ctx_settings_.enable_muti_port && remote_port_info.gids.size() > 1) {
    if (!addMultiPortPaths(remote_port_info, tc)) {
      return false;
    }
  }

  // Use first GID from the vector for the RTR transition
  union ibv_gid remote_gid = {};
  if (!remote_port_info.gids.empty()) {
    remote_gid = remote_port_info.gids[0];
  }

  struct ibv_qp_attr attr = {
      .qp_state = IBV_QPS_RTR,
      .path_mtu = mtu, // Affect read throughput
      .rq_psn = static_cast<uint32_t>(remote_peer_info.psn),
      .dest_qp_num = static_cast<uint32_t>(remote_peer_info.qp_num),
      .ah_attr = buildAddressHandle(
          remote_gid, remote_port_info.lid, port_info_.gid_idx[0], tc),
      .max_dest_rd_atomic = ctx_settings_.max_rd, // Affect read throughput
      .min_rnr_timer = ctx_settings_.min_rnr_timer,
  };

  int flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
      IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
  auto ret = verbs_->ibv_modify_qp(qp_, &attr, flags);
  if (!confirmQpModifiedToState(ret, IBV_QPS_RTR, "RTR")) {
    return false;
  }

  return !policy_ ||
      policy_->afterRtr(qp_, port_info_, remote_port_info, ctx_settings_);
}

bool VerbsPeer::modifyRcQpRTS() {
  struct ibv_qp_attr attr{};
  int attr_mask = IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_TIMEOUT |
      IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC;

  attr.qp_state = IBV_QPS_RTS;
  attr.sq_psn = conn_data_.psn;
  attr.timeout = 18;
  attr.retry_cnt = ctx_settings_.retry_cnt; /* 7 = infinite */
  attr.rnr_retry = ctx_settings_.rnr_retry; /* 7 = infinite */
  attr.max_rd_atomic = ctx_settings_.max_rd; // Affect read throughput

  auto ret = verbs_->ibv_modify_qp(qp_, &attr, attr_mask);
  return confirmQpModifiedToState(ret, IBV_QPS_RTS, "RTS");
}

bool VerbsPeer::modifyRcQpErr() {
  struct ibv_qp_attr attr{};
  attr.qp_state = IBV_QPS_ERR;

  auto ret = verbs_->ibv_modify_qp(qp_, &attr, IBV_QP_STATE);
  return confirmQpModifiedToState(ret, IBV_QPS_ERR, "ERR");
}

VerbsMr::VerbsMr(ibv_mr* mr, VerbsDomain* domain) : mr_(mr), domain_(domain) {}

VerbsMr::~VerbsMr() {
  if (mr_) {
    domain_->getVerbs()->ibv_dereg_mr(mr_);
  }
}

void* VerbsMr::addr() const {
  return mr_->addr;
}

size_t VerbsMr::length() const {
  return mr_->length;
}

uint64_t VerbsMr::rkey() const {
  return mr_->rkey;
}

uint64_t VerbsMr::lkey() const {
  return mr_->lkey;
}

ibv_mr* VerbsMr::getIbMrPtr() const {
  return mr_;
}

IRdmaVerbs* VerbsMr::getVerbs() const {
  return domain_->getVerbs();
}

VerbsCq::VerbsCq(ibv_cq* cq, IRdmaVerbs* verbs) : cq_(cq), verbs_(verbs) {}

VerbsCq::~VerbsCq() {
  if (cq_) {
    verbs_->ibv_destroy_cq(cq_);
  }
}

int VerbsCq::poll(WorkCompletionAdapter* wcs, int max_entries) {
  constexpr int kMaxPollBatch = 64;
  ibv_wc raw[kMaxPollBatch];
  int count =
      verbs_->ibv_poll_cq(cq_, std::min(max_entries, kMaxPollBatch), raw);
  for (int i = 0; i < count; ++i) {
    wcs[i] = fromVerbsWc(raw[i]);
  }
  return count;
}

ibv_cq* VerbsCq::getIbCqPtr() const {
  return cq_;
}

IRdmaVerbs* VerbsCq::getVerbs() const {
  return verbs_;
}

VerbsDomain::VerbsDomain(ibv_pd* pd, VerbsContext* context)
    : pd_(pd),
      context_(context->getIbContextPtr()),
      verbs_(context->getVerbs()),
      port_info_(context->getPortInfo()),
      ctx_settings_(context->getCreationParams()),
      peer_policy_(context->getPeerPolicy()) {}

VerbsDomain::~VerbsDomain() {
  if (pd_) {
    verbs_->ibv_dealloc_pd(pd_);
  }
}

std::unique_ptr<ITransportCq> VerbsDomain::createCq(
    int depth,
    ibv_comp_channel* channel) {
  auto cq = verbs_->ibv_create_cq(context_, depth, nullptr, channel, 0);
  if (cq == nullptr) {
    LOG(FATAL) << fmt::format(
        "Failed to create completion queue with {} entries and channel {}",
        depth,
        (void*)channel);
  }
  return std::make_unique<VerbsCq>(cq, verbs_);
}

std::unique_ptr<ITransportMr>
VerbsDomain::registerMemory(void* addr, size_t length, int access_flags) {
  auto mr = verbs_->ibv_reg_mr_internal(pd_, addr, length, access_flags);
  // errno distinguishes a provider-side rejection from a kernel one, and the
  // kernel logs nothing when the call never reaches it.
  const int reg_errno = errno;
  CHECK(mr) << fmt::format(
      "ibv_reg_mr failed addr: {} length: {} flags: {:x} errno: {} ({})",
      addr,
      length,
      access_flags,
      reg_errno,
      std::strerror(reg_errno));
  return std::make_unique<VerbsMr>(mr, this);
}

std::unique_ptr<ITransportMr> VerbsDomain::registerDmabuf(
    uint64_t offset,
    size_t length,
    uint64_t iova,
    int fd,
    int access_flags) {
  auto mr = verbs_->ibv_reg_dmabuf_internal(
      pd_, offset, length, iova, fd, access_flags);

  int err = errno;
  CHECK(mr) << fmt::format(
      "ibv_reg_dmabuf_internal failed errno: {} offset: {} length: {} iova: {:x} fd: {} flags: {:x}",
      folly::errnoStr(err),
      offset,
      length,
      iova,
      fd,
      access_flags);
  return std::make_unique<VerbsMr>(mr, this);
}

namespace {
constexpr uint32_t kQpnTagBitPos = 24;
} // namespace

ibv_qp* VerbsDomain::createTaggedQp(
    const PeerCreationParams& peerAttributes,
    ITransportCq* cq,
    uint32_t source_qpn) {
  ibv_qp_init_attr_ex init_attr_ex{};
  init_attr_ex.cap.max_send_sge = init_attr_ex.cap.max_recv_sge = 1;
  init_attr_ex.send_cq = cq->getIbCqPtr();
  init_attr_ex.recv_cq = cq->getIbCqPtr();
  init_attr_ex.qp_type =
      peerAttributes.use_qpt_driver ? IBV_QPT_DRIVER : IBV_QPT_RC;
  init_attr_ex.cap.max_send_wr = peerAttributes.max_send_wr;
  init_attr_ex.cap.max_recv_wr = peerAttributes.max_recv_wr;
  init_attr_ex.cap.max_inline_data = peerAttributes.max_inline_data;
  init_attr_ex.sq_sig_all = peerAttributes.use_selective_signaling ? 0 : 1;
  init_attr_ex.pd = pd_;
  init_attr_ex.comp_mask = IBV_QP_INIT_ATTR_CREATE_FLAGS | IBV_QP_INIT_ATTR_PD;
  init_attr_ex.create_flags = IBV_QP_CREATE_SOURCE_QPN;
  init_attr_ex.source_qpn = source_qpn;
  return verbs_->ibv_create_qp_ex(context_, &init_attr_ex);
}

std::unique_ptr<ITransportPeer> VerbsDomain::createPeer(
    const PeerCreationParams& peerAttributes,
    ITransportCq* cq,
    ibv_qp_state targetQpState) {
  ibv_qp_init_attr init_attr{};
  init_attr.cap.max_send_sge = init_attr.cap.max_recv_sge = 1;
  init_attr.send_cq = cq->getIbCqPtr();
  init_attr.recv_cq = cq->getIbCqPtr();
  init_attr.qp_type =
      peerAttributes.use_qpt_driver ? IBV_QPT_DRIVER : IBV_QPT_RC;
  init_attr.cap.max_send_wr = peerAttributes.max_send_wr;
  init_attr.cap.max_recv_wr = peerAttributes.max_recv_wr;
  init_attr.cap.max_inline_data = peerAttributes.max_inline_data;
  init_attr.sq_sig_all = peerAttributes.use_selective_signaling ? 0 : 1;
  auto qp = verbs_->ibv_create_qp(pd_, &init_attr);

  if (!qp) {
    LOG(FATAL) << "Failed to create queue pair with error "
               << folly::errnoStr(errno);
  }
  auto res = std::make_unique<VerbsPeer>(
      qp,
      port_info_,
      ctx_settings_,
      peer_creation_idx_++,
      peerAttributes.access_flags,
      verbs_,
      peer_policy_);
  res->modifyRcQpInit(targetQpState);
  return res;
}

std::unique_ptr<ITransportPeer> VerbsDomain::createChildPeer(
    const PeerCreationParams& peerAttributes,
    ITransportCq* cq,
    uint32_t parent_qpn,
    uint8_t sqp_id) {
  const uint32_t source_qpn =
      (static_cast<uint32_t>(sqp_id) << kQpnTagBitPos) | parent_qpn;
  auto qp = createTaggedQp(peerAttributes, cq, source_qpn);
  if (!qp) {
    LOG(FATAL) << fmt::format(
        "Failed to create child SQP {} for parent QPN {:#x} with error {}",
        sqp_id,
        parent_qpn,
        folly::errnoStr(errno));
  }

  auto res = std::make_unique<VerbsPeer>(
      qp,
      port_info_,
      ctx_settings_,
      peer_creation_idx_++,
      peerAttributes.access_flags,
      verbs_,
      peer_policy_);
  res->setChildSqp();
  return res;
}

ibv_pd* VerbsDomain::getIbPdPtr() const {
  return pd_;
}

IRdmaVerbs* VerbsDomain::getVerbs() const {
  return verbs_;
}
namespace {
std::string gidToString(const ibv_gid& gid) {
  std::array<char, INET6_ADDRSTRLEN> gid_str{};
  CHECK_NE(
      nullptr, inet_ntop(AF_INET6, &gid, std::data(gid_str), sizeof gid_str));
  return std::string(std::data(gid_str));
}

bool isErrorAsyncEvent(ibv_event_type eventType) {
  static const std::set<ibv_event_type> kErrorEvents{
      IBV_EVENT_QP_FATAL,
      IBV_EVENT_QP_REQ_ERR,
      IBV_EVENT_QP_ACCESS_ERR,
      IBV_EVENT_PATH_MIG_ERR,
      IBV_EVENT_CQ_ERR,
      IBV_EVENT_SRQ_ERR,
      IBV_EVENT_PORT_ERR,
      IBV_EVENT_DEVICE_FATAL};
  return kErrorEvents.contains(eventType);
}

bool isRoceV2Gid(
    IRdmaVerbs* verbs,
    ibv_context* context,
    uint8_t ib_port,
    int gid_idx) {
  ibv_gid_entry entry{};
  if (verbs->ibv_query_gid_ex(context, ib_port, gid_idx, &entry, 0) != 0) {
    return false;
  }
  return entry.gid_type == IBV_GID_TYPE_ROCE_V2;
}

} // namespace

std::vector<int32_t> VerbsContext::resolveGidAddresses(
    uint8_t ib_port,
    const std::vector<std::string>& addresses) {
  ibv_port_attr portinfo{};
  CHECK_EQ(verbs_->ibv_query_port(context_, ib_port, &portinfo), 0)
      << "ibv_query_port failed";

  std::vector<int32_t> resolved;
  resolved.reserve(addresses.size());

  for (const auto& addr : addresses) {
    ibv_gid target{};
    CHECK_EQ(inet_pton(AF_INET6, addr.c_str(), &target), 1)
        << fmt::format("Invalid IPv6 address: {}", addr);

    // The same address is typically present twice, as RoCE v1 and then as
    // RoCE v2. Only v2 carries IP/UDP headers, so peers that classify inbound
    // traffic on the UDP destination port never see v1 frames as RDMA. Keep
    // scanning past the first match so v2 wins whenever it is present.
    int32_t first_idx = -1;
    int32_t roce_v2_idx = -1;
    for (int idx = 0; idx < portinfo.gid_tbl_len; idx++) {
      ibv_gid gid{};
      if (verbs_->ibv_query_gid(context_, ib_port, idx, &gid) != 0) {
        continue;
      }
      if (memcmp(&gid, &target, sizeof(ibv_gid)) != 0) {
        continue;
      }
      if (first_idx < 0) {
        first_idx = idx;
      }
      if (isRoceV2Gid(verbs_, context_, ib_port, idx)) {
        roce_v2_idx = idx;
        break;
      }
    }
    CHECK_GE(first_idx, 0) << fmt::format(
        "GID address {} not found in GID table for port {}", addr, ib_port);

    if (roce_v2_idx < 0) {
      // Either the device predates ibv_query_gid_ex or it exposes no v2 entry
      // for this address; the first match is the only thing left to use.
      LOG(WARNING) << fmt::format(
          "Resolved GID address {} to index {}, but could not confirm it is "
          "RoCE v2; traffic may be unroutable",
          addr,
          first_idx);
      resolved.push_back(first_idx);
      continue;
    }

    if (roce_v2_idx != first_idx) {
      LOG(INFO) << fmt::format(
          "GID address {} matches index {} as well; preferring RoCE v2 at "
          "index {}",
          addr,
          first_idx,
          roce_v2_idx);
    }
    LOG(INFO) << fmt::format(
        "Resolved GID address {} to index {} (RoCE v2)", addr, roce_v2_idx);
    resolved.push_back(roce_v2_idx);
  }
  return resolved;
}

uint32_t VerbsContext::findBestGid(uint8_t ib_port) {
  ibv_port_attr portinfo{};
  CHECK_EQ(verbs_->ibv_query_port(context_, ib_port, &portinfo), 0)
      << "ibv_query_port failed";

  int best_gid_idx = 0;
  int best_score = -1;

  int gid_tbl_len = portinfo.gid_tbl_len;
  if (gid_tbl_len <= 0) {
    LOG(FATAL) << fmt::format(
        "gid_tbl_len in ibv_port_attr is {}; cannot choose a gid", gid_tbl_len);
  } else {
    LOG(INFO) << fmt::format(
        "gid_tbl_len in ibv_port_attr is {}; choosing the best gid",
        gid_tbl_len);
  }

  for (int gid_idx = 0; gid_idx < gid_tbl_len; gid_idx++) {
    // prefer global ipv6 over global ipv4 over local ipv6 over rest
    int score = 0;
    union ibv_gid gid{};
    if (verbs_->ibv_query_gid(context_, ib_port, gid_idx, &gid) != 0) {
      LOG(ERROR) << fmt::format("Failed to query gid_idx {}", gid_idx);
      continue;
    }
    const uint16_t v4inv6prefix[] = {0, 0, 0, 0, 0, 0xffff};

    // global unicast ipv6 address 2000::/3
    if ((gid.raw[0] & 0xe0) == 0x20) {
      score = 10;
    }
    // link local ipv6 address, fe80::/10
    else if ((gid.raw[0] & 0xff) == 0xfe && (gid.raw[1] & 0xc0) == 0x80) {
      score = 5;
    }
    // v4 addresses
    else if (memcmp(v4inv6prefix, &gid, sizeof(v4inv6prefix)) == 0) {
      if (gid.raw[13] == 127) {
        score = 0; // ipv4, but localhost-like; should not use.
      } else if (gid.raw[13] == 169) {
        score = 2; // ipv4, but autoassigned 169.254/16-like
      } else if (
          gid.raw[13] == 192 || gid.raw[13] == 10 || gid.raw[13] == 172) {
        score = 3; // ipv4, but rfc1918
      } else {
        score = 8; // ipv4 and not obviously limited
      }
    }

    std::string gid_str = gidToString(gid);
    if (score >= best_score) {
      // Prefer the last GID with the best score, because it appears
      // typical to report roce v1 before roce v2; this could be
      // avoided if using ibv_query_gid_table or ibv_query_gid_ex and
      // giving an explicit score to v2 over v1.
      best_gid_idx = gid_idx;
      best_score = score;
      LOG(INFO) << fmt::format(
          "choosing ibv gid idx {} GID {} with score {}",
          best_gid_idx,
          gid_str,
          best_score);
    } else if (score > 0) {
      LOG(INFO) << fmt::format(
          "skipping ibv gid idx {} GID {} with score {}",
          gid_idx,
          gid_str,
          score);
    }
  }
  return best_gid_idx;
}

VerbsContext::VerbsContext(
    ibv_context* context,
    const ContextCreationParams& settings,
    VerbsDevice* device)
    : context_(context),
      device_(device),
      verbs_(device->getVerbs()),
      ctx_settings_(settings) {
  port_info_.gid_idx = settings.preferred_gid_idx;
  CHECK(!settings.ib_ports.empty()) << "ib_ports must not be empty";
  if (port_info_.gid_idx.empty() && !settings.preferred_gid_addresses.empty()) {
    port_info_.gid_idx = resolveGidAddresses(
        settings.ib_ports[0], settings.preferred_gid_addresses);
  }
  if (port_info_.gid_idx.empty()) {
    int gid_idx = findBestGid(settings.ib_ports[0]);
    if (gid_idx >= 0) {
      port_info_.gid_idx.push_back(gid_idx);
    } else {
      port_info_.gid_idx.push_back(0);
    }
  }

  for (int port_index = 0; port_index < settings.ib_ports.size();
       port_index++) {
    for (int i = 0; i < port_info_.gid_idx.size(); i++) {
      ibv_gid gid{};
      CHECK_EQ(
          verbs_->ibv_query_gid(
              context_,
              settings.ib_ports[port_index],
              port_info_.gid_idx[i],
              &gid),
          0)
          << fmt::format(
                 "ibv_query_gid failed for port {} gid index: {}",
                 settings.ib_ports[port_index],
                 port_info_.gid_idx[i]);
      LOG(INFO) << fmt::format("Using GID: {}", gidToString(gid));
      port_info_.gids.push_back(gid);
    }
  }
  ibv_port_attr portinfo{};
  CHECK_EQ(verbs_->ibv_query_port(context_, settings.ib_ports[0], &portinfo), 0)
      << "ibv_query_port failed";
  port_info_.lid = portinfo.lid;
  port_info_.active_mtu = portinfo.active_mtu;
}

VerbsContext::~VerbsContext() {
  if (context_) {
    verbs_->ibv_close_device(context_);
  }
}

DeviceAttributesAdapter VerbsContext::queryAttributes() {
  ibv_device_attr device_attr{};
  CHECK_EQ(verbs_->ibv_query_device(context_, &device_attr), 0);

  return DeviceAttributesAdapter{
      .max_qp_wr = device_attr.max_qp_wr,
      .max_cqe = device_attr.max_cqe,
      .max_mr = device_attr.max_mr};
}

bool VerbsContext::isPortActive(uint8_t portNum) {
  ibv_port_attr portAttr{};
  int res = verbs_->ibv_query_port(context_, portNum, &portAttr);
  if (res != 0) {
    LOG(FATAL) << fmt::format(
        "Failed to query port {} on device {}", portNum, device_->getName());
  }

  return portAttr.state == IBV_PORT_ACTIVE;
}

std::unique_ptr<ITransportDomain> VerbsContext::createDomain() {
  auto pd = verbs_->ibv_alloc_pd(context_);
  if (pd == nullptr) {
    LOG(FATAL) << fmt::format(
        "Failed to allocate protection domain on device {}",
        device_->getName());
  }
  return std::make_unique<VerbsDomain>(pd, this);
}

ibv_context* VerbsContext::getIbContextPtr() const {
  return context_;
}

IRdmaVerbs* VerbsContext::getVerbs() const {
  return verbs_;
}

bool VerbsContext::hasAsyncEvents() const {
  return context_ != nullptr;
}

std::optional<AsyncEventInfo> VerbsContext::pollAsyncEvent(int timeoutMs) {
  if (context_ == nullptr || asyncFdSetupFailed_) {
    return std::nullopt;
  }

  if (!asyncFdNonBlocking_) {
    // Latch a setup failure: the caller polls on a short interval, so retrying
    // (and re-logging) a permanent fd misconfiguration would flood the log.
    int flags = fcntl(context_->async_fd, F_GETFL);
    if (flags < 0) {
      asyncFdSetupFailed_ = true;
      LOG(ERROR) << fmt::format(
          "Failed to get async fd flags, disabling async event polling: {}",
          folly::errnoStr(errno));
      return std::nullopt;
    }
    if (fcntl(context_->async_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
      asyncFdSetupFailed_ = true;
      LOG(ERROR) << fmt::format(
          "Failed to set non-blocking mode on async fd, disabling async event polling: {}",
          folly::errnoStr(errno));
      return std::nullopt;
    }
    asyncFdNonBlocking_ = true;
  }

  struct pollfd pollFd = {context_->async_fd, POLLIN, 0 /* revents */};
  if (poll(&pollFd, 1 /* nfds */, timeoutMs) <= 0) {
    // No event is available within the timeout.
    return std::nullopt;
  }

  struct ibv_async_event event{};
  if (verbs_->ibv_get_async_event(context_, &event)) {
    LOG(ERROR) << "Unable to receive async event when one was expected";
    return std::nullopt;
  }

  const ibv_event_type eventType = event.event_type;
  const char* description = verbs_->ibv_event_type_str(eventType);
  verbs_->ibv_ack_async_event(&event);

  return AsyncEventInfo{
      .eventType = eventType,
      .description = description != nullptr ? std::string(description) : "",
      .isError = isErrorAsyncEvent(eventType)};
}

PortInfoAdapter VerbsDomain::getPortInfo() const {
  return port_info_;
}

const ContextCreationParams& VerbsContext::getCreationParams() const {
  return ctx_settings_;
}

const PortInfoAdapter& VerbsContext::getPortInfo() const {
  return port_info_;
}

const std::shared_ptr<VerbsPeerPolicy>& VerbsContext::getPeerPolicy() const {
  return device_->getPeerPolicy();
}

VerbsDevice::VerbsDevice(ibv_device* ibv_device, VerbsProvider* adapter)
    : device_(ibv_device), adapter_(adapter), verbs_(adapter_->getVerbs()) {
  auto devName = verbs_->ibv_get_device_name(device_);
  CHECK_NOTNULL(devName);
  name_ = devName;
}

VerbsDevice::~VerbsDevice() {}

std::string_view VerbsDevice::getName() const {
  return name_;
}

ibv_device* VerbsDevice::getIbDevicePtr() const {
  return device_;
}

IRdmaVerbs* VerbsDevice::getVerbs() const {
  return adapter_->getVerbs();
}

const std::shared_ptr<VerbsPeerPolicy>& VerbsDevice::getPeerPolicy() const {
  return adapter_->getPeerPolicy();
}

std::unique_ptr<ITransportContext> VerbsDevice::createContext(
    const ContextCreationParams& settings) {
  auto context = verbs_->ibv_open_device(device_);
  if (context == nullptr) {
    LOG(FATAL) << fmt::format("Failed to open device {}", name_);
  }
  return std::make_unique<VerbsContext>(context, settings, this);
}

bool VerbsDevice::isFabricDevice() const {
  return false;
}

VerbsProvider::VerbsProvider(
    std::shared_ptr<IRdmaVerbs> verbs,
    std::shared_ptr<VerbsPeerPolicy> peer_policy)
    : verbs_(std::move(verbs)), peer_policy_(std::move(peer_policy)) {}

VerbsProvider::~VerbsProvider() {
  if (ibv_device_ptr_ != nullptr) {
    verbs_->ibv_free_device_list(ibv_device_ptr_);
  }
}

std::vector<std::unique_ptr<ITransportDevice>> VerbsProvider::getDevices() {
  if (device_count_ == -1) {
    getDeviceCount(); // initialize device_count_ and ibv_device_ptr_
  }

  if (device_count_ == 0) {
    return {};
  }

  std::vector<std::unique_ptr<ITransportDevice>> devices;
  devices.reserve(device_count_);

  for (int i = 0; i < device_count_; i++) {
    devices.emplace_back(
        std::make_unique<VerbsDevice>(ibv_device_ptr_[i], this));
  }

  return devices;
}

size_t VerbsProvider::getDeviceCount() {
  if (device_count_ != -1) {
    return device_count_;
  }
  ibv_device_ptr_ = verbs_->ibv_get_device_list(&device_count_);
  CHECK_NOTNULL(ibv_device_ptr_);
  return device_count_;
}

IRdmaVerbs* VerbsProvider::getVerbs() const {
  return verbs_.get();
}

const std::shared_ptr<VerbsPeerPolicy>& VerbsProvider::getPeerPolicy() const {
  return peer_policy_;
}

} // namespace facebook::neteng::ai::rdma_gen
