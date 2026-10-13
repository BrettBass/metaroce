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
#include <libsoftmetaroce/Constants.h>
#include <libsoftmetaroce/ReceiveSequence.h>

namespace libsoftmetaroce {

namespace {
constexpr uint16_t kNothingDelivered = 0;
constexpr uint16_t kFirstMsn = 1;
} // namespace

TEST(ReceiveSequenceTest, DeliveringTheNextMsnAdvancesOverItAlone) {
  ReceiveSequence sequence(
      {.nextMsn = kFirstMsn, .nextCsn = kUnconfiguredStartingCsn});
  sequence.noteInFlight(kFirstMsn);

  EXPECT_EQ(sequence.markDelivered(kFirstMsn), 1)
      << "the message that closes the gap is the only one delivery runs over";
  EXPECT_EQ(sequence.cumulativeDelivered(), kFirstMsn)
      << "delivery has run through the message just delivered";
}

TEST(ReceiveSequenceTest, AMessageAheadOfTheGapAdvancesNothingYet) {
  ReceiveSequence sequence(
      {.nextMsn = kFirstMsn, .nextCsn = kUnconfiguredStartingCsn});
  sequence.noteInFlight(3);

  EXPECT_EQ(sequence.markDelivered(3), 0)
      << "msn 1 has not arrived, so no message may be reported delivered "
         "behind it";
  EXPECT_EQ(sequence.cumulativeDelivered(), kNothingDelivered)
      << "the cumulative msn may not pass a message that has not arrived";
}

TEST(ReceiveSequenceTest, ClosingTheGapAdvancesOverTheWholeRun) {
  ReceiveSequence sequence(
      {.nextMsn = kFirstMsn, .nextCsn = kUnconfiguredStartingCsn});
  sequence.noteInFlight(3);
  EXPECT_EQ(sequence.markDelivered(2), 0);
  EXPECT_EQ(sequence.markDelivered(3), 0);

  EXPECT_EQ(sequence.markDelivered(1), 3)
      << "the arriving message releases itself and the two already waiting "
         "behind it, which their initiator must be told about together";
  EXPECT_EQ(sequence.cumulativeDelivered(), 3);
}

TEST(ReceiveSequenceTest, TheCumulativeMsnWrapsPastItsWidth) {
  // The first expected msn is the largest, so the run below crosses the wrap.
  ReceiveSequence sequence(
      {.nextMsn = UINT16_MAX, .nextCsn = kUnconfiguredStartingCsn});
  sequence.noteInFlight(1);
  EXPECT_EQ(sequence.markDelivered(UINT16_MAX), 1);
  EXPECT_EQ(sequence.cumulativeDelivered(), UINT16_MAX);

  EXPECT_EQ(sequence.markDelivered(0), 1)
      << "msn zero follows the largest msn, so delivery runs over it rather "
         "than reading it as long past";
  EXPECT_EQ(sequence.cumulativeDelivered(), 0);
}

TEST(ReceiveSequenceTest, RenumberingStartsTheSequenceOver) {
  ReceiveSequence sequence(
      {.nextMsn = kFirstMsn, .nextCsn = kUnconfiguredStartingCsn});
  sequence.noteInFlight(9);
  EXPECT_EQ(sequence.markDelivered(9), 0);

  sequence = ReceiveSequence({.nextMsn = 41, .nextCsn = 101});

  EXPECT_EQ(sequence.cumulativeDelivered(), 40)
      << "delivery stands one behind the msn an rq_psn says the peer resumes "
         "at";
  EXPECT_EQ(sequence.nextCsn(), 101)
      << "an rq_psn sets the csn the next posted receive takes";

  sequence.noteInFlight(41);
  EXPECT_EQ(sequence.markDelivered(41), 1)
      << "the message recorded under the old numbering went with it, so the "
         "first of the new run closes the gap on its own";
}

TEST(ReceiveSequenceTest, CsnsAreHandedOutInOrder) {
  ReceiveSequence sequence({.nextMsn = kFirstMsn, .nextCsn = 101});

  EXPECT_EQ(sequence.takeNextCsn(), 101);
  EXPECT_EQ(sequence.takeNextCsn(), 102);
  EXPECT_EQ(sequence.nextCsn(), 103)
      << "receives are numbered in the order they are posted, so reading the "
         "next one must not consume it";
}

TEST(ReceiveSequenceTest, TheCompletedCsnEdgeAdvancesOnlyOverTheNextOne) {
  ReceiveSequence sequence({.nextMsn = kFirstMsn, .nextCsn = 101});

  EXPECT_EQ(sequence.cumulativeCompletedCsn(), 100)
      << "the edge starts behind the first csn to be handed out, so that one "
         "completing advances it";

  EXPECT_FALSE(sequence.tryMarkCsnCompleted(102))
      << "a csn with one still outstanding behind it is refused, which is how "
         "the caller learns the peer skipped at least one";
  EXPECT_EQ(sequence.cumulativeCompletedCsn(), 100)
      << "a csn with one still outstanding behind it leaves the edge, which "
         "stands for the run completed with no gap";

  EXPECT_TRUE(sequence.tryMarkCsnCompleted(101))
      << "the csn after the edge is accepted, so its receive completes with "
         "the sequence reporting no gap behind it";
  EXPECT_EQ(sequence.cumulativeCompletedCsn(), 101)
      << "the one after the edge carries it forward";
}

TEST(ReceiveSequenceTest, TheCompletedCsnEdgeWrapsPastItsWidth) {
  ReceiveSequence sequence({.nextMsn = kFirstMsn, .nextCsn = 65535});

  ASSERT_TRUE(sequence.tryMarkCsnCompleted(65535))
      << "precondition: the edge takes the last csn of the width before the "
         "wrap below is tested";
  EXPECT_EQ(sequence.cumulativeCompletedCsn(), 65535)
      << "precondition: the edge stands on the last csn of the width, so the "
         "step below is the one that crosses the wrap";

  EXPECT_TRUE(sequence.tryMarkCsnCompleted(0))
      << "the edge accepts the csn after 65535, so a receive at the wrap is "
         "not read as a skipped csn";
  EXPECT_EQ(sequence.cumulativeCompletedCsn(), 0)
      << "csns are sixteen bits and the edge follows them over the wrap, so "
         "the one after 65535 is the next it accepts";
}

} // namespace libsoftmetaroce
