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
#include <infiniband/verbs.h>
#include <libsoftmetaroce/MemoryRegions.h>

#include <array>
#include <cstdint>
#include <limits>

namespace libsoftmetaroce {
namespace {

/*
 * Each case turns on the access a region carries or the base it sits at, so
 * the registry and the buffer to register are what they share. The domain is
 * null throughout: these reach MemoryRegions directly, below the verb that
 * decides which domains a caller may use.
 */
class MemoryRegionsTest : public ::testing::Test {
 protected:
  struct ibv_mr* registerBuffer(int access) {
    return regions_.reg_mr(nullptr, buffer_.data(), buffer_.size(), access);
  }

  MemoryRegions regions_;
  std::array<char, 128> buffer_{};
};

TEST_F(MemoryRegionsTest, AZeroLengthAccessNeedsNoRegion) {
  constexpr uint32_t kUnknownKey = 0xDEADBEEF;
  ASSERT_FALSE(
      regions_.isValidLocal(nullptr, buffer_.data(), 1, kUnknownKey, 0))
      << "precondition: no region holds this key, so an access touching memory "
         "is refused";
  EXPECT_TRUE(regions_.isValidLocal(nullptr, buffer_.data(), 0, kUnknownKey, 0))
      << "a zero-length access touches no memory, so refusing it for want of a "
         "region would fail work that reads and writes nothing";
}

/*
 * A zero-based offset arrives from the wire, so one that wraps the addition
 * would resolve back inside the region and pass the range check behind it.
 */
TEST_F(MemoryRegionsTest, AZeroBasedOffsetThatWouldWrapIsRefused) {
  struct ibv_mr* mr = registerBuffer(
      IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_ATOMIC |
      IBV_ACCESS_ZERO_BASED);
  ASSERT_NE(mr, nullptr);
  const auto base = reinterpret_cast<uint64_t>(mr->addr);
  ASSERT_EQ(regions_.targetForRemoteAtomic(nullptr, 0, mr->rkey).address, base)
      << "precondition: an offset of zero resolves to the region's base, or "
         "the refusal below is not the wrap under test";

  const auto wrapped = regions_.targetForRemoteAtomic(
      nullptr, std::numeric_limits<uint64_t>::max(), mr->rkey);
  EXPECT_EQ(wrapped.address, std::numeric_limits<uint64_t>::max())
      << "an offset the base cannot be added to must resolve to no address, "
         "leaving the wire value to stand; wrapping would put a peer back "
         "inside the region it may not reach";
  EXPECT_FALSE(wrapped.permitted)
      << "and an offset pointing to no address must not be one the region "
         "permits";
}

/*
 * Above half the address space a region holds offsets that both fail to
 * resolve and fall inside it, so the range check clears what resolution
 * refused and only the resolution result tells the access apart.
 */
TEST_F(MemoryRegionsTest, AnOffsetTheRangeCheckWouldClearIsStillRefused) {
  constexpr uint64_t kHighBase = uint64_t{1} << 63;
  constexpr uint64_t kOffset = kHighBase + 8;
  // Never dereferenced: registration records the bounds and the access, and
  // the checks under test read only those.
  struct ibv_mr* mr = regions_.reg_mr(
      nullptr,
      // NOLINTNEXTLINE(performance-no-int-to-ptr)
      reinterpret_cast<void*>(kHighBase),
      128,
      IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_ATOMIC |
          IBV_ACCESS_ZERO_BASED);
  ASSERT_NE(mr, nullptr);
  const auto target =
      regions_.targetForRemoteAtomic(nullptr, kOffset, mr->rkey);
  ASSERT_EQ(target.address, kOffset)
      << "precondition: the offset must be one the base cannot be added to, so "
         "that the wire value stands unresolved, or the refusal below is not "
         "the one under test";
  // The same bounds, but not zero-based, so the offset is the address itself.
  struct ibv_mr* const absolute = regions_.reg_mr(
      nullptr,
      // NOLINTNEXTLINE(performance-no-int-to-ptr)
      reinterpret_cast<void*>(kHighBase),
      128,
      IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_ATOMIC);
  ASSERT_NE(absolute, nullptr);
  ASSERT_TRUE(regions_.targetForRemoteAtomic(nullptr, kOffset, absolute->rkey)
                  .permitted)
      << "precondition: the range check must clear this offset where no base "
         "is added to it, or the refusal below is the range check's rather "
         "than resolution's";

  EXPECT_FALSE(target.permitted)
      << "an offset that resolves to no address must be refused on that "
         "account; a region based this high leaves the range check agreeing "
         "with a peer that pointed to no address at all";
}

} // namespace
} // namespace libsoftmetaroce
