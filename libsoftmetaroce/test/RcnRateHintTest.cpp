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

#include <libsoftmetaroce/RcnRateHint.h>

#include <gtest/gtest.h>

namespace libsoftmetaroce {

TEST(RcnRateHintTest, RateDividedAmongActiveQps) {
  RcnRateHint rateHint(100);
  EXPECT_EQ(rateHint.getPerQpRateGbps(), 100) << "0 active QPs: full NIC rate";

  rateHint.onQpBecameActive();
  EXPECT_EQ(rateHint.getPerQpRateGbps(), 100) << "1 active QP: full NIC rate";

  rateHint.onQpBecameActive();
  EXPECT_EQ(rateHint.getPerQpRateGbps(), 50) << "2 active QPs: 100/2";

  rateHint.onQpBecameActive();
  EXPECT_EQ(rateHint.getPerQpRateGbps(), 34) << "3 active QPs: ceil(100/3)";

  rateHint.onQpBecameInactive();
  EXPECT_EQ(rateHint.getPerQpRateGbps(), 50) << "back to 2 active QPs: 100/2";

  rateHint.onQpBecameInactive();
  rateHint.onQpBecameInactive();
  EXPECT_EQ(rateHint.getPerQpRateGbps(), 100) << "0 active QPs: full NIC rate";
}

TEST(RcnRateHintTest, CeilDivisionWithNonRoundRate) {
  RcnRateHint rateHint(10);
  rateHint.onQpBecameActive();
  rateHint.onQpBecameActive();
  rateHint.onQpBecameActive();
  EXPECT_EQ(rateHint.getPerQpRateGbps(), 4) << "ceil(10/3) = 4";
}

} // namespace libsoftmetaroce
