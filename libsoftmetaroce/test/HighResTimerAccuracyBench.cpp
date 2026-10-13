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

// Reports through LOG(INFO), so run with GLOG_logtostderr=1 GLOG_minloglevel=0.

#include <folly/init/Init.h>
#include <libsoftmetaroce/test/TimerDelayProbe.h>

#include <chrono>
#include <string>

namespace libsoftmetaroce {
namespace {

constexpr size_t kOneTimer = 1;
constexpr size_t kFiringsOneTimer = 200;
constexpr size_t kManyTimers = 80;
constexpr size_t kFiringsPerManyTimer = 50;

void measureAndReport(size_t timers, size_t firings) {
  reportOvershoot(
      "timers=" + std::to_string(timers) +
          " firings=" + std::to_string(firings),
      measureOvershoot(timers, firings, kAccuracyDelay));
}

} // namespace
} // namespace libsoftmetaroce

int main(int argc, char** argv) {
  folly::Init init(&argc, &argv);

  libsoftmetaroce::measureAndReport(
      libsoftmetaroce::kOneTimer, libsoftmetaroce::kFiringsOneTimer);
  libsoftmetaroce::measureAndReport(
      libsoftmetaroce::kManyTimers, libsoftmetaroce::kFiringsPerManyTimer);
  return 0;
}
