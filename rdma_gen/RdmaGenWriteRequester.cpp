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

#include "rdma_gen/RdmaGenWriteRequester.h"

namespace facebook::neteng::ai::rdma_gen {

RdmaGenWriteRequester::RdmaGenWriteRequester(
    const Params& params,
    const std::vector<int64_t>& numMessagesPerServer,
    bool equalMessagesPerServer,
    const std::vector<std::string>& rcReceivers,
    const std::vector<int>& rcCmPorts,
    bool logMessageLatencies,
    const std::vector<int>& rcCos,
    const std::vector<enum ibv_mtu>& mtus,
    const enum ibv_qp_state targetQPState,
    const std::string& jsonPath)
    : RdmaGenRequester(
          params,
          numMessagesPerServer,
          equalMessagesPerServer,
          rcReceivers,
          rcCmPorts,
          logMessageLatencies,
          rcCos,
          mtus,
          targetQPState,
          jsonPath) {
  CHECK(
      params.role == EndpointRole::WriteRequester ||
      params.role == EndpointRole::WriteImmediateRequester);
  remoteAddrs_ = std::make_shared<std::vector<void*>>();
  rkeys_ = std::make_shared<std::vector<uint32_t>>();

  // Use the factory to create the appropriate control client
  cmClient_ = RdmaGenControlFactory::createControlClient(
      endpoint_, remoteRcQpInfo_, remoteAddrs_, rkeys_);
}

int RdmaGenWriteRequester::postRequest(const RcBuffer& rcBuf) {
  const auto addr = (uintptr_t)endpoint_->getBuffer(rcBuf.token);
  statsRecorder_->recordStartTime(rcBuf.token, getCurrentTimestampUs());
  const auto key = params().use_block ? ctx()->mr_list[0]->lkey()
                                      : ctx()->mr_list[rcBuf.token]->lkey();

  SendWorkRequestAdapter wr{
      .wr_id = createWrIdWithSeq(
          getNextSendSeqNum(rcBuf.qp_idx), rcBuf.qp_idx, rcBuf.token),
      .opcode = rcBuf.maybe_imm_data ? SendOpcode::RdmaWriteWithImm
                                     : SendOpcode::RdmaWrite,
      .send_flags =
          convertVerbsSendFlags(getSendFlags(rcBuf.qp_idx, rcBuf.len)),
      .local_addr = addr,
      .length = rcBuf.len,
      .lkey = key,
      .imm_data = rcBuf.maybe_imm_data,
  };
  wr.target.rdma = RdmaTarget{
      .remote_addr = reinterpret_cast<uint64_t>(remoteAddrs_->at(rcBuf.qp_idx)),
      .rkey = rkeys_->at(rcBuf.qp_idx),
  };

  int postSendRet = ctx()->peer_list[rcBuf.qp_idx]->postSend(wr);

  LOG_IF(ERROR, postSendRet != 0)
      << "ibv_post_send failed for message " << rcBuf.token << " retval "
      << postSendRet << " " << folly::errnoStr(postSendRet);
  return postSendRet;
}
} // namespace facebook::neteng::ai::rdma_gen
