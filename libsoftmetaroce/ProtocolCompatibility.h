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

// Only options that change what libsoftmetaroce puts on the wire, or accepts
// from it, to interoperate with a peer that implements less of the spec.

#pragma once

#include <gflags/gflags.h>

DECLARE_bool(libsoftmetaroce_dont_send_brnr);
DECLARE_bool(libsoftmetaroce_emit_send_last);
DECLARE_bool(libsoftmetaroce_omit_padding);
DECLARE_bool(libsoftmetaroce_validate_crc);
