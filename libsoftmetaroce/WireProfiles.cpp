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

#include <libsoftmetaroce/WireProfiles.h>

#include <libsoftmetaroce/Constants.h>
#include <libsoftmetaroce/EsunProfile.h>
#include <libsoftmetaroce/GidDiscovery.h>

#include <folly/synchronization/CallOnce.h>
#include <glog/logging.h>
#include <utility>

namespace libsoftmetaroce {

void installWireProfiles() {
  // The registries are process-wide, but this entry point runs once per verbs
  // instance, so register the built-in profiles only once.
  static folly::once_flag once;
  folly::call_once(
      once, [] {
        auto esunAddresses = configuredEsunAddresses();
        const bool esunRegistered = defaultEgressRegistry().registerProfile(
            kGidTypeEsun, makeEsunProfile());
        CHECK(esunRegistered)
            << "the MetaRoCE-over-ESUN egress profile is already registered";
        registerEsunGidRule(defaultGidRuleRegistry(), std::move(esunAddresses));

        const bool ethernetRegistered = defaultEgressRegistry().registerProfile(
            kGidTypeMetaRoceEthernet, makeMetaRoceEthernetProfile());
        CHECK(ethernetRegistered)
            << "the built-in Ethernet profile is already registered";
      });
}

void attachConnectionObservers(const ConnectionObserverRegistrar&) {}

} // namespace libsoftmetaroce
