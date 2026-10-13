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

#include <functional>

#include <gflags/gflags.h>

#include <folly/IPAddressV6.h>

#include <libsoftmetaroce/MetaRoceVerbs.h>
#include <libsoftmetaroce/samples/common.h>
#include <libsoftmetaroce/samples/tag_metadata.h>

DEFINE_uint32(
    num_tags,
    1,
    "Number of Tags to create within the QP (1-256). "
    "Tags are numbered 0 to num_tags-1. "
    "Must be the same on both endpoints.");
DEFINE_validator(num_tags, &validateNumTags);

bool validateNumTags(const char* /* flagname */, uint32_t value) {
  return (value > 0 && value <= 256);
}

std::vector<struct TagMetadata<kLengthBytes>> setupQpAndTags(
    libsoftmetaroce::MetaRoceVerbs& verbs,
    struct ibv_context* context,
    struct ibv_pd* pd,
    struct ibv_cq* cq,
    const folly::IPAddressV6& remoteAddr) {
  // create qp
  struct ibv_qp_init_attr init_attr = {.send_cq = cq, .recv_cq = cq};
  struct ibv_qp* main_ibvqp = verbs.ibv_create_qp(pd, &init_attr);

  std::vector<struct TagMetadata<kLengthBytes>> tag_metadatas;
  tag_metadatas.push_back({.tag_ibvqp = main_ibvqp});

  // Optionally create additional Tags.
  uint32_t baseQpNum =
      libsoftmetaroce::MetaRoceVerbs::qpAndTagToQp(main_ibvqp->qp_num);
  // Only used if creating multiple Tags.
  struct ibv_qp_init_attr_ex init_attr_ex = {
      .send_cq = cq,
      .recv_cq = cq,
      .qp_type = IBV_QPT_RC,
      .comp_mask = IBV_QP_INIT_ATTR_CREATE_FLAGS,
      .pd = pd,
      .create_flags = IBV_QP_CREATE_SOURCE_QPN,
  };
  // Tag 0 is the base QP. Additional Tags are numbered 1 to num_tags-1.
  for (uint32_t tag = 1; tag < FLAGS_num_tags; ++tag) {
    LOG(INFO) << "Creating Tag: " << tag;
    init_attr_ex.source_qpn = libsoftmetaroce::MetaRoceVerbs::combineQpAndTag(
        baseQpNum, (uint8_t)tag);
    struct ibv_qp* tag_ibvqp = verbs.ibv_create_qp_ex(context, &init_attr_ex);
    if (!tag_ibvqp) {
      LOG(ERROR) << "ibv_create_qp_ex failed for Tag: " << tag;
      return tag_metadatas;
    }
    struct ibv_qp_attr attr{.rq_psn = 0xfaceb00c, .sq_psn = 0xfaceb00c};
    verbs.ibv_modify_qp(tag_ibvqp, &attr, IBV_QP_RQ_PSN | IBV_QP_SQ_PSN);
    tag_metadatas.push_back({.tag_ibvqp = tag_ibvqp});
  }

  // Transition the QP to RTR/RTS state (do once for main_ibvqp, and this is
  // applied to all Tags).
  struct ibv_qp_attr attr{};
  memcpy(attr.ah_attr.grh.dgid.raw, remoteAddr.toBinary().data(), 16);
  attr.dest_qp_num = libsoftmetaroce::kMetaRoceInitialQpId;
  attr.path_mtu = IBV_MTU_4096;
  attr.qp_state = IBV_QPS_INIT;
  attr.rq_psn = attr.sq_psn = 0xcafef00d;
  verbs.ibv_modify_qp(
      main_ibvqp,
      &attr,
      IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_SQ_PSN | IBV_QP_AV |
          IBV_QP_PATH_MTU | IBV_QP_STATE);
  attr.qp_state = IBV_QPS_RTR;
  verbs.ibv_modify_qp(main_ibvqp, &attr, IBV_QP_STATE);
  attr.qp_state = IBV_QPS_RTS;
  verbs.ibv_modify_qp(main_ibvqp, &attr, IBV_QP_STATE);

  return tag_metadatas;
}

void
setupMemoryRegion(
    libsoftmetaroce::MetaRoceVerbs& verbs,
    struct ibv_pd* pd,
    struct TagMetadata<kLengthBytes>& tm) {
  // Register the memory region (ibv_mr) for this Tag's buffer.
  tm.mr = verbs.ibv_reg_mr_internal(
      pd,
      tm.buffer.data(),
      kLengthBytes,
      IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE);
  if (!tm.mr) {
    return;
  }
  // Prepare the scatter-gather list (ibv_sge).
  tm.send_sge = {{
      {
          (uint64_t)tm.buffer.data(), /* addr */
          kLengthBytes, /* length */
          tm.mr->lkey /* lkey */
      },
  }};
}

void poll_cq_loop_until_completed(
    libsoftmetaroce::MetaRoceVerbs& verbs,
    struct ibv_cq* cq,
    int expected_wc) {
  if (!cq) {
    throw std::runtime_error("Null cq");
  }
  if (expected_wc < 0) {
    throw std::runtime_error("Negative expected_wc");
  }

  std::vector<struct ibv_wc> wc(expected_wc);
  int new_wc, total_wc = 0;
  while (total_wc < expected_wc) {
    new_wc = verbs.ibv_poll_cq(cq, expected_wc, wc.data());
    total_wc += new_wc;
    if (new_wc > 0) {
      LOG(INFO) << fmt::format("Received {} completions. Errors:", new_wc);
    }
    for (int i = 0; i < new_wc; ++i) {
      if (wc[i].status != IBV_WC_SUCCESS) {
        LOG(INFO) << fmt::format(
            "WC status: {}", static_cast<int>(wc[i].status));
      }
    }
    // @lint-ignore CLANGTIDY facebook-hte-BadCall-usleep
    usleep(50000);
  }
}

void teardown(
    libsoftmetaroce::MetaRoceVerbs& verbs,
    std::vector<struct TagMetadata<kLengthBytes>>& tag_metadatas,
    struct ibv_pd* pd,
    struct ibv_cq* cq) {
  for (auto& tm : tag_metadatas) {
    if (tm.tag_ibvqp) {
      verbs.ibv_destroy_qp(tm.tag_ibvqp);
      tm.tag_ibvqp = nullptr;
    }
    if (tm.mr) {
      verbs.ibv_dereg_mr(tm.mr);
      tm.mr = nullptr;
    }
  }
  if (pd) {
    verbs.ibv_dealloc_pd(pd);
  }
  if (cq) {
    verbs.ibv_destroy_cq(cq);
  }
}

int executeCommon(
    const folly::IPAddressV6& remoteAddr,
    const std::function<
        int(libsoftmetaroce::MetaRoceVerbs&,
            struct TagMetadata<kLengthBytes>&)>& postFunc) {
  libsoftmetaroce::MetaRoceVerbs verbs;
  struct ibv_context context{};
  struct ibv_pd* pd = verbs.ibv_alloc_pd(&context);
  // One work request per Tag below, and the poll loop waits for all of them,
  // so the queue is sized to hold every completion at once.
  struct ibv_cq* cq = verbs.ibv_create_cq(
      &context,
      FLAGS_num_tags /* cqe */,
      nullptr /* cq_context */,
      nullptr,
      0 /* comp_vector */);

  std::vector<struct TagMetadata<kLengthBytes>> tag_metadatas =
      setupQpAndTags(verbs, &context, pd, cq, remoteAddr);
  if (tag_metadatas.size() != FLAGS_num_tags) {
    LOG(ERROR) << "Expected " << FLAGS_num_tags << " Tags, but only created "
               << tag_metadatas.size();
    teardown(verbs, tag_metadatas, pd, cq);
    return 1;
  }

  // For each Tag, post one WQE.
  for (auto& tm : tag_metadatas) {
    setupMemoryRegion(verbs, pd, tm);
    if (!tm.mr) {
      LOG(ERROR) << "Failed to setup memory region";
      teardown(verbs, tag_metadatas, pd, cq);
      return 1;
    }
    if (postFunc(verbs, tm)) {
      LOG(ERROR) << "Failed to post WQE";
      teardown(verbs, tag_metadatas, pd, cq);
      return 1;
    }
  }

  // Wait for one message per Tag to complete.
  poll_cq_loop_until_completed(verbs, cq, FLAGS_num_tags);

  teardown(verbs, tag_metadatas, pd, cq);
  return 0;
}
