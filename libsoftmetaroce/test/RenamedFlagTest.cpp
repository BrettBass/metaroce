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

#include <gflags/gflags.h>
#include <gtest/gtest.h>
#include <libsoftmetaroce/RenamedFlag.h>
#include <stdexcept>
#include <string>

static bool acceptAny(const char*, uint32_t) {
  return true;
}

// A pair standing in for a setting midway through a rename, defined the way
// production pairs are.
LSMR_DEFINE_RENAMED_FLAG(
    uint32,
    lsmr_test_renamed_current,
    lsmr_test_renamed_deprecated,
    7,
    &acceptAny,
    "current name, for tests");

namespace libsoftmetaroce {
namespace {

constexpr const char* kCurrent = "lsmr_test_renamed_current";
constexpr const char* kDeprecated = "lsmr_test_renamed_deprecated";

uint32_t resolve() {
  return LSMR_RESOLVE_RENAMED_FLAG(
      lsmr_test_renamed_current, lsmr_test_renamed_deprecated);
}

class RenamedFlagTest : public ::testing::Test {
 protected:
  void set(const char* name, uint32_t value) {
    gflags::SetCommandLineOption(name, std::to_string(value).c_str());
  }

 private:
  // Restores the set-or-not bit along with the value, so a case starts with
  // both names untouched however the case before it left them.
  gflags::FlagSaver saver_;
};

TEST_F(RenamedFlagTest, UntouchedPairResolvesToTheSharedDefault) {
  EXPECT_EQ(resolve(), 7u)
      << "an operator who names neither flag should get the compiled-in "
         "default rather than a value the resolution invented";
}

TEST_F(RenamedFlagTest, BothNamesTakeTheSameDefault) {
  EXPECT_EQ(flagDefault(kCurrent), flagDefault(kDeprecated))
      << "the two names are one setting, so defaults that drift would make the "
         "answer depend on which name a caller happens to omit";
}

TEST_F(RenamedFlagTest, FormerNameAloneStillReachesTheReader) {
  set(kDeprecated, 11);
  EXPECT_EQ(resolve(), 11u)
      << "the former name has to keep working through the rename, or every "
         "caller still passing it drops silently back to the default";
}

TEST_F(RenamedFlagTest, CurrentNameAloneTakesEffect) {
  set(kCurrent, 13);
  EXPECT_EQ(resolve(), 13u)
      << "the new name must work on its own, or nobody can migrate off the "
         "former one";
}

TEST_F(RenamedFlagTest, BothNamesAgreeingIsNotAConflict) {
  set(kCurrent, 17);
  set(kDeprecated, 17);
  EXPECT_EQ(resolve(), 17u)
      << "a config naming the setting twice with one value says something "
         "unambiguous, so it should resolve rather than fail the caller";
}

TEST_F(RenamedFlagTest, BothNamesDisagreeingFails) {
  set(kCurrent, 19);
  set(kDeprecated, 23);
  EXPECT_THROW(resolve(), std::runtime_error)
      << "honouring one name and discarding the other is how a stale config "
         "goes on passing while meaning something its author did not write";
}

TEST_F(RenamedFlagTest, FormerNameSetToTheDefaultValueStillCounts) {
  set(kDeprecated, 7);
  set(kCurrent, 29);
  EXPECT_THROW(resolve(), std::runtime_error)
      << "gflags marks a flag assigned even when the value chosen equals the "
         "default, and reading it as untouched would discard a real setting";
}

TEST_F(RenamedFlagTest, EveryFormerNameGetsItsOwnWarning) {
  EXPECT_TRUE(claimRenamedFlagWarning("lsmr_test_warned_first"))
      << "the first claim on a name must succeed, or an operator still passing "
         "the former name is never told to move off it";
  EXPECT_FALSE(claimRenamedFlagWarning("lsmr_test_warned_first"))
      << "a name that has already been warned about must not warn again, or "
         "every flow a queue pair adds repeats the same line";
  EXPECT_TRUE(claimRenamedFlagWarning("lsmr_test_warned_second"))
      << "a second pair carries its own warning; sharing one would leave an "
         "operator using the former name of all but the first pair untold";
}

} // namespace
} // namespace libsoftmetaroce
