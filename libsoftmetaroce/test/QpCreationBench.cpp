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

// Microbenchmark for creating and destroying queue pairs through the verbs, in
// batches of 32K. Each case times one direction; the other runs suspended.

#include <folly/Benchmark.h>
#include <folly/init/Init.h>
#include <folly/portability/GMock.h>

#include <libsoftmetaroce/MetaRoceVerbs.h>
#include <libsoftmetaroce/test/MockUDPSocket.h>

#include <vector>

using libsoftmetaroce::Modifier;
using ::testing::NiceMock;

namespace {

constexpr size_t kQueuePairs = 32768;
constexpr int kCqEntries = 10;

std::pair<std::unique_ptr<folly::AsyncUDPSocket>, std::unique_ptr<Modifier>>
mockUDPSocketMaker(folly::EventBase* evb) {
  return std::make_pair(
      std::make_unique<NiceMock<libsoftmetaroce::MockUDPSocket>>(evb), nullptr);
}

struct Device {
  Device() {
    CHECK(cq != nullptr) << "create_cq failed, errno " << errno;
    CHECK(pd != nullptr) << "alloc_pd failed, errno " << errno;
  }

  ~Device() {
    CHECK_EQ(verbs.ibv_dealloc_pd(pd), 0);
    CHECK_EQ(verbs.ibv_destroy_cq(cq), 0);
  }

  libsoftmetaroce::MetaRoceVerbs verbs{mockUDPSocketMaker};
  struct ibv_context context{};
  struct ibv_cq* cq =
      verbs.ibv_create_cq(&context, kCqEntries, nullptr, nullptr, 0);
  struct ibv_pd* pd = verbs.ibv_alloc_pd(&context);
};

std::vector<struct ibv_qp*> createQueuePairs(Device& dev) {
  struct ibv_qp_init_attr attr{};
  attr.send_cq = dev.cq;
  attr.recv_cq = dev.cq;
  attr.qp_type = IBV_QPT_RC;
  std::vector<struct ibv_qp*> qps;
  qps.reserve(kQueuePairs);
  for (size_t i = 0; i < kQueuePairs; ++i) {
    struct ibv_qp* qp = dev.verbs.ibv_create_qp(dev.pd, &attr);
    CHECK(qp != nullptr) << "create " << i << " failed, errno " << errno;
    qps.push_back(qp);
  }
  return qps;
}

void destroyQueuePairs(Device& dev, const std::vector<struct ibv_qp*>& qps) {
  for (struct ibv_qp* qp : qps) {
    CHECK_EQ(dev.verbs.ibv_destroy_qp(qp), 0);
  }
}

unsigned create32kQueuePairs(Device& dev, unsigned iters) {
  for (unsigned i = 0; i < iters; ++i) {
    std::vector<struct ibv_qp*> qps = createQueuePairs(dev);
    folly::BenchmarkSuspender suspend;
    destroyQueuePairs(dev, qps);
  }
  return iters;
}

unsigned destroy32kQueuePairs(Device& dev, unsigned iters) {
  for (unsigned i = 0; i < iters; ++i) {
    std::vector<struct ibv_qp*> qps;
    {
      folly::BenchmarkSuspender suspend;
      qps = createQueuePairs(dev);
    }
    destroyQueuePairs(dev, qps);
  }
  return iters;
}

} // namespace

int main(int argc, char** argv) {
  folly::Init init(&argc, &argv);
  // Owned here, so its sender and receiver threads stop before folly's
  // singletons shut down.
  Device dev;
  folly::addBenchmark(__FILE__, "create32kQueuePairs", [&dev](unsigned iters) {
    return create32kQueuePairs(dev, iters);
  });
  folly::addBenchmark(__FILE__, "destroy32kQueuePairs", [&dev](unsigned iters) {
    return destroy32kQueuePairs(dev, iters);
  });
  folly::runBenchmarks();
  return 0;
}
