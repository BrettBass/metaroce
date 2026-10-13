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

#include <libsoftmetaroce/HighResTimer.h>

#include <folly/system/ThreadName.h>
#include <map>
#include <mutex>
#include <string>

namespace libsoftmetaroce {

namespace {
std::string nextTimerName(std::string_view prefix) {
  static std::mutex mutex;
  static std::map<std::string, uint32_t> nextIdByPrefix;
  std::lock_guard<std::mutex> lock(mutex);
  std::string name(prefix);
  return name + std::to_string(nextIdByPrefix[name]++);
}
} // namespace

HighResTimer::HighResTimer(std::string_view namePrefix) {
  timeoutMgr_ = std::make_unique<TimeoutManager>(&evb_);
  wheelTimer_ = folly::HHWheelTimerHighRes::newTimer(timeoutMgr_.get());
  auto name = nextTimerName(namePrefix);
  thread_ = std::make_unique<std::thread>([this, name = std::move(name)]() {
    folly::setThreadName(name);
    evb_.loopForever();
  });
}

} // namespace libsoftmetaroce
