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

#include <cstddef>
#include <cstdint>
#include <map>

#include <folly/Random.h>
#include <string>
#include <vector>

namespace facebook::neteng::ai::rdma_gen {

// Fill a buffer with the deterministic, sequential character pattern in
// a-z,A-Z,1-9.
//
// Each MTU-sized segment is filled with one character from kPatternChars.
// |charIndex| tracks position in the character sequence; updated on return.
void fillBufferWithPattern(
    void* buf,
    uint32_t len,
    uint32_t mtuBytes,
    size_t& charIndex);

// Verify a buffer matches the deterministic character pattern.
// |charIndex| tracks position in the character sequence; updated on return.
// Returns true if the buffer matches the expected pattern.
bool verifyBufferPattern(
    const void* buf,
    uint32_t len,
    uint32_t mtuBytes,
    size_t& charIndex);

// Fill a buffer with data from a file pattern (wraps if message > file).
void fillBufferFromFilePattern(
    void* buf,
    uint32_t len,
    const std::vector<uint8_t>& fileData,
    size_t& fileOffset);

// Verify a buffer matches the file pattern (wraps if message > file).
bool verifyBufferFromFilePattern(
    const void* buf,
    uint32_t len,
    const std::vector<uint8_t>& fileData,
    size_t& fileOffset);

void seedPayloadRng(uint64_t seed);

void fillBufferWithRandom(void* buf, uint32_t len);

std::string hexDumpBuffer(const void* buf, uint32_t len, uint32_t maxBytes);

std::vector<uint8_t> loadFilePattern(const std::string& filePath);

// Build metadata map for payload exchange validation.
// Sets "verify_payload_exchange" to "0"/"1", and if patternFile is non-empty,
// computes CRC32C of file content and stores as "pattern_checksum".
std::map<std::string, std::string> buildPayloadMetadata(
    bool verifyPayload,
    const std::string& patternFile);

// Validate payload metadata between local and remote endpoints.
// If either side has verify_payload_exchange=1, then pattern_checksum
// must match across both sides (including both being absent).
// Returns false on mismatch.
bool validatePayloadMetadata(
    const std::map<std::string, std::string>& local,
    const std::map<std::string, std::string>& remote);

} // namespace facebook::neteng::ai::rdma_gen
