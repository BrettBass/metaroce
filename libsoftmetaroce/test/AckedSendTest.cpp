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

#include <gtest/gtest.h>

#include <folly/IPAddressV6.h>
#include <infiniband/verbs.h>
#include <libsoftmetaroce/Constants.h>
#include <libsoftmetaroce/MetaRoceVerbs.h>
#include <libsoftmetaroce/test/AckingPeer.h>
#include <poll.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>

namespace libsoftmetaroce {
namespace {

constexpr int kCqEntries = 10;
constexpr uint32_t kRemoteQpNum = 10;
constexpr uint64_t kWrId = 200;
// Several maximum-size packets, so the work request spans more than one and
// completes only once every one of them is acked.
constexpr uint32_t kPayloadBytes = 2048 * 5;
// The peer acks each packet as it is written, so a passing run takes
// milliseconds.
constexpr auto kNoProgressBound = std::chrono::seconds(5);

// A queue pair whose peer acks what it sends, so a posted work request reaches
// an ordinary completion rather than the error one that follows exhausted
// retries.
class AckedSendTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_NO_FATAL_FAILURE(bringUp(kCqEntries, /*maxWr=*/0));
  }

  // A completion queue of `cqEntries` and a queue pair on it, in RTS, with its
  // peer answering. `maxWr` sizes both the send and the receive queue, and none
  // takes the library's default for each.
  void bringUp(int cqEntries, uint32_t maxWr) {
    int numDevices = 0;
    struct ibv_device** deviceList = verbs_.ibv_get_device_list(&numDevices);
    ASSERT_GT(numDevices, 0)
        << "precondition: a device to open, or the queue pair has no context";
    context_ = verbs_.ibv_open_device(deviceList[0]);
    verbs_.ibv_free_device_list(deviceList);
    ASSERT_NE(context_, nullptr);
    cq_ = verbs_.ibv_create_cq(context_, cqEntries, nullptr, nullptr, 0);
    ASSERT_NE(cq_, nullptr);
    pd_ = verbs_.ibv_alloc_pd(context_);
    ASSERT_NE(pd_, nullptr);
    ASSERT_NO_FATAL_FAILURE(createQpInRts(maxWr));
    ASSERT_NO_FATAL_FAILURE(pointPeerAtReceiver());
  }

  void TearDown() override {
    // Ahead of the queue pairs below, so no write during teardown is answered.
    peer_.stopAcking();
    if (mr_ != nullptr) {
      EXPECT_EQ(verbs_.ibv_dereg_mr(mr_), 0);
    }
    if (qp_ != nullptr) {
      EXPECT_EQ(verbs_.ibv_destroy_qp(qp_), 0);
    }
    if (pd_ != nullptr) {
      EXPECT_EQ(verbs_.ibv_dealloc_pd(pd_), 0);
    }
    if (cq_ != nullptr) {
      EXPECT_EQ(verbs_.ibv_destroy_cq(cq_), 0);
    }
    if (context_ != nullptr) {
      EXPECT_EQ(verbs_.ibv_close_device(context_), 0);
    }
  }

  void createQpInRts(uint32_t maxWr) {
    struct ibv_qp_init_attr initAttr{};
    initAttr.send_cq = cq_;
    initAttr.recv_cq = cq_;
    initAttr.sq_sig_all = 1;
    initAttr.cap.max_send_wr = maxWr;
    initAttr.cap.max_recv_wr = maxWr;
    qp_ = verbs_.ibv_create_qp(pd_, &initAttr);
    ASSERT_NE(qp_, nullptr);

    struct ibv_qp_attr attr{};
    memcpy(
        attr.ah_attr.grh.dgid.raw,
        folly::IPAddressV6("fc00::1").toBinary().data(),
        16);
    attr.dest_qp_num =
        MetaRoceVerbs::combineQpAndTag(kRemoteQpNum, kMetaRoceInitialTag);
    attr.path_mtu = IBV_MTU_2048;
    attr.qp_state = IBV_QPS_INIT;
    ASSERT_EQ(
        verbs_.ibv_modify_qp(
            qp_,
            &attr,
            IBV_QP_DEST_QPN | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_STATE),
        0)
        << "precondition: the queue pair takes its peer and path, without which "
           "it cannot reach RTS";
    attr.qp_state = IBV_QPS_RTR;
    ASSERT_EQ(verbs_.ibv_modify_qp(qp_, &attr, IBV_QP_STATE), 0)
        << "precondition: the queue pair reaches RTR on its way to RTS";
    attr.qp_state = IBV_QPS_RTS;
    ASSERT_EQ(verbs_.ibv_modify_qp(qp_, &attr, IBV_QP_STATE), 0)
        << "precondition: the queue pair reaches RTS, or there is no sender for "
           "the test to drive";
  }

  // The verbs object builds its receiver while bringing the queue pair up, on
  // the queue pair's local gid, so the peer is pointed at it only here.
  void pointPeerAtReceiver() {
    struct ibv_gid_entry entry{};
    ASSERT_EQ(verbs_.ibv_query_gid_ex(context_, 1, 0, &entry, 0), 0)
        << "precondition: the local gid reads back, or there is no receiver to "
           "name";
    Receiver* receiver =
        verbs_.getReceiver(MetaRoceVerbs::gidEntryToIPAddress(entry), entry);
    ASSERT_NE(receiver, nullptr);
    peer_.ackTo(*receiver);
  }

  void registerPayload(uint32_t bytes) {
    payload_ = std::make_unique<char[]>(bytes);
    mr_ = verbs_.ibv_reg_mr_internal(
        pd_,
        payload_.get(),
        bytes,
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    ASSERT_NE(mr_, nullptr);
  }

  // Posts one write of the registered region, answering with what
  // ibv_post_send returned so a caller can watch for the depth refusing it.
  [[nodiscard]] int postWrite(uint64_t wrId, uint32_t bytes) {
    struct ibv_sge sge = {
        .addr = reinterpret_cast<uint64_t>(payload_.get()),
        .length = bytes,
        .lkey = mr_->lkey};
    struct ibv_send_wr sendWr = {
        .wr_id = wrId,
        .sg_list = &sge,
        .num_sge = 1,
        .opcode = IBV_WR_RDMA_WRITE,
        .wr = {.rdma = {.remote_addr = 1, .rkey = 1}}};
    struct ibv_send_wr* badWr = nullptr;
    return verbs_.ibv_post_send(qp_, &sendWr, &badWr);
  }

  // Registers the payload and posts it as one write, which a peer that acks
  // completes on its own.
  void postOneWrite() {
    ASSERT_NO_FATAL_FAILURE(registerPayload(kPayloadBytes));
    ASSERT_EQ(postWrite(kWrId, kPayloadBytes), 0)
        << "precondition: the post is accepted, or there is no send for the "
           "peer to ack";
  }

  [[nodiscard]] bool pollOneCompletion(struct ibv_wc& wc) {
    const auto deadline = std::chrono::steady_clock::now() + kNoProgressBound;
    while (std::chrono::steady_clock::now() < deadline) {
      if (verbs_.ibv_poll_cq(cq_, 1, &wc) == 1) {
        return true;
      }
      std::this_thread::yield();
    }
    return false;
  }

  // Declared before the verbs object it makes sockets for, so it outlives it.
  AckingPeer peer_;
  MetaRoceVerbs verbs_{peer_.socketMaker()};
  struct ibv_context* context_{nullptr};
  struct ibv_cq* cq_{nullptr};
  struct ibv_pd* pd_{nullptr};
  struct ibv_qp* qp_{nullptr};
  struct ibv_mr* mr_{nullptr};
  std::unique_ptr<char[]> payload_;
};

TEST_F(AckedSendTest, APeerThatAcksCompletesTheWorkRequestItAcks) {
  ASSERT_NO_FATAL_FAILURE(postOneWrite());

  struct ibv_wc wc{};
  ASSERT_TRUE(pollOneCompletion(wc))
      << "the work request completes once its packets are acked, and the peer "
         "acks every packet it is asked to send";
  EXPECT_EQ(wc.status, kSuccess)
      << "an acked work request completes successfully; a status of "
      << wc.status
      << " is what a queue pair reports when it gave up on a peer that never "
         "answered, which is what a packet the peer did not see looks like";
  EXPECT_EQ(wc.wr_id, kWrId)
      << "the completion identifies the work request posted, so a test can "
         "tell it from one the queue pair flushed";
  EXPECT_GT(peer_.getAckCount(), 0)
      << "the peer answered the packets, rather than the completion arriving "
         "by some path that does not need it";
}

// A completion queue of two entries under a send queue of four, so the work
// runs past what the queue can report while the send queue keeps recycling.
constexpr int kShallowCqEntries = 2;
constexpr uint32_t kShallowSendDepth = 4;
// One packet per work request, so a completion follows each ack directly.
constexpr uint32_t kOnePacketBytes = 1024;
constexpr uint64_t kAckedPosts = 8;
// A loaded machine only lengthens the wait under test, so slowness cannot turn
// a held destroy into a passing one.
constexpr auto kUnblockedDestroyBound = std::chrono::milliseconds(200);

class ShallowCompletionQueueSendTest : public AckedSendTest {
 protected:
  void SetUp() override {
    ASSERT_NO_FATAL_FAILURE(bringUp(kShallowCqEntries, kShallowSendDepth));
  }

  // A send queue holding its depth answers ENOMEM until an earlier work
  // request gives an entry back, so a post it refuses is posted again.
  void postUntilAccepted(
      uint64_t firstWrId,
      uint64_t count,
      std::chrono::steady_clock::time_point deadline) {
    uint64_t accepted = 0;
    while (accepted < count && std::chrono::steady_clock::now() < deadline) {
      const int rc = postWrite(firstWrId + accepted, kOnePacketBytes);
      if (rc == 0) {
        ++accepted;
      } else {
        ASSERT_EQ(rc, ENOMEM)
            << "the send queue depth is the only thing refusing a post here, "
               "and it answers ENOMEM; "
            << rc << " came from somewhere else";
        std::this_thread::yield();
      }
    }
    ASSERT_EQ(accepted, count)
        << "a work request gives its send queue entry back whether or not the "
           "completion queue had an entry to report it, so every post is "
           "accepted in turn; posting stopped at "
        << accepted;
  }
};

TEST_F(
    ShallowCompletionQueueSendTest,
    WorkPastTheDepthRunsAndLosesOnlyItsReports) {
  ASSERT_NO_FATAL_FAILURE(registerPayload(kOnePacketBytes));
  const auto deadline = std::chrono::steady_clock::now() + kNoProgressBound;

  ASSERT_NO_FATAL_FAILURE(postUntilAccepted(0, kAckedPosts, deadline));

  // One packet per work request, so an ack apiece means the peer has answered
  // every one of them.
  while (peer_.getAckCount() < static_cast<int>(kAckedPosts) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  ASSERT_GE(peer_.getAckCount(), static_cast<int>(kAckedPosts))
      << "precondition: the peer answered every packet, or work is still "
         "outstanding and a completion for it could still arrive";
  peer_.stopAcking();

  // Work no ack can complete: accepting a send queue's depth of it means
  // every work request above completed, holding `reported` below still.
  ASSERT_NO_FATAL_FAILURE(
      postUntilAccepted(kAckedPosts, kShallowSendDepth, deadline));

  int reported = 0;
  struct ibv_wc wc{};
  while (verbs_.ibv_poll_cq(cq_, 1, &wc) == 1) {
    EXPECT_EQ(wc.status, kSuccess)
        << "the peer acked every packet the queue found an entry for, so each "
           "work request it reports succeeded; it reported "
        << wc.status;
    ++reported;
  }

  EXPECT_LE(reported, kShallowCqEntries)
      << "a client polls what ibv_create_cq sized the queue for and no more, "
         "because a completion the queue had no entry for was dropped; it "
         "answered "
      << reported << " for " << kAckedPosts << " work requests";
  EXPECT_GE(reported, 1)
      << "the entries the queue does have are used, so a client that sized its "
         "queue under its work still sees the completions it had room for";
}

TEST_F(
    ShallowCompletionQueueSendTest,
    ADroppedReportRaisesAnEventAClientCanPollFor) {
  ASSERT_NO_FATAL_FAILURE(registerPayload(kOnePacketBytes));
  const auto deadline = std::chrono::steady_clock::now() + kNoProgressBound;
  ASSERT_NO_FATAL_FAILURE(postUntilAccepted(0, kAckedPosts, deadline));

  // The queue holds two, so the work past that is reported to nobody and each
  // of those raises the event this waits for.
  struct pollfd asyncPoll = {context_->async_fd, POLLIN, 0 /* revents */};
  const int timeoutMs = static_cast<int>(
      std::chrono::duration_cast<std::chrono::milliseconds>(kNoProgressBound)
          .count());
  ASSERT_EQ(poll(&asyncPoll, 1, timeoutMs), 1)
      << "async_fd reports readable once a completion is dropped, which is how "
         "a client learns of one without polling the completion queue";

  struct ibv_async_event event{};
  ASSERT_EQ(verbs_.ibv_get_async_event(context_, &event), 0)
      << "the readable descriptor answers with the event behind it";
  EXPECT_EQ(event.event_type, IBV_EVENT_CQ_ERR)
      << "a completion the depth refused is reported as a completion queue "
         "error, the event the Linux rxe driver raises for the same refusal";
  EXPECT_EQ(event.element.cq, cq_)
      << "the event carries the queue that refused, so a client holding several "
         "knows which one lost a report";
  verbs_.ibv_ack_async_event(&event);
}

TEST_F(
    ShallowCompletionQueueSendTest,
    DestroyingAQueueTakesBackTheEventsRaisedForIt) {
  ASSERT_NO_FATAL_FAILURE(registerPayload(kOnePacketBytes));
  const auto deadline = std::chrono::steady_clock::now() + kNoProgressBound;
  ASSERT_NO_FATAL_FAILURE(postUntilAccepted(0, kAckedPosts, deadline));

  struct pollfd asyncPoll = {context_->async_fd, POLLIN, 0 /* revents */};
  const int timeoutMs = static_cast<int>(
      std::chrono::duration_cast<std::chrono::milliseconds>(kNoProgressBound)
          .count());
  ASSERT_EQ(poll(&asyncPoll, 1, timeoutMs), 1)
      << "precondition: a refused completion raised the event the destroy "
         "below has to take back";

  ASSERT_EQ(verbs_.ibv_destroy_qp(qp_), 0);
  qp_ = nullptr;
  ASSERT_EQ(verbs_.ibv_destroy_cq(cq_), 0);
  cq_ = nullptr;

  struct ibv_async_event event{};
  const int taken = verbs_.ibv_get_async_event(context_, &event);
  const int failure = errno;
  EXPECT_EQ(taken, -1)
      << "an event left for a destroyed completion queue would hand a client a "
         "pointer to memory the destroy freed";
  EXPECT_EQ(failure, EAGAIN)
      << "the context is still open and simply holds no event";
  asyncPoll.revents = 0;
  EXPECT_EQ(poll(&asyncPoll, 1, 0 /* timeoutMs */), 0)
      << "the wakeup byte goes with the event it was written for, or a client "
         "polls ready for an event it can never take";
}

TEST_F(
    ShallowCompletionQueueSendTest,
    DestroyingAQueueWaitsForTheEventItHandedOut) {
  ASSERT_NO_FATAL_FAILURE(registerPayload(kOnePacketBytes));
  const auto deadline = std::chrono::steady_clock::now() + kNoProgressBound;
  ASSERT_NO_FATAL_FAILURE(postUntilAccepted(0, kAckedPosts, deadline));

  struct pollfd asyncPoll = {context_->async_fd, POLLIN, 0 /* revents */};
  const int timeoutMs = static_cast<int>(
      std::chrono::duration_cast<std::chrono::milliseconds>(kNoProgressBound)
          .count());
  ASSERT_EQ(poll(&asyncPoll, 1, timeoutMs), 1)
      << "precondition: a refused completion raised an event to hand out";

  struct ibv_async_event event{};
  ASSERT_EQ(verbs_.ibv_get_async_event(context_, &event), 0)
      << "precondition: the event reaches the client, or the destroy below has "
         "nothing outstanding to wait on";

  ASSERT_EQ(verbs_.ibv_destroy_qp(qp_), 0)
      << "precondition: the queue pair goes first, or the destroy below "
         "answers EBUSY for the reference it holds instead of waiting";
  qp_ = nullptr;

  std::mutex destroyedMutex;
  std::condition_variable destroyedChanged;
  bool destroyed = false;
  std::thread destroyer([&] {
    EXPECT_EQ(verbs_.ibv_destroy_cq(cq_), 0)
        << "the queue pair is gone and the event is the only thing left "
           "holding the queue, so the destroy waits rather than refusing";
    {
      const std::lock_guard<std::mutex> guard{destroyedMutex};
      destroyed = true;
    }
    destroyedChanged.notify_all();
  });

  {
    std::unique_lock<std::mutex> lock{destroyedMutex};
    EXPECT_FALSE(destroyedChanged.wait_for(
        lock, kUnblockedDestroyBound, [&destroyed] { return destroyed; }))
        << "the destroy must not free the queue while a client holds an event "
           "raised for it, whose element.cq would then point into freed "
           "memory";
  }

  verbs_.ibv_ack_async_event(&event);

  {
    std::unique_lock<std::mutex> lock{destroyedMutex};
    EXPECT_TRUE(destroyedChanged.wait_for(
        lock, kNoProgressBound, [&destroyed] { return destroyed; }))
        << "the ack gives back the last reference the event held, so the "
           "destroy it was keeping waiting finishes";
  }
  destroyer.join();
  cq_ = nullptr;
}

} // namespace
} // namespace libsoftmetaroce
