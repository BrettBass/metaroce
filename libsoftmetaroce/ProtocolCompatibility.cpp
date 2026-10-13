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

#include <libsoftmetaroce/ProtocolCompatibility.h>

DEFINE_bool(
    libsoftmetaroce_dont_send_brnr,
    false,
    "Discard received frame that would generate BRNR");

DEFINE_bool(
    libsoftmetaroce_emit_send_last,
    false,
    "Mark the final packet of an ordinary send with the Send Last opcode");

DEFINE_bool(
    libsoftmetaroce_omit_padding,
    false,
    "Skip zero-padding wire payloads to the next 4-byte boundary. "
    "Set to disable padding for testing non-padded interop.");

DEFINE_bool(
    libsoftmetaroce_validate_crc,
    true,
    "Validate CRC on received packets");
