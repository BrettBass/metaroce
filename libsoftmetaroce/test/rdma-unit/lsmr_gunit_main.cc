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

// Custom rdma-unit-test entry point for the libsoftmetaroce backend: registers
// the MetaRoCE introspection and adapts the folly/absl init the suite needs.
// See DESIGN.md.

#include <folly/Singleton.h>

#include <unistd.h>
#include <cstdint>
#include <cstdio>

#include "absl/debugging/failure_signal_handler.h"
#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/log/initialize.h"
#include "gtest/gtest.h"

#include "public/flags.h"

#include <libsoftmetaroce/MetaRoceVerbs.h>
#include <libsoftmetaroce/ProtocolCompatibility.h>
#include <libsoftmetaroce/test/rdma-unit/introspection_lsmr.h>

ABSL_FLAG(
    int32_t,
    lsmr_udp_port,
    0,
    "UDP port the backend binds and sends to; 0 derives one from the pid");

namespace {

// The suite is its own peer, so it sends to the port it binds. 20000 keeps
// these below the ephemeral ports the kernel assigns.
int32_t portForThisProcess() {
  return 20000 + static_cast<int32_t>(::getpid() % 10000);
}

} // namespace

int main(int argc, char* argv[]) {
  testing::InitGoogleTest(&argc, argv);
  // Parse flags before initializing logging so absl log flags passed on the
  // command line take effect at initialization.
  absl::ParseCommandLine(argc, argv);
  absl::InitializeLog();
  absl::FailureSignalHandlerOptions options;
  absl::InstallFailureSignalHandler(options);

  // MetaRoceVerbs uses folly Singletons. The suite initializes absl, not folly,
  // so mark folly singleton registration complete here; a full folly::init
  // would reparse argv through gflags and reject the suite's absl flags.
  folly::SingletonVault::singleton()->registrationComplete();

  // Set before the first verb reaches MetaRoceVerbs, which reads both at
  // construction and hands them to the sender and the ping controller.
  const int32_t configuredPort = absl::GetFlag(FLAGS_lsmr_udp_port);
  CHECK(configuredPort >= 0 && configuredPort <= 65535)
      << "--lsmr_udp_port=" << configuredPort
      << " is outside the 16-bit port range, so narrowing it would bind some "
         "other port";
  FLAGS_libsoftmetaroce_bind_to_port =
      configuredPort != 0 ? configuredPort : portForThisProcess();
  FLAGS_libsoftmetaroce_send_to_port = FLAGS_libsoftmetaroce_bind_to_port;

  // lsmr's own MetaRoCE introspection, keyed on the "lsmr" device prefix.
  rdma_unit_test::IntrospectionLsmr::Register();

  FLAGS_libsoftmetaroce_emit_send_last = true;
  const int rc = RUN_ALL_TESTS();
  // The emulation's folly thread pools deadlock in the folly singleton atexit
  // teardown, hanging the process after the verdict is already decided. Skip
  // static destructors and exit with the test result.
  std::fflush(nullptr);
  _exit(rc);
}
