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

// Plain gtest main for the interposer ABI test: unlike the default test
// main, it does not initialize folly. The interposer under test owns the
// only folly::Init in the process, exactly as with a real C consumer, so
// backend teardown runs before singleton teardown by construction order.
// Also serves the EnvGidsHonoredInFreshProcess child mode: when argv carries
// kPreloadEnvChildArg, run the child check directly instead of the suite,
// before InitGoogleTest sees a flag it would reject.

#include <gtest/gtest.h>

#include <cstring>

#include "PreloadEnvChild.h"

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], kPreloadEnvChildArg) == 0) {
      return RunPreloadEnvChild();
    }
  }
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
