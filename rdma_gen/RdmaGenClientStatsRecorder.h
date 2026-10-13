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

#include <optional>

#include <folly/json/dynamic.h>
#include "rdma_gen/CqeLogEntry.h"
#include "rdma_gen/RdmaGenStatsRecorder.h"

namespace facebook::neteng::ai::rdma_gen {

class RdmaGenClientStatsRecorder : public RdmaGenStatsRecorder {
 public:
  RdmaGenClientStatsRecorder(
      uint32_t numQps,
      uint32_t txDepth,
      const std::vector<int64_t>& numMessagesPerServer,
      bool logMessageLatencies,
      const std::string& jsonPath,
      uint8_t sqpPerQp = 1)
      : RdmaGenStatsRecorder(jsonPath),
        logMessageLatencies_(logMessageLatencies) {
    msgCompletionCnt_.resize(numQps, 0);
    msgTotalCnt_.resize(numQps, 0);
    if (logMessageLatencies_) {
      const uint32_t totalBuffers = numQps * sqpPerQp;
      msgStartTimesUs_.resize(
          (std::vector<uint64_t>::size_type)totalBuffers * txDepth, 0);
      msgTimeTakenUs_.resize(numQps);
      msgTimeStampsUs_.resize(numQps);
      for (auto i = 0; i < numQps; i++) {
        msgTimeTakenUs_[i].reserve(numMessagesPerServer[i]);
        msgTimeStampsUs_[i].reserve(numMessagesPerServer[i]);
      }
    }
  }

  ~RdmaGenClientStatsRecorder() override = default;

  using RdmaGenStatsRecorder::finalizeJsonReport;

  void setupJson() override;
  void addJsonEntry(const std::vector<std::string>& rcReceivers) override;

  void finalizeJsonReport(
      double gbps,
      double mpps,
      const std::vector<std::string>& rcReceivers,
      OperationType opType,
      VerbsMode::Value verbsMode,
      const std::vector<CqeLogEntry>& errCqeLog = {},
      const std::vector<CqeLogEntry>& allCqeLog = {},
      std::optional<double> cpLatencyUsecs = std::nullopt);

  // Print message completion count for each QP
  void printStats();

  // Reset message completion count for each QP
  void resetStats();

  // record time taken for operation to complete given buffer index and qp index
  void
  recordTimeTaken(uint32_t bufferIdx, uint32_t qpIdx, uint64_t currentTime);

  // record operation start time given buffer token
  void recordStartTime(uint32_t bufferToken, uint64_t currentTime);

  // Create a new client stats entry
  void createNewClientStatsEntry();

  // Get current JSON object where new data will be added
  folly::dynamic& getCurrentJsonObject();

  folly::dynamic generateClientResult(
      double gbps,
      double mpps,
      std::optional<double> cpLatencyUsecs);

  folly::dynamic generateServerResult(
      const std::vector<std::string>& rcReceivers,
      OperationType opType,
      VerbsMode::Value verbsMode);

  // get message completion count given qp index
  uint64_t getMsgTotalCnt(uint32_t qpIdx) const {
    return msgTotalCnt_[qpIdx];
  }

  // get message completion count given qp index
  uint64_t getMsgCompletionCnt(uint32_t qpIdx) const {
    return msgCompletionCnt_[qpIdx];
  }

  uint64_t getMsgCompletionCntSize() const {
    return msgCompletionCnt_.size();
  }

  uint64_t getMsgStartTimesUsSize() const {
    return msgStartTimesUs_.size();
  }

  uint64_t getMsgTimeTakenUsSize() const {
    return msgTimeTakenUs_.size();
  }

  uint64_t getMsgTimeStampsUsSize() const {
    return msgTimeStampsUs_.size();
  }

  uint64_t getMsgTimeTakenUsForQpSize(uint32_t qpIdx) const {
    return msgTimeTakenUs_.at(qpIdx).size();
  }

  uint64_t getMsgTimeStampsUsForQpSize(uint32_t qpIdx) const {
    return msgTimeStampsUs_.at(qpIdx).size();
  }

  // Get json content from root object, used for testing
  std::string getJsonContent() const;

 private:
  const bool logMessageLatencies_;
  std::vector<uint64_t> msgTotalCnt_;
  std::vector<uint64_t> msgCompletionCnt_;

  // vector to keep track of sent messages start times corresponding
  // to buffer entry
  std::vector<uint64_t> msgStartTimesUs_;
  // vector to keep track of send messages end to end time taken per qp
  std::vector<std::vector<uint64_t>> msgTimeTakenUs_;
  // vector to keep track of send messages start time and end time per qp
  std::vector<std::vector<std::pair<uint64_t, uint64_t>>> msgTimeStampsUs_;
  // Root object to keep track of json entries
  folly::dynamic jsonRoot_;
};

} // namespace facebook::neteng::ai::rdma_gen
