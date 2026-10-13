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

#include <memory>

#include <folly/coro/BlockingWait.h>
#include <folly/io/async/EventBase.h>
#include <glog/logging.h>

#include <map>

#include "rdma_gen/Common.h"
#include "rdma_gen/RdmaGenEndpoint.h"
#include "rdma_gen/RdmaGenThriftControlClient.h"
#include "rdma_gen/RdmaGenThriftControlServer.h"

namespace facebook::neteng::ai::rdma_gen {

// Base interface for control servers to enable polymorphism
class IControlServer {
 public:
  virtual ~IControlServer() = default;
  virtual void start() = 0;
  virtual void stop() = 0;
  virtual uint16_t getBindPort() const = 0;
  virtual int getCurrentTerminateCount() const = 0;
  virtual bool continueFlag() const = 0;
  virtual void setContinueFlag(bool flag) = 0;
  virtual void setMetadata(
      const std::map<std::string, std::string>& metadata) = 0;
  virtual void setMetadataCallback(MetadataCallback callback) = 0;
  virtual void setRxCompleteWaiter(RxCompleteWaiter waiter) = 0;
  virtual void setBarrierTimeout(std::chrono::milliseconds timeout) = 0;
};

// Wrapper for thrift-based control server
class ThriftControlServerWrapper : public IControlServer {
 public:
  explicit ThriftControlServerWrapper(
      std::shared_ptr<RdmaGenThriftControlServer> server)
      : server_(std::move(server)) {}

  void start() override {
    server_->start();
  }
  void stop() override {
    server_->stop();
  }
  uint16_t getBindPort() const override {
    return server_->getBindPort();
  }
  int getCurrentTerminateCount() const override {
    return server_->getCurrentTerminateCount();
  }
  bool continueFlag() const override {
    return server_->continueFlag();
  }
  void setContinueFlag(bool flag) override {
    server_->setContinueFlag(flag);
  }
  void setMetadata(
      const std::map<std::string, std::string>& metadata) override {
    server_->setMetadata(metadata);
  }
  void setMetadataCallback(MetadataCallback callback) override {
    server_->setMetadataCallback(std::move(callback));
  }
  void setRxCompleteWaiter(RxCompleteWaiter waiter) override {
    server_->setRxCompleteWaiter(std::move(waiter));
  }
  void setBarrierTimeout(std::chrono::milliseconds timeout) override {
    server_->setBarrierTimeout(timeout);
  }

 private:
  std::shared_ptr<RdmaGenThriftControlServer> server_;
};

// Base interface for control clients to enable polymorphism
class IControlClient {
 public:
  virtual ~IControlClient() = default;
  virtual bool signalServer(
      const std::string& servername,
      int port,
      uint32_t qpIdx,
      ControlMessage controlMsg) = 0;
  virtual bool exchangeMetadata(
      const std::string& servername,
      int port,
      const std::map<std::string, std::string>& localMetadata,
      std::map<std::string, std::string>& remoteMetadata) = 0;
  virtual bool completionBarrier(
      const std::string& servername,
      int port,
      int qpIdx,
      uint64_t completed) = 0;
};

// Wrapper for thrift-based control client
class ThriftControlClientWrapper : public IControlClient {
 public:
  explicit ThriftControlClientWrapper(
      std::unique_ptr<RdmaGenThriftControlClient> client)
      : client_(std::move(client)) {}

  bool signalServer(
      const std::string& servername,
      int port,
      uint32_t qpIdx,
      ControlMessage controlMsg) override;
  bool exchangeMetadata(
      const std::string& servername,
      int port,
      const std::map<std::string, std::string>& localMetadata,
      std::map<std::string, std::string>& remoteMetadata) override;
  bool completionBarrier(
      const std::string& servername,
      int port,
      int qpIdx,
      uint64_t completed) override;

 private:
  std::unique_ptr<RdmaGenThriftControlClient> client_;
};

// Available control transport implementations.
// When a new implementation is added, extend this enum and handle it in the
// factory switch statements and test INSTANTIATE_TEST_SUITE_P.
enum class ControlTransport { Thrift };

// Factory class to create control servers and clients
class RdmaGenControlFactory {
 public:
  static std::unique_ptr<IControlServer> createControlServer(
      uint16_t port,
      std::shared_ptr<RdmaGenEndpoint> endpoint,
      std::shared_ptr<std::vector<RcQpInfo>> remoteRcQpInfo,
      std::shared_ptr<ContinueFlag> continueFlag,
      int terminateThreshold,
      ControlTransport transport = ControlTransport::Thrift);

  static std::unique_ptr<IControlClient> createControlClient(
      std::shared_ptr<RdmaGenEndpoint> endpoint,
      std::shared_ptr<std::vector<RcQpInfo>> remoteRcQpInfo,
      std::shared_ptr<std::vector<void*>> remoteAddrs =
          std::make_shared<std::vector<void*>>(),
      std::shared_ptr<std::vector<uint32_t>> rkeys =
          std::make_shared<std::vector<uint32_t>>(),
      ControlTransport transport = ControlTransport::Thrift);
};

} // namespace facebook::neteng::ai::rdma_gen
