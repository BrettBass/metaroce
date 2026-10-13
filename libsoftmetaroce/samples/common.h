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

#include <functional>

#include <gflags/gflags.h>

#include <folly/IPAddressV6.h>

#include <libsoftmetaroce/MetaRoceVerbs.h>
#include <libsoftmetaroce/samples/tag_metadata.h>

DECLARE_uint32(num_tags);

inline constexpr auto kLengthBytes = 120000;

// Ensure that the number of Tags is in the range [1, 256].
bool validateNumTags(const char* flagname, uint32_t value);

// Creates the QP and its initial Tag using ibv_create_qp(), then create any
// additional Tags using ibv_create_qp_ex().
std::vector<struct TagMetadata<kLengthBytes>> setupQpAndTags(
    libsoftmetaroce::MetaRoceVerbs& verbs,
    struct ibv_context* context,
    struct ibv_pd* pd,
    struct ibv_cq* cq,
    const folly::IPAddressV6& remoteAddr);

// Creates a memory region (ibv_mr) and scatter-gather list (ibv_sge) for this
// Tag and installs them in the TagMetadata struct.
void setupMemoryRegion(
    libsoftmetaroce::MetaRoceVerbs& verbs,
    struct ibv_pd* pd,
    struct TagMetadata<kLengthBytes>& tm);

// Polls until we get the expected number of completions.
void poll_cq_loop_until_completed(
    libsoftmetaroce::MetaRoceVerbs& verbs,
    struct ibv_cq* cq,
    int expected_wc);

void teardown(
    libsoftmetaroce::MetaRoceVerbs& verbs,
    std::vector<struct TagMetadata<kLengthBytes>>& tag_metadatas,
    struct ibv_pd* pd,
    struct ibv_cq* cq);

// Main executor that creates the QP and Tags, posts WQEs, and waits for them to
// complete. The logic for posting a WQEs is generic via the postFunc parameter.
// Returns 0 on success, or 1 otherwise.
int executeCommon(
    const folly::IPAddressV6& remoteAddr,
    const std::function<
        int(libsoftmetaroce::MetaRoceVerbs&,
            struct TagMetadata<kLengthBytes>&)>& postFunc);
