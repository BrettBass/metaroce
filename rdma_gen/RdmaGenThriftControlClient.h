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

#include <map>
#include <memory>
#include <unordered_map>

#include <folly/coro/Task.h>
#include <folly/io/async/ScopedEventBaseThread.h>

#include "rdma_gen/Common.h"
#include "rdma_gen/RdmaGenEndpoint.h"
#include "rdma_gen/if/gen-cpp2/RdmaGenControlService.h"

namespace facebook::neteng::ai::rdma_gen {

// Thrift-based control client for RDMA Gen operations
class RdmaGenThriftControlClient {
 public:
  RdmaGenThriftControlClient(
      std::shared_ptr<RdmaGenEndpoint> endpoint,
      std::shared_ptr<std::vector<RcQpInfo>> remoteRcQpInfo,
      std::shared_ptr<std::vector<void*>> remoteAddrs =
          std::make_shared<std::vector<void*>>(),
      std::shared_ptr<std::vector<uint32_t>> rkeys =
          std::make_shared<std::vector<uint32_t>>());

  ~RdmaGenThriftControlClient() = default;

  // Main signal method matching the original interface
  folly::coro::Task<bool> signalServer(
      const std::string& servername,
      int port,
      uint32_t qpIdx,
      ControlMessage controlMsg);

  folly::coro::Task<bool> exchangeMetadata(
      const std::string& servername,
      int port,
      const std::map<std::string, std::string>& localMetadata,
      std::map<std::string, std::string>& remoteMetadata);

  // Blocks on the server's completionBarrier RPC until the server confirms it
  // has drained all expected RX completions (or the server times out). Returns
  // the barrier status reported by the server.
  folly::coro::Task<bool> completionBarrier(
      const std::string& servername,
      int port,
      int qpIdx,
      uint64_t clientCompleted);

  // Get reference to the EventBase for external use
  folly::EventBase& getEventBase();

 private:
  // Individual thrift operations
  folly::coro::Task<bool>
  exchangeQpInfo(const std::string& servername, int port, uint32_t qpIdx);

  folly::coro::Task<bool>
  terminate(const std::string& servername, int port, uint32_t qpIdx);

  folly::coro::Task<bool>
  write(const std::string& servername, int port, uint32_t qpIdx);

  folly::coro::Task<bool>
  read(const std::string& servername, int port, uint32_t qpIdx);

  // Helper methods for client creation and conversion
  folly::coro::Task<apache::thrift::Client<thrift::RdmaGenControlService>*>
  getClient(const std::string& servername, int port);

  folly::coro::Task<
      std::unique_ptr<apache::thrift::Client<thrift::RdmaGenControlService>>>
  createClient(const std::string& servername, int port);

  std::shared_ptr<RdmaGenEndpoint> endpoint_;
  std::shared_ptr<std::vector<RcQpInfo>> remoteRcQpInfo_;
  std::shared_ptr<std::vector<void*>> remoteAddrs_;
  std::shared_ptr<std::vector<uint32_t>> rkeys_;

  mutable std::unordered_map<
      std::string,
      std::unique_ptr<apache::thrift::Client<thrift::RdmaGenControlService>>>
      clientCache_;

  // Declared after clientCache_ so it is destroyed first, stopping the
  // EventBase thread before cached clients (which hold AsyncSockets
  // registered on this EventBase) are destroyed.
  folly::ScopedEventBaseThread evbThread_;
};

} // namespace facebook::neteng::ai::rdma_gen
