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

#include <chrono>
#include <thread>

#include <folly/init/Init.h>

#include <libsoftmetaroce/MetaRoceVerbs.h>

DEFINE_string(remote_addr, "fc00::1", "Remote address to ping (IPv4 or IPv6).");
DEFINE_int32(source_qp_num, 2, "Source QP number for the ping.");
DEFINE_int32(remote_qp_num, 2, "Remote QP number to ping.");
DEFINE_int32(tag_num, 0, "Tag number within the QP (0-255).");
DEFINE_int32(flow_id, 0, "Flow ID to use for the ping.");
DEFINE_int32(
    supported_flow_count,
    8,
    "Number of TX flows supported by the sender.");
DEFINE_int32(sgid_index, 0, "Source GID index (local address index).");
DEFINE_int32(timeout_ms, 1000, "Ping timeout in milliseconds.");
DEFINE_int32(count, 1, "Number of pings to send.");
DEFINE_int32(interval_ms, 1000, "Interval between pings in milliseconds.");

static bool validateTagNum(const char*, int32_t value) {
  return value >= 0 && value <= 255;
}
DEFINE_validator(tag_num, &validateTagNum);

static bool validateCount(const char*, int32_t value) {
  return value >= 1;
}
DEFINE_validator(count, &validateCount);

namespace libsoftmetaroce {

int runPing() {
  MetaRoceVerbs verbs;

  folly::IPAddress remoteAddr;
  try {
    remoteAddr = folly::IPAddress(FLAGS_remote_addr);
  } catch (const std::exception& e) {
    LOG(ERROR) << "Invalid remote address: " << FLAGS_remote_addr << " - "
               << e.what();
    return 1;
  }

  LOG(INFO) << "Starting ping to " << remoteAddr.str() << " (QP "
            << FLAGS_remote_qp_num << ", Tag " << FLAGS_tag_num << ", Flow "
            << FLAGS_flow_id << ", SupportedFlowCount "
            << FLAGS_supported_flow_count << ")";

  int completedCount = 0;
  int timeoutCount = 0;

  for (int i = 0; i < FLAGS_count; ++i) {
    if (i > 0 && FLAGS_interval_ms > 0) {
      // @lint-ignore CLANGTIDY facebook-hte-BadCall-sleep_for
      std::this_thread::sleep_for(std::chrono::milliseconds(FLAGS_interval_ms));
    }

    VLOG(1) << "Ping " << (i + 1) << "/" << FLAGS_count << "...";

    int result = verbs.sendPing(
        static_cast<uint8_t>(FLAGS_sgid_index),
        remoteAddr,
        static_cast<uint32_t>(FLAGS_source_qp_num),
        static_cast<uint32_t>(FLAGS_remote_qp_num),
        static_cast<uint8_t>(FLAGS_tag_num),
        static_cast<uint16_t>(FLAGS_flow_id),
        static_cast<uint16_t>(FLAGS_supported_flow_count),
        FLAGS_timeout_ms);

    if (result >= 0) {
      VLOG(1) << "Ping " << (i + 1) << " completed (Error State: " << result
              << ")";
      ++completedCount;
    } else {
      LOG(WARNING) << "Ping " << (i + 1) << " did not complete";
      ++timeoutCount;
    }
  }

  LOG(INFO) << "Ping summary: " << completedCount << "/" << FLAGS_count
            << " completed, " << timeoutCount << "/" << FLAGS_count
            << " timed out";

  return timeoutCount > 0 ? 1 : 0;
}

} // namespace libsoftmetaroce

int main(int argc, char* argv[]) {
  folly::Init init(&argc, &argv);
  return libsoftmetaroce::runPing();
}
