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

#include <folly/init/Init.h>

#include <libsoftmetaroce/MetaRoceVerbs.h>
#include <libsoftmetaroce/samples/common.h>

DEFINE_string(sender_addr, "fc00::101", "Expect this address to send us data.");

namespace libsoftmetaroce {} // namespace libsoftmetaroce

int main(int argc, char* argv[]) {
  folly::Init init(&argc, &argv);

  auto postRecvHelper = [](libsoftmetaroce::MetaRoceVerbs& verbs,
                           struct TagMetadata<kLengthBytes>& tm) {
    struct ibv_recv_wr* bad_wr = nullptr;
    struct ibv_recv_wr recv_wr = {
        .wr_id = 200 +
            (uint64_t)libsoftmetaroce::MetaRoceVerbs::qpAndTagToTag(
                     tm.tag_ibvqp->qp_num),
        .sg_list = tm.send_sge.data(),
        .num_sge = 1,
    };
    if (verbs.ibv_post_recv(tm.tag_ibvqp, &recv_wr, &bad_wr)) {
      LOG(ERROR) << "ibv_post_recv failed";
      return 1;
    }
    return 0;
  };

  return executeCommon(folly::IPAddressV6(FLAGS_sender_addr), postRecvHelper);
}
