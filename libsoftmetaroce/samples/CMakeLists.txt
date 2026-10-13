# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Sample tools for the standalone libsoftmetaroce build. Mirrors samples/BUCK:
# the common support library plus the send, receive, ping, and
# metaroce_config_tutorial binaries. Added by the parent only when samples
# are requested and this tree is present. Source names stay explicit (no
# loops) so test/CMakeListCheck.sh can grep them for BUCK parity.

# Support library for send and receive (BUCK :common, including :tag_metadata).
add_library(lsmr_samples_common STATIC common.cpp common.h tag_metadata.h)
# send.cpp and receive.cpp include common.h, whose signatures name folly
# types, so the verbs library (and its folly includes) propagate.
target_link_libraries(lsmr_samples_common PUBLIC libsoftmetaroce::softmetaroce)

# Target names are global; prefix so add_subdirectory() cannot collide
# with sibling targets.
add_executable(lsmr_send send.cpp)
target_link_libraries(lsmr_send PRIVATE lsmr_samples_common)

add_executable(lsmr_receive receive.cpp)
target_link_libraries(lsmr_receive PRIVATE lsmr_samples_common)

add_executable(lsmr_ping ping.cpp)
target_link_libraries(lsmr_ping PRIVATE libsoftmetaroce::softmetaroce)

# The tutorial uses only the rdma-core verbs API, like its BUCK target.
add_executable(lsmr_metaroce_config_tutorial metaroce_config_tutorial.cpp)
target_include_directories(
  lsmr_metaroce_config_tutorial PRIVATE ${IBVERBS_INCLUDE_DIRS})
target_link_libraries(
  lsmr_metaroce_config_tutorial PRIVATE ${IBVERBS_LIBRARIES})
