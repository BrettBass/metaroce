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

#include <folly/ScopeGuard.h>
#include <folly/portability/GMock.h>
#include <gflags/gflags.h>
#include <gtest/gtest.h>
#include <libsoftmetaroce/MetaRoceVerbs.h>
#include <libsoftmetaroce/test/MockUDPSocket.h>
#include <cstring>
#include <memory>

namespace libsoftmetaroce {
namespace {

std::pair<std::unique_ptr<folly::AsyncUDPSocket>, std::unique_ptr<Modifier>>
mockUDPSocketMaker(folly::EventBase* evb) {
  return std::make_pair(
      std::make_unique<::testing::NiceMock<MockUDPSocket>>(evb), nullptr);
}

// One case in a binary of its own: the counts resolve once per process, so a
// second case here would read whatever answer this one settled on.
TEST(FlowCountFlagsTest, FormerPathNamesStillSizeTheQueuePair) {
  gflags::SetCommandLineOption("libsoftmetaroce_txpaths", "3");
  gflags::SetCommandLineOption("libsoftmetaroce_rxpaths", "5");

  MetaRoceVerbs verbs(mockUDPSocketMaker);
  struct ibv_context context{};
  struct ibv_cq* cq = verbs.ibv_create_cq(&context, 10, nullptr, nullptr, 0);
  ASSERT_NE(cq, nullptr);
  SCOPE_EXIT {
    EXPECT_EQ(verbs.ibv_destroy_cq(cq), 0);
  };
  struct ibv_pd* pd = verbs.ibv_alloc_pd(&context);
  ASSERT_NE(pd, nullptr);
  SCOPE_EXIT {
    EXPECT_EQ(verbs.ibv_dealloc_pd(pd), 0);
  };

  struct ibv_qp_init_attr initAttr{};
  initAttr.send_cq = cq;
  initAttr.recv_cq = cq;
  struct ibv_qp* ibvqp = verbs.ibv_create_qp(pd, &initAttr);
  ASSERT_NE(ibvqp, nullptr);
  SCOPE_EXIT {
    EXPECT_EQ(verbs.ibv_destroy_qp(ibvqp), 0);
  };

  struct ibv_qp_attr attr{};
  const auto remote = folly::IPAddressV6("fc00::1");
  memcpy(attr.ah_attr.grh.dgid.raw, remote.toBinary().data(), 16);
  attr.dest_qp_num = MetaRoceVerbs::combineQpAndTag(10, kMetaRoceInitialTag);
  attr.path_mtu = IBV_MTU_2048;
  attr.qp_state = IBV_QPS_INIT;
  verbs.ibv_modify_qp(
      ibvqp,
      &attr,
      IBV_QP_DEST_QPN | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_STATE);
  // Sizing happens on the move to RTR, which is where the counts are read.
  attr.qp_state = IBV_QPS_RTR;
  verbs.ibv_modify_qp(ibvqp, &attr, IBV_QP_STATE);

  auto* qp = verbs.getQpByNum(MetaRoceVerbs::qpAndTagToQp(ibvqp->qp_num));
  ASSERT_NE(qp, nullptr);

  EXPECT_EQ(qp->getNumTxFlows(), 3u)
      << "a caller still passing --libsoftmetaroce_txpaths has to keep getting "
         "the transmit count it asked for rather than silently the default";
  EXPECT_EQ(qp->getNumRxFlows(), 5u)
      << "the receive count resolves from its own pair of names, so a correct "
         "transmit wiring does not vouch for it";
}

} // namespace
} // namespace libsoftmetaroce
