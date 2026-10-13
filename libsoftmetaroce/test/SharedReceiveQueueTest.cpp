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

#include <libsoftmetaroce/SharedReceiveQueue.h>

#include <folly/ScopeGuard.h>
#include <gtest/gtest.h>
#include <infiniband/verbs.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace libsoftmetaroce {
namespace {
// A refused tag is recorded under these, and a post cancels its RNR.
constexpr uint32_t kWaitingQpNum = 7;
constexpr uint8_t kWaitingTagNum = 3;
// A taker that is neither recorded tag, so these takes drop no record.
constexpr uint32_t kTakingQpNum = kWaitingQpNum + 1;
constexpr uint8_t kTakingTagNum = kWaitingTagNum + 1;
constexpr uint16_t kCsn = 4242;
// A loaded machine only lengthens the wait under test, so slowness cannot turn
// a held destroy into a passing one.
constexpr auto kUnblockedWaitBound = std::chrono::milliseconds(200);

std::optional<SharedReceive> tryTakeLeaving(
    SharedReceiveQueue& queue,
    uint32_t leaveAtLeast,
    uint32_t qpNum,
    uint8_t tagNum,
    uint16_t csn = kCsn) {
  return queue.tryTakeOldestLeavingAtLeast(
      leaveAtLeast,
      SharedReceiveTake{
          .taker = {.qpNum = qpNum, .tagNum = tagNum},
          .csnToRecordOnRefusal = csn});
}

// Takes from the queue as a taker neither recorded tag uses, for the cases
// that do not turn on which tag takes.
std::optional<SharedReceive> tryTakeLeaving(
    SharedReceiveQueue& queue,
    uint32_t leaveAtLeast) {
  return tryTakeLeaving(queue, leaveAtLeast, kTakingQpNum, kTakingTagNum);
}

std::optional<SharedReceive> tryTakeLeavingWithoutRecording(
    SharedReceiveQueue& queue,
    uint32_t leaveAtLeast,
    uint32_t qpNum,
    uint8_t tagNum) {
  return queue.tryTakeOldestLeavingAtLeast(
      leaveAtLeast,
      SharedReceiveTake{.taker = {.qpNum = qpNum, .tagNum = tagNum}});
}

// A take no posted receive can satisfy, which is how a tag joins the records
// a later post cancels for.
void takeFromEmptyQueueToRecord(
    SharedReceiveQueue& queue,
    uint32_t qpNum,
    uint8_t tagNum,
    uint16_t csn = kCsn) {
  EXPECT_FALSE(tryTakeLeaving(queue, 0, qpNum, tagNum, csn).has_value())
      << "precondition: the empty queue records queue pair " << qpNum << " tag "
      << static_cast<int>(tagNum) << " on an unsatisfiable take";
}
} // namespace

TEST(SharedReceiveQueueTest, TakesTheDepthItWasCreatedWithAndNoMore) {
  struct ibv_pd pd{};
  constexpr uint32_t kDepth = 3;
  SharedReceiveQueue queue{kDepth, /*maxSge=*/1, /*protectionDomain=*/pd};

  for (uint32_t i = 0; i < kDepth; ++i) {
    EXPECT_TRUE(queue.tryPost(SharedReceive{.workRequestId = i}))
        << "the queue holds the depth ibv_create_srq was given, so receive "
        << i << " has somewhere to go";
  }

  EXPECT_FALSE(queue.tryPost(SharedReceive{.workRequestId = kDepth}))
      << "a receive past the depth is refused rather than taken, so a client "
         "learns through bad_wr that the queue is full";
  EXPECT_EQ(queue.getNumPosted(), kDepth)
      << "the refused receive left what was already taken, which the queue "
         "pairs drawing on it still need";
}

TEST(SharedReceiveQueueTest, HandsOutReceivesOldestFirst) {
  struct ibv_pd pd{};
  SharedReceiveQueue queue{
      /*maxWorkRequests=*/3, /*maxSge=*/1, /*protectionDomain=*/pd};
  for (uint64_t i = 0; i < 3; ++i) {
    ASSERT_TRUE(queue.tryPost(SharedReceive{.workRequestId = i}))
        << "precondition: the queue holds receive " << i;
  }

  for (uint64_t i = 0; i < 3; ++i) {
    const std::optional<SharedReceive> taken = tryTakeLeaving(queue, 0);
    ASSERT_TRUE(taken.has_value())
        << "the queue holds three receives, so taking at " << i << " finds one";
    EXPECT_EQ(taken->workRequestId, i)
        << "a queue pair takes in the order the client posted, so the work "
           "request ids come back in the order they went in";
  }

  EXPECT_FALSE(tryTakeLeaving(queue, 0).has_value())
      << "a drained queue hands out no receive, which is what leaves an "
         "arriving message refused with BRNR";
}

TEST(
    SharedReceiveQueueTest,
    ATakeIsRefusedUnlessItLeavesTheOnesAheadTheirReceives) {
  struct ibv_pd pd{};
  SharedReceiveQueue queue{
      /*maxWorkRequests=*/3, /*maxSge=*/1, /*protectionDomain=*/pd};
  for (uint64_t i = 0; i < 3; ++i) {
    ASSERT_TRUE(queue.tryPost(SharedReceive{.workRequestId = i}))
        << "precondition: the queue holds receive " << i;
  }

  EXPECT_FALSE(tryTakeLeaving(queue, 3).has_value())
      << "three receives cannot serve a message and the three still missing "
         "ahead of it, so the queue keeps them rather than spending one on "
         "work that cannot report until those messages arrive";
  EXPECT_EQ(queue.getNumPosted(), 3u)
      << "a refused take leaves every receive where it was, so the messages "
         "ahead still find one when they come";

  const std::optional<SharedReceive> taken = tryTakeLeaving(queue, 2);
  ASSERT_TRUE(taken.has_value())
      << "the same three serve a message with two missing ahead of it, since "
         "the two those need are still here once this one is taken";
  EXPECT_EQ(queue.getNumPosted(), 2u)
      << "the take left exactly what the messages ahead of it need";
}

TEST(SharedReceiveQueueTest, ATakenReceiveFreesRoomToPostAgain) {
  struct ibv_pd pd{};
  SharedReceiveQueue queue{
      /*maxWorkRequests=*/1, /*maxSge=*/1, /*protectionDomain=*/pd};
  ASSERT_TRUE(queue.tryPost(SharedReceive{.workRequestId = 1}))
      << "precondition: the only entry is taken";
  ASSERT_FALSE(queue.tryPost(SharedReceive{.workRequestId = 2}))
      << "precondition: the queue is full, which is what taking one relieves";

  EXPECT_TRUE(tryTakeLeaving(queue, 0).has_value())
      << "the queue holds a receive, so a queue pair taking one finds it";
  EXPECT_TRUE(queue.tryPost(SharedReceive{.workRequestId = 2}))
      << "taking it gave the entry back, or a client that keeps the queue full "
         "could never replenish it";
}

TEST(SharedReceiveQueueTest, ALimitIsReportedBelowItAndOnlyOnce) {
  struct ibv_pd pd{};
  int reports = 0;
  SharedReceiveQueue queue{/*maxWorkRequests=*/3,
                           /*maxSge=*/1,
                           /*protectionDomain=*/pd,
                           [&reports] {
                             ++reports;
                             return true;
                           }};
  // The client acks what it was told, which a report holds a reference for.
  SCOPE_EXIT {
    queue.releaseEventReferences(reports);
  };
  for (uint64_t i = 0; i < 3; ++i) {
    ASSERT_TRUE(queue.tryPost(SharedReceive{.workRequestId = i}))
        << "precondition: the queue holds receive " << i;
  }
  queue.armLimit(2);

  ASSERT_TRUE(tryTakeLeaving(queue, 0).has_value())
      << "precondition: the first take hands out a receive, which is what "
         "brings the queue to the limit";
  EXPECT_EQ(reports, 0)
      << "two receives are left, which reaches the limit without falling "
         "below it, and a client armed at two still has the two it asked to "
         "keep";

  ASSERT_TRUE(tryTakeLeaving(queue, 0).has_value())
      << "precondition: the second take hands one out too, leaving fewer than "
         "the limit";
  EXPECT_EQ(reports, 1)
      << "the take that left fewer than the limit is the one a client arms "
         "for, since it is the point at which it must post more";

  ASSERT_TRUE(tryTakeLeaving(queue, 0).has_value())
      << "precondition: a third take drains the queue, so a second report "
         "would have to come from the spent arming";
  EXPECT_EQ(reports, 1)
      << "the arming was spent on the first report, so draining further tells "
         "the client nothing until it arms again";
  EXPECT_EQ(queue.getArmedLimit(), 0u)
      << "a spent arming reads as none, which is what ibv_query_srq reports";
}

TEST(SharedReceiveQueueTest, AnUnarmedQueueReportsNothing) {
  struct ibv_pd pd{};
  int reports = 0;
  SharedReceiveQueue queue{/*maxWorkRequests=*/2,
                           /*maxSge=*/1,
                           /*protectionDomain=*/pd,
                           [&reports] {
                             ++reports;
                             return true;
                           }};
  // The client acks what it was told, which a report holds a reference for.
  SCOPE_EXIT {
    queue.releaseEventReferences(reports);
  };
  ASSERT_TRUE(queue.tryPost(SharedReceive{.workRequestId = 1}))
      << "precondition: the queue holds a receive for the take below";
  ASSERT_TRUE(queue.tryPost(SharedReceive{.workRequestId = 2}))
      << "precondition: the queue is full, so the takes below empty it";

  ASSERT_TRUE(tryTakeLeaving(queue, 0).has_value())
      << "precondition: the first take empties the queue by half";
  ASSERT_TRUE(tryTakeLeaving(queue, 0).has_value())
      << "precondition: the second drains it, which is as far as a take can "
         "carry an unarmed queue";
  EXPECT_EQ(reports, 0)
      << "a client that armed no limit is told nothing however far the queue "
         "is drained, or an empty queue would raise an event for every client";
}

TEST(SharedReceiveQueueTest, AnArmedQueueWithNoReporterTakesQuietly) {
  struct ibv_pd pd{};
  SharedReceiveQueue queue{
      /*maxWorkRequests=*/2, /*maxSge=*/1, /*protectionDomain=*/pd};
  ASSERT_TRUE(queue.tryPost(SharedReceive{.workRequestId = 1}))
      << "precondition: the queue holds the receive the take below removes";
  queue.armLimit(2);

  EXPECT_TRUE(tryTakeLeaving(queue, 0).has_value())
      << "a queue built with no way to report still hands out its receives, "
         "rather than throwing on the take that would have reported";
}

TEST(SharedReceiveQueueTest, AQueueIsReferencedWhileAQueuePairHoldsOne) {
  struct ibv_pd pd{};
  SharedReceiveQueue queue{
      /*maxWorkRequests=*/1, /*maxSge=*/1, /*protectionDomain=*/pd};
  EXPECT_FALSE(queue.isReferenced())
      << "a queue no queue pair uses is free for ibv_destroy_srq to take";

  queue.takeReference();
  EXPECT_TRUE(queue.isReferenced())
      << "the queue pair holding it can still draw from it, so a destroy "
         "must be refused";

  queue.dropReference();
  EXPECT_FALSE(queue.isReferenced())
      << "the last queue pair gave its reference back, so the destroy that "
         "was refused now succeeds";
}

TEST(SharedReceiveQueueTest, TheWaitHoldsUntilTheEventIsAcked) {
  struct ibv_pd pd{};
  SharedReceiveQueue queue{
      /*maxWorkRequests=*/1, /*maxSge=*/1, /*protectionDomain=*/pd};
  queue.takeEventReference();

  std::mutex returnedMutex;
  std::condition_variable returnedChanged;
  bool returned = false;
  std::thread waiter([&] {
    queue.waitForEventsAcked();
    {
      const std::lock_guard<std::mutex> guard{returnedMutex};
      returned = true;
    }
    returnedChanged.notify_all();
  });

  {
    std::unique_lock<std::mutex> lock{returnedMutex};
    EXPECT_FALSE(returnedChanged.wait_for(
        lock, kUnblockedWaitBound, [&returned] { return returned; }))
        << "the destroy this wait stands for must not free the queue while a "
           "client holds an event raised for it";
  }

  queue.releaseEventReferences(1);

  {
    std::unique_lock<std::mutex> lock{returnedMutex};
    EXPECT_TRUE(returnedChanged.wait_for(
        lock, std::chrono::seconds(5), [&returned] { return returned; }))
        << "the ack gives back the last reference the event held, so the wait "
           "it was holding finishes";
  }
  waiter.join();
}

TEST(SharedReceiveQueueTest, APostCancelsTheRnrOfTheTagItsReceiveServes) {
  constexpr uint16_t kCsnExpected = 41;
  struct ibv_pd pd{};
  std::vector<std::pair<uint32_t, uint8_t>> rnrCanceledTags;
  std::vector<uint16_t> cancelCsns;
  SharedReceiveQueue queue{/*maxWorkRequests=*/2,
                           /*maxSge=*/1,
                           /*protectionDomain=*/pd,
                           /*reportLimitReached=*/{},
                           [&rnrCanceledTags, &cancelCsns](
                               SharedReceiveTaker refused, uint16_t csn) {
                             rnrCanceledTags.emplace_back(
                                 refused.qpNum, refused.tagNum);
                             cancelCsns.push_back(csn);
                             return true;
                           }};

  takeFromEmptyQueueToRecord(
      queue, kWaitingQpNum, kWaitingTagNum, kCsnExpected);
  EXPECT_TRUE(rnrCanceledTags.empty())
      << "the queue cancels for a recorded tag only once a receive it can "
         "take arrives";

  ASSERT_TRUE(queue.tryPost(SharedReceive{.workRequestId = 1}))
      << "the post has to be accepted, or no receive arrives for the recorded "
         "tag";
  EXPECT_EQ(
      rnrCanceledTags,
      (std::vector<std::pair<uint32_t, uint8_t>>{
          {kWaitingQpNum, kWaitingTagNum}}))
      << "the posted receive goes to the tag it can serve, so that tag's "
         "initiator is the one the RNR-Cancel reaches";
  EXPECT_EQ(cancelCsns, (std::vector<uint16_t>{kCsnExpected}))
      << "the RNR-Cancel carries the csn recorded when the tag was refused, "
         "which the responder read on the thread that advances it";
}

TEST(SharedReceiveQueueTest, OneReceiveCancelsTheRnrOfOneTagBlockedOnIt) {
  constexpr uint8_t kSecondTag = kWaitingTagNum + 1;
  struct ibv_pd pd{};
  std::vector<std::pair<uint32_t, uint8_t>> rnrCanceledTags;
  SharedReceiveQueue queue{
      /*maxWorkRequests=*/2,
      /*maxSge=*/1,
      /*protectionDomain=*/pd,
      /*reportLimitReached=*/{},
      [&rnrCanceledTags](SharedReceiveTaker refused, uint16_t) {
        rnrCanceledTags.emplace_back(refused.qpNum, refused.tagNum);
        return true;
      }};
  takeFromEmptyQueueToRecord(queue, kWaitingQpNum, kWaitingTagNum);
  takeFromEmptyQueueToRecord(queue, kWaitingQpNum, kSecondTag);

  ASSERT_TRUE(queue.tryPost(SharedReceive{.workRequestId = 1}))
      << "the post has to be accepted, or the first recorded tag is never "
         "reached";
  EXPECT_EQ(
      rnrCanceledTags,
      (std::vector<std::pair<uint32_t, uint8_t>>{
          {kWaitingQpNum, kWaitingTagNum}}))
      << "one receive serves one take, so a cancel for the second tag too "
         "would leave it no receive and have it refused again";

  ASSERT_TRUE(queue.tryPost(SharedReceive{.workRequestId = 2}))
      << "the second post has to be accepted for the tag behind to be served";
  ASSERT_EQ(rnrCanceledTags.size(), 2u)
      << "the second receive reaches the tag that waited behind the first";
  EXPECT_EQ(rnrCanceledTags.back(), std::make_pair(kWaitingQpNum, kSecondTag))
      << "tags recorded on the queue are served oldest first, as the receives "
         "are";
}

TEST(SharedReceiveQueueTest, ATagRecordedTwiceGetsOneRnrCancel) {
  struct ibv_pd pd{};
  int cancels = 0;
  SharedReceiveQueue queue{/*maxWorkRequests=*/2,
                           /*maxSge=*/1,
                           /*protectionDomain=*/pd,
                           /*reportLimitReached=*/{},
                           [&cancels](SharedReceiveTaker, uint16_t) {
                             ++cancels;
                             return true;
                           }};
  for (int attempt = 0; attempt < 2; ++attempt) {
    takeFromEmptyQueueToRecord(queue, kWaitingQpNum, kWaitingTagNum);
  }

  ASSERT_TRUE(queue.tryPost(SharedReceive{.workRequestId = 1}))
      << "precondition: two receives are accepted, so a second cancel could "
         "go out if the tag was recorded twice";
  ASSERT_TRUE(queue.tryPost(SharedReceive{.workRequestId = 2}))
      << "precondition: two receives are accepted, so a second cancel could "
         "go out if the tag was recorded twice";
  EXPECT_EQ(cancels, 1)
      << "a tag waits once however often its initiator retried, or one client "
         "would get a cancel for every probe";
}

TEST(SharedReceiveQueueTest, ARepeatRefusalRecordsTheCsnItNowExpects) {
  struct ibv_pd pd{};
  uint16_t cancelCsn = 0;
  SharedReceiveQueue queue{/*maxWorkRequests=*/1,
                           /*maxSge=*/1,
                           /*protectionDomain=*/pd,
                           /*reportLimitReached=*/{},
                           [&cancelCsn](SharedReceiveTaker, uint16_t csn) {
                             cancelCsn = csn;
                             return true;
                           }};
  takeFromEmptyQueueToRecord(queue, kWaitingQpNum, kWaitingTagNum, 41);
  ASSERT_FALSE(
      tryTakeLeaving(queue, 0, kWaitingQpNum, kWaitingTagNum, 42).has_value())
      << "precondition: the queue records the retry too, moving the recorded "
         "csn";

  ASSERT_TRUE(queue.tryPost(SharedReceive{.workRequestId = 1}))
      << "precondition: one receive arrives for the tag recorded twice";
  EXPECT_EQ(cancelCsn, 42)
      << "the cancel tells the initiator the csn the tag expects now, since "
         "one it completed meanwhile would leave the initiator backed off";
}

TEST(SharedReceiveQueueTest, ATagThatTookAReceiveGetsNoRnrCancel) {
  struct ibv_pd pd{};
  std::vector<uint8_t> rnrCanceledTags;
  SharedReceiveQueue queue{
      /*maxWorkRequests=*/4,
      /*maxSge=*/1,
      /*protectionDomain=*/pd,
      /*reportLimitReached=*/{},
      [&rnrCanceledTags](SharedReceiveTaker refused, uint16_t) {
        rnrCanceledTags.push_back(refused.tagNum);
        return true;
      }};
  constexpr uint8_t kSecondTag = kWaitingTagNum + 1;
  takeFromEmptyQueueToRecord(queue, kWaitingQpNum, kWaitingTagNum);
  takeFromEmptyQueueToRecord(queue, kWaitingQpNum, kSecondTag);

  ASSERT_TRUE(queue.tryPost(SharedReceive{.workRequestId = 1}))
      << "precondition: one receive arrives, which cancels for the tag at the "
         "front and leaves the one behind it recorded";
  ASSERT_TRUE(tryTakeLeaving(queue, 0, kWaitingQpNum, kSecondTag).has_value())
      << "precondition: the tag still recorded is the one that takes it, "
         "reaching the receive before the tag the cancel went to";

  ASSERT_TRUE(queue.tryPost(SharedReceive{.workRequestId = 2}))
      << "precondition: a second receive arrives";
  EXPECT_EQ(rnrCanceledTags, (std::vector<uint8_t>{kWaitingTagNum}))
      << "a tag holding a receive it took gets no cancel meant for another, "
         "which would spend the cancel a tag still recorded needs";
}

TEST(SharedReceiveQueueTest, AQueuePairLettingGoLeavesNoRecordBehind) {
  struct ibv_pd pd{};
  constexpr uint32_t kOtherQpNum = kWaitingQpNum + 10;
  std::vector<uint32_t> rnrCanceledQps;
  SharedReceiveQueue queue{
      /*maxWorkRequests=*/2,
      /*maxSge=*/1,
      /*protectionDomain=*/pd,
      /*reportLimitReached=*/{},
      [&rnrCanceledQps](SharedReceiveTaker refused, uint16_t) {
        rnrCanceledQps.push_back(refused.qpNum);
        return true;
      }};
  queue.takeReference();
  queue.takeReference();
  takeFromEmptyQueueToRecord(queue, kWaitingQpNum, kWaitingTagNum);
  takeFromEmptyQueueToRecord(queue, kOtherQpNum, kWaitingTagNum);

  queue.forgetRefusedTags(kWaitingQpNum);
  queue.dropReference();

  ASSERT_TRUE(queue.tryPost(SharedReceive{.workRequestId = 1}))
      << "precondition: a receive arrives with one of the two recorded queue "
         "pairs gone";
  EXPECT_EQ(rnrCanceledQps, (std::vector<uint32_t>{kOtherQpNum}))
      << "the queue pair still here is the one canceled for, since a number a "
         "later create may reuse would otherwise take the cancel";

  queue.forgetRefusedTags(kOtherQpNum);
  queue.dropReference();
}

TEST(SharedReceiveQueueTest, APostCancelsForTheFirstTagItsDepthServes) {
  constexpr uint8_t kSecondTag = kWaitingTagNum + 1;
  struct ibv_pd pd{};
  std::vector<uint8_t> canceledTags;
  SharedReceiveQueue queue{
      /*maxWorkRequests=*/4,
      /*maxSge=*/1,
      /*protectionDomain=*/pd,
      /*reportLimitReached=*/{},
      [&canceledTags](SharedReceiveTaker refused, uint16_t) {
        canceledTags.push_back(refused.tagNum);
        return true;
      }};
  // The first recorded tag holds three back, the second none.
  ASSERT_FALSE(
      tryTakeLeaving(queue, 3, kWaitingQpNum, kWaitingTagNum).has_value())
      << "precondition: the empty queue records the first tag on an "
         "unsatisfiable take";
  takeFromEmptyQueueToRecord(queue, kWaitingQpNum, kSecondTag);

  ASSERT_TRUE(queue.tryPost(SharedReceive{.workRequestId = 1}))
      << "precondition: one receive arrives, serving only the second tag";
  EXPECT_EQ(canceledTags, (std::vector<uint8_t>{kSecondTag}))
      << "the post skips the tag its depth cannot serve, or that tag "
         "would take the cancel and be refused again";

  ASSERT_TRUE(queue.tryPost(SharedReceive{.workRequestId = 2}))
      << "precondition: a second receive arrives, still short of three";
  ASSERT_TRUE(queue.tryPost(SharedReceive{.workRequestId = 3}))
      << "precondition: a third receive arrives, matching but not clearing it";
  EXPECT_EQ(canceledTags, (std::vector<uint8_t>{kSecondTag}))
      << "three posted is not more than the three the tag holds back, so "
         "no cancel goes out";

  ASSERT_TRUE(queue.tryPost(SharedReceive{.workRequestId = 4}))
      << "precondition: a fourth receive arrives, clearing the threshold";
  EXPECT_EQ(canceledTags, (std::vector<uint8_t>{kSecondTag, kWaitingTagNum}))
      << "the post that serves the first tag finally cancels for it";
}

TEST(SharedReceiveQueueTest, ARepeatRefusalKeepsTheSmallerThreshold) {
  struct ibv_pd pd{};
  std::vector<uint8_t> canceledTags;
  SharedReceiveQueue queue{
      /*maxWorkRequests=*/2,
      /*maxSge=*/1,
      /*protectionDomain=*/pd,
      /*reportLimitReached=*/{},
      [&canceledTags](SharedReceiveTaker refused, uint16_t) {
        canceledTags.push_back(refused.tagNum);
        return true;
      }};
  takeFromEmptyQueueToRecord(queue, kWaitingQpNum, kWaitingTagNum);
  ASSERT_FALSE(
      tryTakeLeaving(queue, 3, kWaitingQpNum, kWaitingTagNum).has_value())
      << "precondition: a retry needing more must not raise the threshold past "
         "what the first refusal needed";

  ASSERT_TRUE(queue.tryPost(SharedReceive{.workRequestId = 1}))
      << "precondition: one receive arrives, serving the tag recorded first";
  EXPECT_EQ(canceledTags, (std::vector<uint8_t>{kWaitingTagNum}))
      << "the threshold stays at the smaller need, or after the larger retry "
         "no post that serves the tag cancels for it";
}

TEST(SharedReceiveQueueTest, ASuppressedTakeRecordsNoTag) {
  struct ibv_pd pd{};
  std::vector<uint8_t> canceledTags;
  SharedReceiveQueue queue{
      /*maxWorkRequests=*/2,
      /*maxSge=*/1,
      /*protectionDomain=*/pd,
      /*reportLimitReached=*/{},
      [&canceledTags](SharedReceiveTaker refused, uint16_t) {
        canceledTags.push_back(refused.tagNum);
        return true;
      }};
  ASSERT_FALSE(
      tryTakeLeavingWithoutRecording(queue, 0, kWaitingQpNum, kWaitingTagNum)
          .has_value())
      << "precondition: the empty queue records no tag on an unsatisfiable "
         "take";
  ASSERT_TRUE(queue.tryPost(SharedReceive{.workRequestId = 1}))
      << "precondition: a receive arrives after the unrecorded take";
  EXPECT_TRUE(canceledTags.empty())
      << "no tag was recorded, so the post cancels for none";
}

TEST(SharedReceiveQueueTest, ATakeOnlyRefusalLeavesTheRecordedTagItsCancel) {
  struct ibv_pd pd{};
  std::vector<uint8_t> canceledTags;
  SharedReceiveQueue queue{
      /*maxWorkRequests=*/2,
      /*maxSge=*/1,
      /*protectionDomain=*/pd,
      /*reportLimitReached=*/{},
      [&canceledTags](SharedReceiveTaker refused, uint16_t) {
        canceledTags.push_back(refused.tagNum);
        return true;
      }};
  // The queue selects records in order, so the take that records no tag goes
  // first.
  ASSERT_FALSE(
      tryTakeLeavingWithoutRecording(queue, 0, kTakingQpNum, kTakingTagNum)
          .has_value())
      << "precondition: the empty queue refuses this take and records no tag "
         "for it";
  takeFromEmptyQueueToRecord(queue, kWaitingQpNum, kWaitingTagNum);

  ASSERT_TRUE(queue.tryPost(SharedReceive{.workRequestId = 1}))
      << "precondition: one receive arrives for the two takes to contend";
  EXPECT_EQ(canceledTags, (std::vector<uint8_t>{kWaitingTagNum}))
      << "the tag whose initiator is backed off gets the cancel, or a "
         "refusal already bound for a NAK spends it and leaves the waiting "
         "initiator paused until its timer expires";
}

TEST(SharedReceiveQueueTest, ConcurrentPostsCancelForOneTagForTheDepthTheyAdd) {
  constexpr int kRounds = 1000;
  constexpr uint8_t kSecondTag = kWaitingTagNum + 1;
  for (int round = 0; round < kRounds; ++round) {
    struct ibv_pd pd{};
    std::atomic<int> cancels{0};
    SharedReceiveQueue queue{/*maxWorkRequests=*/2,
                             /*maxSge=*/1,
                             /*protectionDomain=*/pd,
                             /*reportLimitReached=*/{},
                             [&cancels](SharedReceiveTaker, uint16_t) {
                               cancels.fetch_add(1, std::memory_order_acq_rel);
                               return true;
                             }};
    // Each recorded tag holds one back, so two posted receives clear one of
    // them and the two posts add exactly the depth for a single cancel.
    for (const uint8_t tagNum : {kWaitingTagNum, kSecondTag}) {
      ASSERT_FALSE(tryTakeLeaving(queue, 1, kWaitingQpNum, tagNum).has_value())
          << "round " << round
          << ": precondition: the empty queue records both tags on "
             "unsatisfiable takes";
    }

    std::atomic<int> atTheLine{0};
    auto waitForTheOther = [&atTheLine] {
      atTheLine.fetch_add(1, std::memory_order_acq_rel);
      while (atTheLine.load(std::memory_order_acquire) < 2) {
        std::this_thread::yield();
      }
    };
    std::atomic<int> posted{0};
    std::vector<std::thread> posters;
    posters.reserve(2);
    for (uint64_t i = 0; i < 2; ++i) {
      posters.emplace_back([&waitForTheOther, &queue, &posted, i] {
        waitForTheOther();
        if (queue.tryPost(SharedReceive{.workRequestId = i + 1})) {
          posted.fetch_add(1, std::memory_order_acq_rel);
        }
      });
    }
    for (auto& poster : posters) {
      poster.join();
    }

    ASSERT_EQ(posted.load(std::memory_order_acquire), 2)
        << "round " << round
        << ": precondition: the queue has room for both receives";
    ASSERT_EQ(cancels.load(std::memory_order_acquire), 1)
        << "round " << round
        << ": the second receive is the only one either tag can take while "
           "leaving the one it holds back, so selecting the tag outside "
           "the lock lets both posts claim it and cancels for a tag whose take is "
           "refused again";
  }
}

} // namespace libsoftmetaroce
