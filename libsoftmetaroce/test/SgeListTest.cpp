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
#include <libsoftmetaroce/SgeList.h>

#include <array>
#include <cstdint>
#include <limits>

namespace libsoftmetaroce {
namespace {

TEST(SgeListTest, AnEmptyListIsValidAndHoldsNoBytes) {
  const auto list = validatedSgeList(nullptr, 0);
  ASSERT_TRUE(list.has_value())
      << "a zero-length send or receive may post no sges at all";
  EXPECT_TRUE(list->sges.empty())
      << "resolving an empty list must access no SGE";
  EXPECT_EQ(list->totalLength, 0u)
      << "posting no SGEs must remain a zero-length operation";
}

TEST(SgeListTest, TheTotalLengthSumsEverySge) {
  std::array<ibv_sge, kMaxSgesPerWorkRequest> sges{};
  for (ibv_sge& sge : sges) {
    sge.length = 16;
  }
  const auto list = validatedSgeList(sges.data(), sges.size());
  ASSERT_TRUE(list.has_value())
      << "precondition: a list at the device limit is well formed";
  EXPECT_EQ(list->sges.size(), sges.size())
      << "the list must view every posted sge";
  EXPECT_EQ(list->totalLength, 16 * kMaxSgesPerWorkRequest)
      << "the message is every sge's bytes back to back";
}

TEST(SgeListTest, AMalformedListIsRefused) {
  std::array<ibv_sge, kMaxSgesPerWorkRequest + 1> sges{};
  EXPECT_FALSE(validatedSgeList(sges.data(), -1).has_value())
      << "a negative count names no list";
  EXPECT_FALSE(validatedSgeList(nullptr, 1).has_value())
      << "a count without a list would dereference null";
  EXPECT_FALSE(validatedSgeList(sges.data(), sges.size()).has_value())
      << "the device reports kMaxSgesPerWorkRequest, and a post must not "
         "exceed what it reports";
}

TEST(SgeListTest, ATotalPastWhatAMessageHoldsIsRefused) {
  if (kMaxSgesPerWorkRequest < 2) {
    GTEST_SKIP() << "one sge's uint32_t length cannot overflow the total";
  }
  std::array<ibv_sge, 2> sges{{
      {.length = std::numeric_limits<uint32_t>::max()},
      {.length = 1},
  }};
  EXPECT_FALSE(validatedSgeList(sges.data(), sges.size()).has_value())
      << "the message length is a uint32_t, so the sum would wrap";
}

} // namespace
} // namespace libsoftmetaroce
