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

#include <libsoftmetaroce/EgressProfile.h>

#include <folly/IPAddress.h>
#include <cstdint>
#include <set>

namespace libsoftmetaroce {

class GidRuleRegistry;

// Starts a separate allocation range from the built-in transport types.
inline constexpr uint32_t kGidTypeEsun = 200;

EgressProfile makeEsunProfile();

// Parses and validates --libsoftmetaroce_esun_gids.
std::set<folly::IPAddress> configuredEsunAddresses();

// Empty address sets are a no-op. Otherwise, registers a rule that classifies
// exactly these addresses.
void registerEsunGidRule(
    GidRuleRegistry& rules,
    std::set<folly::IPAddress> addresses);

} // namespace libsoftmetaroce
