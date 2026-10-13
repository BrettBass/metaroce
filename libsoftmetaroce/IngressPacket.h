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

#include <folly/IPAddress.h>
#include <folly/SocketAddress.h>
#include <memory>
#include "libsoftmetaroce/codec/Packet.h"

namespace libsoftmetaroce {

// Conveys a buffer and metadata for a packet that is received.
struct IngressPacket {
  std::unique_ptr<Packet> packet;
  // For UDP: the kernel-provided source IP and port.
  // For raw-socket transports: a synthesized address from the source MAC
  // via RawEncapsulation::resolveSrcAddr, with port 0. The Receiver
  // overrides the port with metaRoceSendToPort_ before sending replies.
  const folly::SocketAddress sourceSocketAddress;
  const uint8_t tos;
};

} // namespace libsoftmetaroce
