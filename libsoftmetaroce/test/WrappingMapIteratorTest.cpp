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

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <libsoftmetaroce/WrappingMapIterator.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace libsoftmetaroce;
using namespace testing;

class WrappingMapIteratorTest : public ::testing::Test {
 protected:
  void SetUp() override {}
};

/*
 * Tests the WrappingMapIterator functionality.
 *
 * This test verifies that the iterateFrom function correctly handles:
 * 1. Iteration starting from a specified key value
 * 2. Wrapping behavior when reaching the end of the key space
 * 3. Correct ordering of iteration results
 * 4. Edge cases like empty maps and single elements
 */
TEST_F(WrappingMapIteratorTest, BasicWrappingBehavior) {
  // STEP 1: Test basic wrapping behavior with uint8_t keys and string values
  // --------------------------------------
  std::map<uint8_t, std::unique_ptr<std::string>> testMap;

  // Create values for testing - string representation of the key
  testMap[0] = std::make_unique<std::string>("0");
  testMap[1] = std::make_unique<std::string>("1");
  testMap[253] = std::make_unique<std::string>("253");
  testMap[254] = std::make_unique<std::string>("254");
  testMap[255] = std::make_unique<std::string>("255");

  // Test iteration starting from 254
  // Expected order: [255, 0, 1, 253, 254]
  std::vector<uint8_t> expectedKeys = {255, 0, 1, 253, 254};
  std::vector<std::string> expectedValues = {"255", "0", "1", "253", "254"};

  std::vector<uint8_t> actualKeys;
  std::vector<std::string> actualValues;

  for (const auto [key, valuePtr] :
       iterateFrom(testMap, static_cast<uint8_t>(254))) {
    actualKeys.push_back(key);
    actualValues.push_back(*valuePtr);
  }

  EXPECT_THAT(actualKeys, ElementsAreArray(expectedKeys))
      << "Keys should be iterated in wrapping order";
  EXPECT_THAT(actualValues, ElementsAreArray(expectedValues))
      << "Values should match the expected order";

  // STEP 2: Test iteration starting from 255
  // --------------------------------------
  // Starting from 255 should give order: [0, 1, 253, 254, 255]
  expectedKeys = {0, 1, 253, 254, 255};
  expectedValues = {"0", "1", "253", "254", "255"};

  actualKeys.clear();
  actualValues.clear();

  for (const auto [key, valuePtr] :
       iterateFrom(testMap, static_cast<uint8_t>(255))) {
    actualKeys.push_back(key);
    actualValues.push_back(*valuePtr);
  }

  EXPECT_THAT(actualKeys, ElementsAreArray(expectedKeys))
      << "Keys should be iterated starting from 0";
  EXPECT_THAT(actualValues, ElementsAreArray(expectedValues))
      << "Values should match the expected order";

  // STEP 3: Test iteration starting from 0
  // --------------------------------------
  // Starting from 0 should give order: [1, 253, 254, 255, 0]
  expectedKeys = {1, 253, 254, 255, 0};
  expectedValues = {"1", "253", "254", "255", "0"};

  actualKeys.clear();
  actualValues.clear();

  for (const auto [key, valuePtr] :
       iterateFrom(testMap, static_cast<uint8_t>(0))) {
    actualKeys.push_back(key);
    actualValues.push_back(*valuePtr);
  }

  EXPECT_THAT(actualKeys, Eq(expectedKeys))
      << "Keys should be iterated starting from 1";
  EXPECT_THAT(actualValues, Eq(expectedValues))
      << "Values should match the expected order";
}

/*
 * Tests edge cases for the WrappingMapIterator.
 */
TEST_F(WrappingMapIteratorTest, EdgeCases) {
  // Test with empty map
  std::map<uint16_t, std::unique_ptr<std::string>> emptyMap;
  int count = 0;
  for (const auto entry : iterateFrom(emptyMap, static_cast<uint16_t>(0))) {
    (void)entry;
    count++;
  }
  EXPECT_THAT(count, Eq(0)) << "Empty map should produce no iterations";

  // Test with single element
  std::map<uint16_t, std::unique_ptr<std::string>> singleMap;
  singleMap[42] = std::make_unique<std::string>("42");

  std::vector<uint16_t> actualKeys;
  std::vector<std::string> actualValues;

  for (const auto [key, valuePtr] :
       iterateFrom(singleMap, static_cast<uint16_t>(41))) {
    actualKeys.push_back(key);
    actualValues.push_back(*valuePtr);
  }

  ASSERT_THAT(actualKeys, ElementsAre(42))
      << "Single element map should produce one iteration with the correct key";
  EXPECT_THAT(actualValues, ElementsAre("42"))
      << "Single element should have correct value";

  // Test starting from key that exists in map
  actualKeys.clear();
  actualValues.clear();

  for (const auto [key, valuePtr] :
       iterateFrom(singleMap, static_cast<uint16_t>(42))) {
    actualKeys.push_back(key);
    actualValues.push_back(*valuePtr);
  }

  ASSERT_THAT(actualKeys, ElementsAre(42))
      << "Starting from existing key should still produce one iteration with the same key";
  EXPECT_THAT(actualValues, ElementsAre("42"))
      << "Starting from existing key should still produce the same value";
}
