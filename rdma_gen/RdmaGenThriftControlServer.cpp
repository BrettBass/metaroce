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

#include "rdma_gen/RdmaGenThriftControlServer.h"

#include <folly/coro/BlockingWait.h>
#include <folly/coro/Sleep.h>
#include <glog/logging.h>

#include <stdexcept>

#include "rdma_gen/RdmaGenThriftControlCommon.h"

namespace facebook::neteng::ai::rdma_gen {

namespace {
// A healthy ThriftServer binds and notifies within milliseconds of serve();
// this only has to outlast scheduling noise on a loaded emulation host.
constexpr std::chrono::seconds kServerReadyTimeout{60};
} // namespace

// RdmaGenThriftControlServiceHandler implementation

RdmaGenThriftControlServiceHandler::RdmaGenThriftControlServiceHandler(
    std::shared_ptr<RdmaGenEndpoint> endpoint,
    std::shared_ptr<std::vector<RcQpInfo>> remoteRcQpInfo,
    std::shared_ptr<ContinueFlag> continueFlag,
    int terminateThreshold,
    RdmaGenThriftControlServer* server)
    : endpoint_(std::move(endpoint)),
      remoteRcQpInfo_(std::move(remoteRcQpInfo)),
      continueFlag_(std::move(continueFlag)),
      server_(server),
      terminateThreshold_(terminateThreshold) {}

folly::coro::Task<void>
RdmaGenThriftControlServiceHandler::co_onStartServing() {
  VLOG(1) << "Service handler co_onStartServing called - server is ready";
  if (server_) {
    server_->notifyServerReady();
  }
  co_return;
}

folly::coro::Task<thrift::ExchangeQpInfoResponse>
RdmaGenThriftControlServiceHandler::co_exchangeQpInfo(
    const thrift::ExchangeQpInfoRequest& request) {
  LOG(INFO) << "Received exchangeQpInfo request";

  thrift::ExchangeQpInfoResponse response;

  try {
    // Convert thrift QpInfo to native format
    RcQpInfo requestQpInfo =
        RdmaGenThriftConversions::convertFromThriftQpInfo(*request.qpInfo());

    if (endpoint_->params().role != EndpointRole::Receiver) {
      response.status() = false;
    } else {
      remoteRcQpInfo_->push_back(requestQpInfo);
      auto qpIdx = static_cast<int>(remoteRcQpInfo_->size() - 1);
      const auto sqp_per_qp = endpoint_->params().sqp_per_qp;
      response.status() = endpoint_->modifyRcQpsRts(
          remoteRcQpInfo_->at(qpIdx),
          qpIdx * sqp_per_qp,
          0, // qp_cos
          endpoint_->params().mtu,
          IBV_QPS_RTS);
      response.qpInfo() = RdmaGenThriftConversions::convertToThriftQpInfo(
          endpoint_->getLocalRcQpInfo(qpIdx));
    }

    LOG(INFO) << "Successfully processed exchangeQpInfo request";
  } catch (const std::exception& ex) {
    LOG(ERROR) << "Error processing exchangeQpInfo request: " << ex.what();
    response.status() = false;
    response.qpInfo() = thrift::RcQpInfo{};
  }

  co_return response;
}

folly::coro::Task<thrift::TerminationResponse>
RdmaGenThriftControlServiceHandler::co_terminate(
    const thrift::TerminationRequest& request) {
  LOG(INFO) << "Received termination request for QP " << *request.qpn();

  thrift::TerminationResponse response;

  try {
    // Check if the QP is known locally
    bool foundLocally = false;
    for (const auto& qpInfo : *remoteRcQpInfo_) {
      if (qpInfo.qpn == *request.qpn()) {
        foundLocally = true;
        break;
      }
    }

    if (!foundLocally) {
      LOG(WARNING) << "received termination request from unknown client";
      response.status() = false;
    } else {
      response.status() = true;
      // Thread-safe increment of termination count
      auto guard = co_await terminateMutex_.co_scoped_lock();
      currentTerminateCount_++;

      LOG(INFO) << "Current terminate count: " << currentTerminateCount_
                << ", threshold: " << terminateThreshold_;

      // Set continue flag to false if we've reached the threshold
      if (currentTerminateCount_ >= terminateThreshold_ &&
          terminateThreshold_ > 0) {
        LOG(INFO) << "Termination threshold reached, stopping server";
        setContinueFlag(false);
      }
    }

    LOG(INFO) << "Successfully processed termination request";
  } catch (const std::exception& ex) {
    LOG(ERROR) << "Error processing termination request: " << ex.what();
    response.status() = false;
  }

  co_return response;
}

folly::coro::Task<thrift::CompletionBarrierResponse>
RdmaGenThriftControlServiceHandler::co_completionBarrier(
    const thrift::CompletionBarrierRequest& request) {
  // `request` dangles once the sleep below suspends: the processor's frame
  // returns while this coroutine sleeps.
  const auto qpn = *request.qpn();
  LOG(INFO) << "Received completion barrier request for QP " << qpn
            << " (client completed " << *request.clientCompleted() << ")";

  thrift::CompletionBarrierResponse response;
  response.status() = false;
  response.serverCompleted() = 0;

  try {
    // Validate the QP is known locally, same as co_terminate. Crucially, this
    // handler must NOT touch the continue flag so the RX poll thread keeps
    // draining completions while we wait.
    bool foundLocally = false;
    for (const auto& qpInfo : *remoteRcQpInfo_) {
      if (qpInfo.qpn == qpn) {
        foundLocally = true;
        break;
      }
    }

    if (!foundLocally) {
      LOG(WARNING) << "received completion barrier request from unknown client";
      co_return response;
    }

    // Poll the injected RX-complete waiter until it reports completion or the
    // barrier timeout expires. We use a coroutine sleep-poll loop (same pattern
    // as the client's createClient retry loop) so the thrift IO thread is not
    // blocked while we wait. Each iteration probes the waiter with a zero
    // timeout so the wait itself is non-blocking.
    constexpr auto kPollInterval = std::chrono::milliseconds(1);
    const auto deadline = std::chrono::steady_clock::now() + barrierTimeout_;
    bool complete = rxCompleteWaiter_(std::chrono::milliseconds(0));
    while (!complete) {
      if (std::chrono::steady_clock::now() >= deadline) {
        LOG(WARNING) << fmt::format(
            "Completion barrier timed out after {}ms waiting for RX completions for QP {}",
            barrierTimeout_.count(),
            qpn);
        break;
      }
      co_await folly::coro::sleep(kPollInterval);
      complete = rxCompleteWaiter_(std::chrono::milliseconds(0));
    }

    response.status() = complete;
    LOG(INFO) << "Completion barrier for QP " << qpn
              << (complete ? " satisfied" : " timed out");
  } catch (const std::exception& ex) {
    LOG(ERROR) << "Error processing completion barrier request: " << ex.what();
    response.status() = false;
  }

  co_return response;
}

folly::coro::Task<thrift::WriteResponse>
RdmaGenThriftControlServiceHandler::co_write(
    const thrift::WriteRequest& request) {
  VLOG(1) << "Received write request with length " << *request.writeLength();

  thrift::WriteResponse response;

  try {
    // Convert thrift QpInfo to native format
    RcQpInfo requestQpInfo =
        RdmaGenThriftConversions::convertFromThriftQpInfo(*request.qpInfo());
    auto qpIdx = static_cast<int>(remoteRcQpInfo_->size());

    if (qpIdx > endpoint_->params().message_size.size() - 1 ||
        endpoint_->params().message_size[qpIdx] < *request.writeLength()) {
      response.status() = false;
    } else {
      remoteRcQpInfo_->push_back(requestQpInfo);
      response.status() = endpoint_->modifyRcQpsRts(
          remoteRcQpInfo_->at(qpIdx),
          qpIdx,
          0,
          endpoint_->params().mtu,
          IBV_QPS_RTS);
      response.qpInfo() = RdmaGenThriftConversions::convertToThriftQpInfo(
          endpoint_->getLocalRcQpInfo(qpIdx));
      if (endpoint_->params().use_block) {
        response.writeAddr() =
            reinterpret_cast<uint64_t>(endpoint_->ctx()->mr_list[0]->addr());
        response.rkey() = endpoint_->ctx()->mr_list[0]->rkey();
      } else {
        response.writeAddr() = reinterpret_cast<uint64_t>(
            endpoint_->ctx()->mr_list[qpIdx]->addr());
        response.rkey() = endpoint_->ctx()->mr_list[qpIdx]->rkey();
      }
    }

    VLOG(1) << "Successfully processed write request";
  } catch (const std::exception& ex) {
    LOG(ERROR) << "Error processing write request: " << ex.what();
    response.status() = false;
    response.qpInfo() = thrift::RcQpInfo{};
    response.writeAddr() = 0;
    response.rkey() = 0;
  }

  co_return response;
}

folly::coro::Task<thrift::ReadResponse>
RdmaGenThriftControlServiceHandler::co_read(
    const thrift::ReadRequest& request) {
  VLOG(1) << "Received read request with length " << *request.readLength();

  thrift::ReadResponse response;

  try {
    // Convert thrift QpInfo to native format
    RcQpInfo requestQpInfo =
        RdmaGenThriftConversions::convertFromThriftQpInfo(*request.qpInfo());
    auto qpIdx = static_cast<int>(remoteRcQpInfo_->size());

    if ((endpoint_->params().role != EndpointRole::ReadResponder &&
         endpoint_->params().role != EndpointRole::AtomicResponder) ||
        qpIdx > endpoint_->params().message_size.size() - 1 ||
        endpoint_->params().message_size[qpIdx] < *request.readLength()) {
      response.status() = false;
    } else {
      remoteRcQpInfo_->push_back(requestQpInfo);
      qpIdx = static_cast<int>(remoteRcQpInfo_->size() - 1);
      response.status() = endpoint_->modifyRcQpsRts(
          remoteRcQpInfo_->at(qpIdx),
          qpIdx,
          0,
          endpoint_->params().mtu,
          IBV_QPS_RTS);
      response.qpInfo() = RdmaGenThriftConversions::convertToThriftQpInfo(
          endpoint_->getLocalRcQpInfo(qpIdx));
      if (endpoint_->params().use_block) {
        response.readAddr() =
            reinterpret_cast<uint64_t>(endpoint_->ctx()->mr_list[0]->addr());
        response.rkey() = endpoint_->ctx()->mr_list[0]->rkey();
      } else {
        response.readAddr() = reinterpret_cast<uint64_t>(
            endpoint_->ctx()->mr_list[qpIdx]->addr());
        response.rkey() = endpoint_->ctx()->mr_list[qpIdx]->rkey();
      }
    }

    VLOG(1) << "Successfully processed read request";
  } catch (const std::exception& ex) {
    LOG(ERROR) << "Error processing read request: " << ex.what();
    response.status() = false;
    response.qpInfo() = thrift::RcQpInfo{};
    response.readAddr() = 0;
    response.rkey() = 0;
  }

  co_return response;
}

folly::coro::Task<thrift::MetadataExchangeResponse>
RdmaGenThriftControlServiceHandler::co_exchangeMetadata(
    const thrift::MetadataExchangeRequest& request) {
  VLOG(1) << "Received exchangeMetadata request with "
          << request.metadata()->size() << " entries";

  thrift::MetadataExchangeResponse response;

  try {
    response.status() = true;
    response.metadata() = *localMetadata_.rlock();

    if (metadataCallback_) {
      auto* connCtx = getRequestContext()->getConnectionContext();
      auto clientAddr = connCtx->getPeerAddress()->getAddressStr();
      metadataCallback_(*request.metadata(), *response.metadata(), clientAddr);
    }

    VLOG(1) << "Successfully processed exchangeMetadata request, returning "
            << response.metadata()->size() << " entries";
  } catch (const std::exception& ex) {
    LOG(ERROR) << "Error processing exchangeMetadata request: " << ex.what();
    response.status() = false;
  }

  co_return response;
}

bool RdmaGenThriftControlServiceHandler::continueFlag() const {
  return continueFlag_->get();
}

void RdmaGenThriftControlServiceHandler::setContinueFlag(bool flag) {
  continueFlag_->set(flag);
}

void RdmaGenThriftControlServiceHandler::setMetadata(
    const std::map<std::string, std::string>& metadata) {
  *localMetadata_.wlock() = metadata;
}

void RdmaGenThriftControlServiceHandler::setMetadataCallback(
    MetadataCallback callback) {
  metadataCallback_ = std::move(callback);
}

void RdmaGenThriftControlServiceHandler::setRxCompleteWaiter(
    RxCompleteWaiter waiter) {
  if (waiter) {
    rxCompleteWaiter_ = std::move(waiter);
  }
}

void RdmaGenThriftControlServiceHandler::setBarrierTimeout(
    std::chrono::milliseconds timeout) {
  barrierTimeout_ = timeout;
}

int RdmaGenThriftControlServiceHandler::getCurrentTerminateCount() const {
  // Use blocking wait since this is called from non-coroutine context
  auto guard =
      folly::coro::blockingWait(terminateMutex_.co_scoped_lock_shared());
  return currentTerminateCount_;
}

// RdmaGenThriftControlServer implementation

RdmaGenThriftControlServer::RdmaGenThriftControlServer(
    uint16_t port,
    std::shared_ptr<RdmaGenEndpoint> endpoint,
    std::shared_ptr<std::vector<RcQpInfo>> remoteRcQpInfo,
    std::shared_ptr<ContinueFlag> continueFlag,
    int terminateThreshold)
    : bindPort_(port),
      handler_(
          std::make_shared<RdmaGenThriftControlServiceHandler>(
              std::move(endpoint),
              std::move(remoteRcQpInfo),
              std::move(continueFlag),
              terminateThreshold,
              this)),
      server_(std::make_unique<apache::thrift::ThriftServer>()) {
  LOG(INFO) << "Creating RdmaGenThriftControlServer on port " << bindPort_;

  // Configure the thrift server.
  // rdma_gen uses ThriftServer as a simple control channel on localhost —
  // it does not need TLS, service discovery, or other production
  // infrastructure. Disabling these prevents lazy creation of infrastructure
  // singletons during serve() whose background threads would block process
  // exit in a minimal virtual machine.
  server_->setInterface(handler_);
  server_->setPort(bindPort_);
  server_->setNumIOWorkerThreads(1);
  server_->setNumCPUWorkerThreads(1);
  server_->setAllowPlaintextOnLoopback(true);
  server_->setSSLPolicy(apache::thrift::SSLPolicy::DISABLED);
  // Disable queue timeout — this is a local test control channel, not a
  // production service. The default 100ms timeout causes load shedding
  // when QP setup takes longer than expected.
  server_->setQueueTimeout(std::chrono::milliseconds(0));
}

RdmaGenThriftControlServer::~RdmaGenThriftControlServer() {
  stop();
}

void RdmaGenThriftControlServer::start() {
  if (serverStarted_.load()) {
    LOG(ERROR) << "Server already started";
    return;
  }

  // Start the server thread
  serverThread_ = std::thread([this]() { this->launchServerInThread(); });

  // Wait for server to be ready (co_onStartServing has been called).
  // Bounded: serve() can fail before ever notifying us -- EADDRINUSE when
  // another process already holds the port is the common case -- and
  // launchServerInThread only logs that exception, so an unbounded wait hangs
  // the process for good. Fail fast and loudly instead; the port is bound
  // within milliseconds of serve() on a healthy start, so this timeout is
  // generous even on emulation.
  {
    std::unique_lock<std::mutex> lock(readyMutex_);
    if (!readyCondition_.wait_for(lock, kServerReadyTimeout, [this] {
          return serverReady_.load();
        })) {
      throw std::runtime_error(
          "Thrift control server failed to start on port " +
          std::to_string(bindPort_) + " within " +
          std::to_string(kServerReadyTimeout.count()) +
          "s (port already in use?); see any 'Error in thrift server thread' "
          "above");
    }
  }

  bindPort_ = server_->getAddress().getPort();
  LOG(INFO) << "RdmaGenThriftControlServer started on port " << bindPort_;
}

void RdmaGenThriftControlServer::notifyServerReady() {
  LOG(INFO) << "Server notified as ready";
  serverReady_.store(true);
  {
    std::lock_guard<std::mutex> lock(readyMutex_);
    readyCondition_.notify_all();
  }
}

void RdmaGenThriftControlServer::stop() {
  LOG(INFO) << "Stopping RdmaGenThriftControlServer";

  if (!serverStarted_.load()) {
    return;
  }

  handler_->setContinueFlag(false);

  if (server_) {
    server_->stop();
  }

  if (serverThread_.joinable()) {
    serverThread_.join();
  }

  serverStarted_.store(false);
  LOG(INFO) << "RdmaGenThriftControlServer stopped";
}

int RdmaGenThriftControlServer::getCurrentTerminateCount() const {
  return handler_->getCurrentTerminateCount();
}

bool RdmaGenThriftControlServer::continueFlag() const {
  return handler_->continueFlag();
}

void RdmaGenThriftControlServer::setContinueFlag(bool flag) {
  handler_->setContinueFlag(flag);
}

void RdmaGenThriftControlServer::setMetadata(
    const std::map<std::string, std::string>& metadata) {
  handler_->setMetadata(metadata);
}

void RdmaGenThriftControlServer::setMetadataCallback(
    MetadataCallback callback) {
  handler_->setMetadataCallback(std::move(callback));
}

void RdmaGenThriftControlServer::setRxCompleteWaiter(RxCompleteWaiter waiter) {
  handler_->setRxCompleteWaiter(std::move(waiter));
}

void RdmaGenThriftControlServer::setBarrierTimeout(
    std::chrono::milliseconds timeout) {
  handler_->setBarrierTimeout(timeout);
}

uint16_t RdmaGenThriftControlServer::getBindPort() const {
  return bindPort_;
}

void RdmaGenThriftControlServer::launchServerInThread() {
  try {
    LOG(INFO) << "Launching thrift server in thread";

    serverStarted_.store(true);

    // This will block until the server is stopped
    server_->serve();

    LOG(INFO) << "Thrift server stopped";

  } catch (const std::exception& ex) {
    LOG(ERROR) << "Error in thrift server thread: " << ex.what();
  }
}

} // namespace facebook::neteng::ai::rdma_gen
