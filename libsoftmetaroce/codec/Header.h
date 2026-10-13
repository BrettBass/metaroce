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

#pragma once
#include <folly/io/Cursor.h>
#include <cstdint>

namespace libsoftmetaroce {

// Minimum rate hint value (used in test assertions)
inline constexpr uint32_t kMinRateHintGbps = 1;
// Default rate hint when creating packets
inline constexpr uint32_t kDefaultRateHintGbps = 10;

class Header {
 public:
  Header() = default;
  Header(const Header& other) = default;
  Header& operator=(const Header& other) = default;
  bool operator==(const Header&) const = default;
  virtual ~Header() = default;

  /*
   * Builds packet into IOBuf's QueueAppender. This throws if there isn't enough
   * room to write the Header.
   */
  virtual void write(folly::io::QueueAppender& appender) const = 0;
};

} // namespace libsoftmetaroce
