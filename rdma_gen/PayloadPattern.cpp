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

#include "rdma_gen/PayloadPattern.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <string_view>

#include <fmt/format.h>
#include <folly/Random.h>
#include <folly/hash/Checksum.h>
#include <glog/logging.h>

namespace facebook::neteng::ai::rdma_gen {

// a-z, A-Z, 1-9: 61 characters total
const std::string_view kPatternChars =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ123456789";

void fillBufferWithPattern(
    void* buf,
    uint32_t len,
    uint32_t mtuBytes,
    size_t& charIndex) {
  CHECK(buf != nullptr);
  CHECK_GT(mtuBytes, 0u);

  auto* dst = static_cast<uint8_t*>(buf);
  for (uint32_t offset = 0; offset < len; offset += mtuBytes) {
    char ch = kPatternChars[charIndex % kPatternChars.size()];
    // Last packet may be smaller than mtu
    uint32_t segLen = std::min(mtuBytes, len - offset);
    std::memset(dst + offset, ch, segLen);
    charIndex = (charIndex + 1) % kPatternChars.size();
  }
}

bool verifyBufferPattern(
    const void* buf,
    uint32_t len,
    uint32_t mtuBytes,
    size_t& charIndex) {
  CHECK(buf != nullptr);
  CHECK_GT(mtuBytes, 0u);

  const auto* src = static_cast<const uint8_t*>(buf);
  for (uint32_t offset = 0; offset < len; offset += mtuBytes) {
    char expected = kPatternChars[charIndex % kPatternChars.size()];
    // Last packet may be smaller than mtu
    uint32_t segLen = std::min(mtuBytes, len - offset);
    for (uint32_t i = 0; i < segLen; i++) {
      if (src[offset + i] != static_cast<uint8_t>(expected)) {
        LOG(ERROR) << fmt::format(
            "Data verification failed at byte {}: expected '{}' ({:#x}), "
            "got {:#x}",
            offset + i,
            expected,
            static_cast<uint8_t>(expected),
            src[offset + i]);
        return false;
      }
    }
    charIndex = (charIndex + 1) % kPatternChars.size();
  }
  return true;
}

namespace {
folly::Random::DefaultGenerator& payloadRng() {
  static auto rng = folly::Random::create();
  return rng;
}
} // namespace

void seedPayloadRng(uint64_t seed) {
  payloadRng().seed(static_cast<uint32_t>(seed));
}

void fillBufferWithRandom(void* buf, uint32_t len) {
  CHECK(buf != nullptr);

  auto& rng = payloadRng();
  auto* dst = static_cast<uint64_t*>(buf);
  const uint32_t fullWords = len / sizeof(uint64_t);
  for (uint32_t i = 0; i < fullWords; ++i) {
    dst[i] = folly::Random::rand64(rng);
  }

  auto* tail = static_cast<uint8_t*>(buf) + fullWords * sizeof(uint64_t);
  const uint32_t remainingBytes = len % sizeof(uint64_t);
  for (uint32_t i = 0; i < remainingBytes; ++i) {
    tail[i] = static_cast<uint8_t>(folly::Random::rand32(rng));
  }
}

std::string hexDumpBuffer(const void* buf, uint32_t len, uint32_t maxBytes) {
  CHECK(buf != nullptr);
  const auto* src = static_cast<const uint8_t*>(buf);
  const uint32_t dumpLen = std::min(len, maxBytes);
  std::string result;
  result.reserve(dumpLen * 3);
  for (uint32_t i = 0; i < dumpLen; ++i) {
    if (i > 0) {
      result.push_back(' ');
    }
    result.append(fmt::format("{:02x}", src[i]));
  }
  if (dumpLen < len) {
    result.append(fmt::format(" ... ({} bytes total)", len));
  }
  return result;
}

void fillBufferFromFilePattern(
    void* buf,
    uint32_t len,
    const std::vector<uint8_t>& fileData,
    size_t& fileOffset) {
  CHECK(buf != nullptr);
  CHECK(!fileData.empty());
  CHECK_LT(fileOffset, fileData.size()) << "File offset is out of range";

  auto* dst = static_cast<uint8_t*>(buf);
  for (size_t written = 0; written < len; /* written += toCopy */) {
    const size_t toCopy = std::min(
        fileData.size() - fileOffset, static_cast<size_t>(len - written));
    std::memcpy(dst + written, fileData.data() + fileOffset, toCopy);
    fileOffset = (fileOffset + toCopy) % fileData.size();
    written += toCopy;
  }
}

bool verifyBufferFromFilePattern(
    const void* buf,
    uint32_t len,
    const std::vector<uint8_t>& fileData,
    size_t& fileOffset) {
  CHECK(buf != nullptr);
  CHECK(!fileData.empty());
  CHECK_LT(fileOffset, fileData.size()) << "File offset is out of range";

  const auto* src = static_cast<const uint8_t*>(buf);
  for (size_t verified = 0; verified < len; /* verified += toCompare */) {
    const size_t toCompare = std::min(
        fileData.size() - fileOffset, static_cast<size_t>(len - verified));
    if (std::memcmp(src + verified, fileData.data() + fileOffset, toCompare) !=
        0) {
      LOG(ERROR) << fmt::format(
          "Data verification failed at buffer offset {} "
          "(file offset {}, chunk size {})",
          verified,
          fileOffset,
          toCompare);
      return false;
    }
    fileOffset = (fileOffset + toCompare) % fileData.size();
    verified += toCompare;
  }
  return true;
}

std::vector<uint8_t> loadFilePattern(const std::string& filePath) {
  std::ifstream file(filePath, std::ios::binary | std::ios::ate);
  CHECK(file.is_open()) << fmt::format(
      "Failed to open payload pattern file: {}", filePath);
  auto size = file.tellg();
  CHECK_GT(size, 0) << fmt::format(
      "Payload pattern file is empty: {}", filePath);
  file.seekg(0);
  std::vector<uint8_t> data(static_cast<size_t>(size));
  file.read(reinterpret_cast<char*>(data.data()), size);
  CHECK(!file.fail()) << fmt::format(
      "Failed to read payload pattern file: {}", filePath);
  return data;
}

std::map<std::string, std::string> buildPayloadMetadata(
    bool verifyPayload,
    const std::string& patternFile) {
  std::map<std::string, std::string> metadata;
  metadata["verify_payload_exchange"] = verifyPayload ? "1" : "0";
  if (!patternFile.empty()) {
    auto fileData = loadFilePattern(patternFile);
    uint32_t crc = folly::crc32c(fileData.data(), fileData.size());
    metadata["pattern_checksum"] = std::to_string(crc);
  }
  return metadata;
}

bool validatePayloadMetadata(
    const std::map<std::string, std::string>& local,
    const std::map<std::string, std::string>& remote) {
  auto getVal = [](const std::map<std::string, std::string>& m,
                   const std::string& key) -> std::string {
    auto it = m.find(key);
    return it != m.end() ? it->second : "";
  };

  bool localVerify = getVal(local, "verify_payload_exchange") == "1";
  bool remoteVerify = getVal(remote, "verify_payload_exchange") == "1";
  const std::string localChecksum = getVal(local, "pattern_checksum");
  const std::string remoteChecksum = getVal(remote, "pattern_checksum");

  const auto verifyEnabledStr = [](const bool& val) {
    if (val) {
      return "enabled";
    }
    return "disabled";
  };

  if (localVerify != remoteVerify) {
    LOG(ERROR) << fmt::format(
        "Payload verification flags mismatch - those must be symmetric: local={}, remote={}",
        verifyEnabledStr(localVerify),
        verifyEnabledStr(remoteVerify));
    return false;
  }
  // If local or remote side verifies, the pattern file (or lack of it)
  // must be the same at both sides for the verification to succeed (or
  // correctly fail with a true positive)
  if (localVerify || remoteVerify) {
    if (localChecksum != remoteChecksum) {
      LOG(ERROR) << fmt::format(
          "Payload verification requested, but there is a payload pattern file "
          "checksum mismatch: local={} remote={}. "
          "Both endpoints must use the same --payload_exchange_pattern "
          "file when --verify_payload_exchange is enabled, or not use the "
          "--payload_exchange_pattern flag at all.",
          localChecksum.empty() ? std::string_view("unused")
                                : std::string_view(localChecksum),
          remoteChecksum.empty() ? std::string_view("unused")
                                 : std::string_view(remoteChecksum));
      return false;
    }
  }
  return true;
}

} // namespace facebook::neteng::ai::rdma_gen
