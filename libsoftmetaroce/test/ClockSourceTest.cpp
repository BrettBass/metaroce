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
#include <libsoftmetaroce/ClockSource.h>

#include <chrono>
#include <memory>

namespace libsoftmetaroce {
// we declare the flag here so we can set it and test getClocSourceInstance()
// and getNowNs()

// NOLINTNEXTLINE(facebook-hte-PreventUseOfDeclareGflag)
DECLARE_string(libsoftmetaroce_ptp_dev);
} // namespace libsoftmetaroce

namespace libsoftmetaroce {

class ClockSourceTest : public ::testing::Test {
 protected:
  void SetUp() override {}
  void TearDown() override {}
  void setClockSourceSystem() {
    FLAGS_libsoftmetaroce_ptp_dev = "";
  }
  void setClockSourcePTP() {
    FLAGS_libsoftmetaroce_ptp_dev = ptpDev;
  }
  // PTPClockSource constructor should throw if fd isn't a valid ptp device
  // since we do not have a /dev/ptp device on test machines, just use /dev/null
  // so open doesn't complain. A valid device is covered by the
  // clocksource_timerfd_vmtest target, which runs against /dev/ptp_kvm.
  const std::string ptpDev = "/dev/null";
};

// Test ClockSource base class
TEST_F(ClockSourceTest, BaseClockSource) {
  // set flag
  auto const clockSource = std::make_unique<ClockSource>();
  ASSERT_NE(clockSource, nullptr);

  // The returned clock should have CLOCK_REALTIME when no PTP is configured
  EXPECT_EQ(clockSource->GetClockId(), CLOCK_REALTIME);

  // check that the clock is valid
  auto before = std::chrono::high_resolution_clock::now();
  auto now = clockSource->now();
  auto after = std::chrono::high_resolution_clock::now();

  // The time from ClockSource should be between before and after
  auto nowNs = now.time_since_epoch().count();
  auto beforeNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      before.time_since_epoch())
                      .count();
  auto afterNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                     after.time_since_epoch())
                     .count();

  EXPECT_GE(nowNs, beforeNs);
  EXPECT_LE(nowNs, afterNs);
}

TEST_F(ClockSourceTest, PTPClockSource) {
  auto clockSource = std::make_unique<PTPClockSource>(ptpDev);
  ASSERT_NE(clockSource, nullptr);

  // clockid would be anything but CLOCK_REALTIME (i.e. 0)
  ASSERT_NE(clockSource->GetClockId(), CLOCK_REALTIME);

  // this should cause exception since we don't have a ptp device
  EXPECT_THROW(clockSource->now(), std::runtime_error);
}

// validate getClockSourceInstance() returns the correct clock source instance
TEST_F(ClockSourceTest, GetClockSourceInstanceDefault) {
  setClockSourceSystem();
  const auto clockSource = getClockSourceInstance();
  ASSERT_NE(clockSource, nullptr);
  const auto& ref = *clockSource;
  EXPECT_EQ(typeid(ref), typeid(ClockSource));
}

TEST_F(ClockSourceTest, GetClockSourceInstancePTP) {
  setClockSourcePTP();
  const auto clockSource = getClockSourceInstance();
  ASSERT_NE(clockSource, nullptr);
  const auto& ref = *clockSource;
  EXPECT_EQ(typeid(ref), typeid(PTPClockSource));
}

TEST_F(ClockSourceTest, GetNowNsDefault) {
  setClockSourceSystem();
  EXPECT_NO_THROW(getNowNs());
}

TEST_F(ClockSourceTest, GetNowNsPTPClock) {
  // since we set ptp dev to /dev/null, getNowNs should throw
  setClockSourcePTP();
  EXPECT_THROW(getNowNs(), std::runtime_error);
}

} // namespace libsoftmetaroce
