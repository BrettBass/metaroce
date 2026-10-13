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

// Test main: initialize gtest, enable stderr logging, initialize folly,
// then run the tests.
#include <folly/init/Init.h>

#include <glog/logging.h>
#include <gtest/gtest.h>

int main(int argc, char** argv) {
  // InitGoogleTest first: it strips --gtest_* flags (list_tests, filter)
  // that folly::Init would otherwise reject as unknown gflags.
  ::testing::InitGoogleTest(&argc, argv);
  // Log to stderr so CaptureStderr tests observe log output; glog's file
  // default would hide it. Set before parsing so CLI still wins.
  FLAGS_logtostderr = true;
  folly::Init init(&argc, &argv);
  return RUN_ALL_TESTS();
}
