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

#include "rdma_gen/RdmaGenClientStatsRecorder.h"
#include <folly/json/dynamic.h>
#include <folly/json/json.h>
#include <folly/stats/Histogram.h>
#include <cstdint>
#include <fstream>

#include "rdma_gen/transport/RdmaGenVerbsMode.h"

namespace facebook::neteng::ai::rdma_gen {

void RdmaGenClientStatsRecorder::printStats() {
  std::string output = "Messages: ";
  for (const auto c : msgCompletionCnt_) {
    output += folly::to<std::string>(c) + ",";
  }
  VLOG(2) << output;
}

void RdmaGenClientStatsRecorder::setupJson() {
  std::string existingJson = readExistingJson();
  // Initialize jsonRoot_ for the first run
  auto initializeJsonRoot = [&]() {
    jsonRoot_ = folly::dynamic::object;
    jsonRoot_["client_stats"] = folly::dynamic::array;
  };
  if (existingJson.empty()) {
    initializeJsonRoot();
    return;
  }

  // Load and process exisiting JSON data for second time run onwards
  try {
    jsonRoot_ = folly::parseJson(existingJson);

    if (jsonRoot_.isObject()) {
      folly::dynamic tempArray = folly::dynamic::array;
      tempArray.push_back(jsonRoot_);
      jsonRoot_ = std::move(tempArray);
    } else if (!jsonRoot_.isArray()) {
      LOG(ERROR) << "Invalid JSON format";
      initializeJsonRoot();
      return;
    }
  } catch (const folly::TypeError& e) {
    LOG(ERROR) << "TypeError parsing JSON: " << e.what();
    // Overwrite jsonRoot_ in case of TypeError
    initializeJsonRoot();
    return;
  } catch (const std::exception& e) {
    LOG(ERROR) << "Error parsing JSON: " << e.what();
    // Overwrite jsonRoot_ in case of any other exception
    initializeJsonRoot();
    return;
  }
  createNewClientStatsEntry();
}

void RdmaGenClientStatsRecorder::finalizeJsonReport(
    double gbps,
    double mpps,
    const std::vector<std::string>& rcReceivers,
    OperationType opType,
    VerbsMode::Value verbsMode,
    const std::vector<CqeLogEntry>& errCqeLog,
    const std::vector<CqeLogEntry>& allCqeLog,
    std::optional<double> cpLatencyUsecs) {
  if (jsonPath_.empty()) {
    LOG(INFO) << "No client-side json path provided, skipping json report";
    return;
  }
  folly::dynamic clientResults =
      generateClientResult(gbps, mpps, cpLatencyUsecs);
  if (logMessageLatencies_) {
    clientResults["server_results"] =
        generateServerResult(rcReceivers, opType, verbsMode);
  }

  // Add error CQEs to the results if any exist
  if (!errCqeLog.empty()) {
    clientResults["error_cqes"] = cqeLogToDynamic(errCqeLog, opType);
  }

  folly::dynamic& currentJsonObject = getCurrentJsonObject();
  currentJsonObject["results"] = clientResults;

  if (!allCqeLog.empty()) {
    currentJsonObject["cqes"] = cqeLogToDynamic(allCqeLog, opType);
  }

  std::ofstream outfile(jsonPath_, std::ios::trunc);
  outfile << folly::toPrettyJson(jsonRoot_) << "\n";
}

void RdmaGenClientStatsRecorder::addJsonEntry(
    const std::vector<std::string>& rcReceivers) {
  folly::dynamic clientStatsEntry = folly::dynamic::object;
  for (size_t i = 0; i < msgCompletionCnt_.size(); i++) {
    clientStatsEntry[rcReceivers[i]] = msgCompletionCnt_[i];
  }

  folly::dynamic& currentJsonObject = getCurrentJsonObject();

  if (!currentJsonObject.count("client_stats")) {
    currentJsonObject["client_stats"] = folly::dynamic::array;
  }

  currentJsonObject["client_stats"].push_back(clientStatsEntry);
}

void RdmaGenClientStatsRecorder::resetStats() {
  for (int i = 0; i < msgCompletionCnt_.size(); i++) {
    msgCompletionCnt_[i] = 0;
  }
}

void RdmaGenClientStatsRecorder::recordTimeTaken(
    uint32_t bufferIdx,
    uint32_t qpIdx,
    uint64_t currentTime) {
  if (logMessageLatencies_) {
    auto startTime = msgStartTimesUs_[bufferIdx];
    auto timeTaken = currentTime - startTime;
    msgTimeTakenUs_[qpIdx].emplace_back(timeTaken);
    msgTimeStampsUs_[qpIdx].emplace_back(startTime, currentTime);
  }
  msgCompletionCnt_[qpIdx]++;
  msgTotalCnt_[qpIdx]++;
}

void RdmaGenClientStatsRecorder::recordStartTime(
    uint32_t bufferToken,
    uint64_t currentTime) {
  if (logMessageLatencies_) {
    msgStartTimesUs_[bufferToken] = currentTime;
  }
}

void RdmaGenClientStatsRecorder::createNewClientStatsEntry() {
  folly::dynamic newEntry = folly::dynamic::object;
  newEntry["client_stats"] = folly::dynamic::array;
  jsonRoot_.push_back(newEntry);
}

folly::dynamic& RdmaGenClientStatsRecorder::getCurrentJsonObject() {
  // If this is the first time run, jsonRoot_ is a JSON object `{}`
  if (jsonRoot_.isObject()) {
    return jsonRoot_;
  }
  // If this is not first time run, jsonRoot_ is an array `[{}, {}...]`
  if (jsonRoot_.isArray() && !jsonRoot_.empty()) {
    return jsonRoot_[jsonRoot_.size() - 1];
  }

  LOG(ERROR) << "Invalid JSON format";
  jsonRoot_ = folly::dynamic::object;
  return jsonRoot_;
}

std::string RdmaGenClientStatsRecorder::getJsonContent() const {
  return folly::toPrettyJson(jsonRoot_);
}

folly::dynamic RdmaGenClientStatsRecorder::generateClientResult(
    double gbps,
    double mpps,
    std::optional<double> cpLatencyUsecs) {
  folly::dynamic clientResults = folly::dynamic::object;
  clientResults["Mpps"] = mpps;
  clientResults["Gbps"] = gbps;
  if (cpLatencyUsecs.has_value()) {
    clientResults["cp_latency_usecs"] = cpLatencyUsecs.value();
  }
  clientResults["total_messages"] = folly::dynamic::array;
  for (size_t i = 0; i < msgTotalCnt_.size(); i++) {
    clientResults["total_messages"].push_back(
        static_cast<int64_t>(msgTotalCnt_[i]));
  }
  return clientResults;
}

folly::dynamic RdmaGenClientStatsRecorder::generateServerResult(
    const std::vector<std::string>& rcReceivers,
    OperationType opType,
    VerbsMode::Value verbsMode) {
  folly::dynamic serverResults = folly::dynamic::array;
  for (auto i = 0; i < msgTimeTakenUs_.size(); i++) {
    folly::dynamic messages = folly::dynamic::array;
    folly::Histogram<uint64_t> histogram(
        (uint64_t)10, (uint64_t)0, (uint64_t)100000);
    std::string str;
    for (int j = 0; j < msgTimeTakenUs_[i].size(); j++) {
      histogram.addValue(msgTimeTakenUs_[i][j]);
      str.append(std::to_string(msgTimeTakenUs_[i][j]));
      if (j != msgTimeTakenUs_[i].size() - 1) {
        str.append(",");
      }

      if (verbsMode == VerbsMode::LibSoftMetaRoCE) {
        folly::dynamic message = folly::dynamic::object;
        message["qp_idx"] = i;
        message["start_time_us"] = msgTimeStampsUs_[i][j].first;
        message["end_time_us"] = msgTimeStampsUs_[i][j].second;
        message["op_type"] = folly::dynamic(operationTypeToString(opType));
        messages.push_back(message);
      }
    }

    folly::dynamic serverEntry = folly::dynamic::object;
    if (verbsMode == VerbsMode::LibSoftMetaRoCE) {
      serverEntry["messages"] = messages;
    }

    serverEntry["per_msg_latencies_p10"] =
        histogram.getPercentileEstimate(0.10);
    serverEntry["per_msg_latencies_p50"] =
        histogram.getPercentileEstimate(0.50);
    serverEntry["per_msg_latencies_p90"] =
        histogram.getPercentileEstimate(0.90);
    serverEntry["per_msg_latencies_p99"] =
        histogram.getPercentileEstimate(0.99);
    serverEntry["msg_latencies"] = str;
    serverEntry["server"] = rcReceivers[i];
    serverResults.push_back(serverEntry);
  }
  return serverResults;
}

} // namespace facebook::neteng::ai::rdma_gen
