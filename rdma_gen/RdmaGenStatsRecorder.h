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

#include <string>
#include <vector>
#include "rdma_gen/RdmaGenUtils.h"
#include "rdma_gen/transport/RdmaGenVerbsMode.h"

namespace facebook::neteng::ai::rdma_gen {

// This class handles all stats recording related stuff.
class RdmaGenStatsRecorder {
 public:
  explicit RdmaGenStatsRecorder(const std::string& jsonPath)
      : jsonPath_(jsonPath) {}
  virtual ~RdmaGenStatsRecorder() = default;
  virtual void setupJson() {}
  // Add an entry to json report
  virtual void addJsonEntry(const std::vector<std::string>& /*rcReceivers*/) {}
  virtual void finalizeJsonReport(
      double /* gbps */,
      double /* mpps */,
      const std::vector<std::string>& /* rcReceivers */,
      OperationType /* opType */,
      VerbsMode::Value /* verbsMode */) {}
  // Read existing json file contents
  inline std::string readExistingJson() {
    std::ifstream infile(jsonPath_);
    if (!infile) {
      return "";
    }

    infile.clear();
    infile.seekg(0, std::ios::beg);
    std::ostringstream buffer;
    buffer << infile.rdbuf();
    infile.close();

    return buffer.str();
  }

 protected:
  std::string jsonPath_;
};

} // namespace facebook::neteng::ai::rdma_gen
