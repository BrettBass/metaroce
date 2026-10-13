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

#include <folly/io/async/AsyncUDPSocket.h>
#include <folly/io/async/EventBase.h>
#include <infiniband/verbs.h>
#include <libsoftmetaroce/PacketModifierSocket.h>
#include <functional>
#include <memory>

namespace libsoftmetaroce {

// Create a PacketModifierSocket for the given GID entry. For Ethernet
// entries this creates a RawPacketSocket over AF_PACKET; for all
// others it creates a UdpPacketSocket via socketMaker.
// Never returns null; throws on failure.
std::unique_ptr<PacketModifierSocket> makePacketModifierSocket(
    folly::EventBase* evb,
    const ibv_gid_entry& entry,
    const std::function<std::pair<
        std::unique_ptr<folly::AsyncUDPSocket>,
        std::unique_ptr<Modifier>>(folly::EventBase*)>& socketMaker);

// Check whether a MetaRoCE MTU is supported for the given GID entry.
// Returns true if the MTU fits, false with an error log if not.
// UDP entries always return true (kernel handles fragmentation).
bool validateMtuForEntry(const ibv_gid_entry& entry, uint16_t mtu);

} // namespace libsoftmetaroce
