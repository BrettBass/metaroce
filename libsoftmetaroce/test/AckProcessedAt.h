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

#include <cstdint>

namespace libsoftmetaroce {

// Nonzero RTT added to an ACK's echoed send timestamp to model when it is
// processed.
inline constexpr uint32_t kSimulatedAckRttNs = 1000;

// The instant an ACK is modeled as processed: the send timestamp it echoes plus
// one RTT. Tests pass this (not the raw timestamp) as processAckPacket's nowNs,
// so the derived RTT is nonzero and distinct from the packet timestamp.
inline constexpr uint32_t simulatedProcessedAtNs(uint32_t sentTimestampNs) {
  return sentTimestampNs + kSimulatedAckRttNs;
}

} // namespace libsoftmetaroce
