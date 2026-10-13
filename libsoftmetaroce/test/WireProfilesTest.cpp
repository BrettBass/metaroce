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

#include <gtest/gtest.h>

#include <libsoftmetaroce/Constants.h>
#include <libsoftmetaroce/EgressProfile.h>
#include <libsoftmetaroce/EsunProfile.h>
#include <libsoftmetaroce/WireProfiles.h>

namespace libsoftmetaroce {

namespace {

TEST(WireProfilesTest, InstallingOncePerVerbsInstanceIsSafe) {
  installWireProfiles();
  installWireProfiles();

  EXPECT_EQ(
      defaultEgressRegistry().getProfile(kGidTypeEsun).rawEthertype,
      kProvisionalEsunEthertype)
      << "the MetaRoCE-over-ESUN capability should always be installed";
  EXPECT_EQ(
      defaultEgressRegistry().getProfile(kGidTypeMetaRoceEthernet).socketKind,
      SocketKind::RAW)
      << "the Ethernet profile should be installed rather than left to fall "
         "back to the built-in UDP one";
}

} // namespace

} // namespace libsoftmetaroce
