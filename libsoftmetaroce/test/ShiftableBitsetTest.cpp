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

#include <libsoftmetaroce/ShiftableBitset.h>

#include <gtest/gtest.h>
#include <vector>

#include <libsoftmetaroce/codec/BitVector.h>

namespace libsoftmetaroce {
namespace {

using Bitset = ShiftableBitset<256>;

std::vector<size_t> setBits(const Bitset& b) {
  std::vector<size_t> out;
  b.forEachSetBit([&](size_t i) { out.push_back(i); });
  return out;
}

TEST(ShiftableBitsetTest, SetTestResetSpanWordBoundaries) {
  Bitset b;
  EXPECT_FALSE(b.any())
      << "a freshly constructed bitset must report no bits set";
  for (size_t i : {size_t{0}, size_t{1}, size_t{63}, size_t{64}, size_t{255}}) {
    b.set(i);
  }
  EXPECT_TRUE(b.test(0) && b.test(63) && b.test(64) && b.test(255))
      << "set must light up bits on both sides of a 64-bit word boundary";
  EXPECT_FALSE(b.test(62))
      << "set must not touch neighbouring bits within a word";
  b.reset(63);
  EXPECT_FALSE(b.test(63))
      << "reset must clear exactly the given bit without disturbing the word";
  EXPECT_TRUE(b.test(64))
      << "reset of one bit must leave an adjacent set bit in the next word";
}

TEST(ShiftableBitsetTest, IntersectsDetectsOverlapBeyondFirstWord) {
  Bitset a;
  Bitset b;
  a.set(5);
  a.set(200);
  b.set(64);
  EXPECT_FALSE(a.intersects(b))
      << "intersects must be false when no bit is set in both bitsets";
  b.set(200);
  EXPECT_TRUE(a.intersects(b))
      << "intersects must find an overlap that lives past the first 64-bit word";
}

TEST(ShiftableBitsetTest, ForEachSetBitVisitsAscending) {
  Bitset b;
  b.set(200);
  b.set(5);
  b.set(64);
  b.set(0);
  EXPECT_EQ(setBits(b), (std::vector<size_t>{0, 5, 64, 200}))
      << "forEachSetBit must visit bits in ascending index order so the ack "
         "path produces newly-acked packet ids in fsn order";
}

TEST(ShiftableBitsetTest, ShiftDownRetiresLowEntries) {
  Bitset b;
  b.set(1);
  b.set(70);
  b.set(255);
  b.shiftDown(1);
  // Bit 1 -> 0, 70 -> 69, 255 -> 254; the discarded low bit leaves nothing.
  EXPECT_EQ(setBits(b), (std::vector<size_t>{0, 69, 254}))
      << "shiftDown(1) must move every bit one place toward index 0, modelling "
         "snd_una advancing past one retired packet";
}

TEST(ShiftableBitsetTest, ShiftDownAcrossWordBoundary) {
  Bitset b;
  b.set(64);
  b.set(130);
  b.shiftDown(65);
  // 64 -> -1 (discarded), 130 -> 65.
  EXPECT_EQ(setBits(b), (std::vector<size_t>{65}))
      << "shiftDown by more than a word must carry high-word bits down into "
         "lower words and discard everything below the shift";
}

TEST(ShiftableBitsetTest, ShiftDownByCapacityClears) {
  Bitset b;
  b.set(0);
  b.set(255);
  b.shiftDown(256);
  EXPECT_FALSE(b.any())
      << "shifting down by the full capacity must retire every bit";
}

TEST(ShiftableBitsetTest, AndNotIsolatesNewlySetBits) {
  Bitset incoming; // what this ack reports as sacked
  incoming.set(0);
  incoming.set(1);
  incoming.set(2);
  Bitset already; // what we had already recorded as acked
  already.set(1);
  Bitset newly = incoming;
  newly.andNot(already);
  EXPECT_EQ(setBits(newly), (std::vector<size_t>{0, 2}))
      << "andNot must yield exactly the bits set in the incoming ack that were "
         "not already acked -- the work the ack path must still do";
}

TEST(ShiftableBitsetTest, OrAccumulates) {
  Bitset a;
  a.set(3);
  Bitset b;
  b.set(3);
  b.set(9);
  a |= b;
  EXPECT_EQ(setBits(a), (std::vector<size_t>{3, 9}))
      << "operator|= must union the two bitsets so acked state accumulates "
         "across successive acks";
}

TEST(ShiftableBitsetTest, KeepLowBitsMasksAboveRange) {
  Bitset b;
  b.set(0);
  b.set(63);
  b.set(64);
  b.set(200);
  b.keepLowBits(64);
  EXPECT_EQ(setBits(b), (std::vector<size_t>{0, 63}))
      << "keepLowBits(n) must clear every bit at or above n, bounding the ack "
         "to the packets it actually covers";
}

TEST(ShiftableBitsetTest, CountTrailingOnesStopsAtFirstHole) {
  Bitset b;
  b.set(0);
  b.set(1);
  b.set(2);
  // bit 3 is the hole
  b.set(4);
  EXPECT_EQ(b.countTrailingOnes(), 3u)
      << "countTrailingOnes must measure only the contiguous run from index 0, "
         "which is exactly how far snd_una may advance";
}

TEST(ShiftableBitsetTest, CountTrailingOnesSpansWordBoundary) {
  Bitset b;
  for (size_t i = 0; i < 70; ++i) {
    b.set(i);
  }
  EXPECT_EQ(b.countTrailingOnes(), 70u)
      << "a contiguous run that crosses a 64-bit word boundary must be counted "
         "in full";
}

TEST(ShiftableBitsetTest, CountTrailingOnesEmptyAndFull) {
  Bitset empty;
  EXPECT_EQ(empty.countTrailingOnes(), 0u)
      << "an empty bitset has no leading run, so snd_una cannot advance";
  Bitset full;
  for (size_t i = 0; i < 256; ++i) {
    full.set(i);
  }
  EXPECT_EQ(full.countTrailingOnes(), 256u)
      << "an entirely set bitset's run is the full capacity";
}

TEST(ShiftableBitsetTest, PopcountRangeCountsOnlyWithinBounds) {
  Bitset b;
  b.set(5);
  b.set(10);
  b.set(63);
  b.set(64);
  b.set(200);
  EXPECT_EQ(b.popcountRange(10, 64), 2u)
      << "popcountRange must count bits in [lo, hi) inclusive of lo, exclusive "
         "of hi (10 and 63 here), which is how newly-lost holes are tallied";
  EXPECT_EQ(b.popcountRange(0, 256), 5u)
      << "a full-width range must count every set bit";
  EXPECT_EQ(b.popcountRange(70, 70), 0u) << "an empty range must count nothing";
}

TEST(ShiftableBitsetTest, FromBitVectorMatchesBitPositions) {
  BitVector<256> bv;
  bv.setBit(0);
  bv.setBit(31); // top of first 32-bit word
  bv.setBit(32); // bottom of second 32-bit word -> packed into same uint64
  bv.setBit(64);
  bv.setBit(255);
  Bitset b = Bitset::fromBitVector(bv);
  EXPECT_EQ(setBits(b), (std::vector<size_t>{0, 31, 32, 64, 255}))
      << "fromBitVector must preserve every bit position when packing the "
         "wire's 32-bit words into 64-bit words";
}

TEST(ShiftableBitsetTest, AlignIncomingByShiftDown) {
  // Models aligning an ack's cfsn-relative sack bitmap to snd_una offset.
  BitVector<256> bv;
  bv.setBit(2); // fsn = cfsn+1+2
  bv.setBit(3);
  Bitset b = Bitset::fromBitVector(bv);
  b.shiftDown(2); // shift = snd_una - (cfsn+1) = 2
  EXPECT_EQ(setBits(b), (std::vector<size_t>{0, 1}))
      << "shifting a freshly loaded ack bitmap down by (snd_una - cfsn - 1) "
         "must place snd_una at index 0";
}

} // namespace
} // namespace libsoftmetaroce
