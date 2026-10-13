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

#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include <folly/Synchronized.h>
#include <folly/coro/SharedMutex.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>

#include "rdma_gen/Common.h"
#include "rdma_gen/RdmaGenEndpoint.h"
#include "rdma_gen/if/gen-cpp2/RdmaGenControlService.h"

namespace facebook::neteng::ai::rdma_gen {

// Forward declarations
class RdmaGenThriftControlServer;

// Callback invoked by the completion barrier handler to wait until the server
// has drained all expected RX completions. Takes a timeout and returns whether
// RX accounting completed within that timeout. Defaults to an always-complete
// implementation when no receiver is wired in.
using RxCompleteWaiter = std::function<bool(std::chrono::milliseconds)>;

// Thrift service handler for RDMA Gen control operations
class RdmaGenThriftControlServiceHandler
    : public apache::thrift::ServiceHandler<thrift::RdmaGenControlService> {
 public:
  RdmaGenThriftControlServiceHandler(
      std::shared_ptr<RdmaGenEndpoint> endpoint,
      std::shared_ptr<std::vector<RcQpInfo>> remoteRcQpInfo,
      std::shared_ptr<ContinueFlag> continueFlag,
      int terminateThreshold,
      RdmaGenThriftControlServer* server = nullptr);

  folly::coro::Task<void> co_onStartServing() override;

  folly::coro::Task<thrift::ExchangeQpInfoResponse> co_exchangeQpInfo(
      const thrift::ExchangeQpInfoRequest& request) override;

  folly::coro::Task<thrift::TerminationResponse> co_terminate(
      const thrift::TerminationRequest& request) override;

  folly::coro::Task<thrift::CompletionBarrierResponse> co_completionBarrier(
      const thrift::CompletionBarrierRequest& request) override;

  folly::coro::Task<thrift::WriteResponse> co_write(
      const thrift::WriteRequest& request) override;

  folly::coro::Task<thrift::ReadResponse> co_read(
      const thrift::ReadRequest& request) override;

  folly::coro::Task<thrift::MetadataExchangeResponse> co_exchangeMetadata(
      const thrift::MetadataExchangeRequest& request) override;

  // Helper methods
  bool continueFlag() const;
  void setContinueFlag(bool flag);
  int getCurrentTerminateCount() const;
  void setMetadata(const std::map<std::string, std::string>& metadata);
  void setMetadataCallback(MetadataCallback callback);
  void setRxCompleteWaiter(RxCompleteWaiter waiter);
  void setBarrierTimeout(std::chrono::milliseconds timeout);

 private:
  std::shared_ptr<RdmaGenEndpoint> endpoint_;
  std::shared_ptr<std::vector<RcQpInfo>> remoteRcQpInfo_;
  std::shared_ptr<ContinueFlag> continueFlag_;

  // Pointer to the server (for notifying when ready)
  RdmaGenThriftControlServer* server_;

  // Thread safety for termination count
  mutable folly::coro::SharedMutex terminateMutex_;
  int currentTerminateCount_ = 0;
  const int terminateThreshold_;
  folly::Synchronized<std::map<std::string, std::string>> localMetadata_;
  // Metadata callback to allow the server to process incoming metadata from
  // clients
  MetadataCallback metadataCallback_;
  // Waiter invoked by co_completionBarrier to block until all expected RX
  // completions have been drained. Defaults to always-complete.
  RxCompleteWaiter rxCompleteWaiter_ = [](std::chrono::milliseconds) {
    return true;
  };
  // Upper bound on how long the barrier handler waits for RX completions.
  std::chrono::milliseconds barrierTimeout_{std::chrono::seconds(30)};
};

// Wrapper class for the thrift server to match the factory interface
class RdmaGenThriftControlServer {
 public:
  RdmaGenThriftControlServer(
      uint16_t port,
      std::shared_ptr<RdmaGenEndpoint> endpoint,
      std::shared_ptr<std::vector<RcQpInfo>> remoteRcQpInfo,
      std::shared_ptr<ContinueFlag> continueFlag,
      int terminateThreshold);

  ~RdmaGenThriftControlServer();

  // Disable copy and move operations
  RdmaGenThriftControlServer(const RdmaGenThriftControlServer&) = delete;
  RdmaGenThriftControlServer& operator=(const RdmaGenThriftControlServer&) =
      delete;
  RdmaGenThriftControlServer(RdmaGenThriftControlServer&&) = delete;
  RdmaGenThriftControlServer& operator=(RdmaGenThriftControlServer&&) = delete;

  // Interface methods
  void start();
  void stop();
  uint16_t getBindPort() const;
  int getCurrentTerminateCount() const;
  bool continueFlag() const;
  void setContinueFlag(bool flag);
  void setMetadata(const std::map<std::string, std::string>& metadata);
  void setMetadataCallback(MetadataCallback callback);
  void setRxCompleteWaiter(RxCompleteWaiter waiter);
  void setBarrierTimeout(std::chrono::milliseconds timeout);

  // Called by the handler when server is ready to serve
  void notifyServerReady();

 private:
  void launchServerInThread();

  uint16_t bindPort_ = 0;
  std::shared_ptr<RdmaGenThriftControlServiceHandler> handler_;
  std::unique_ptr<apache::thrift::ThriftServer> server_;
  std::thread serverThread_;
  std::atomic<bool> serverStarted_{false};
  std::atomic<bool> serverReady_{false};
  std::mutex readyMutex_;
  std::condition_variable readyCondition_;
};

} // namespace facebook::neteng::ai::rdma_gen
