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
#include <future>
#include <thread>
#include <vector>

#include <folly/coro/BlockingWait.h>
#include <folly/coro/Task.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/futures/Future.h>
#include <folly/synchronization/Baton.h>
#include <gtest/gtest.h>

#include <libsoftmetaroce/MultiUnboundedQueue.h>

using namespace testing;
namespace libsoftmetaroce {

class MultiUnboundedQueueTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Create a fresh queue for each test
    queue_ = std::make_unique<MultiUnboundedQueue<int, int>>();
  }

  void TearDown() override {
    queue_.reset();
  }

  std::unique_ptr<MultiUnboundedQueue<int, int>> queue_;
};

// Test adding duplicate queue keys throws exception
TEST_F(MultiUnboundedQueueTest, AddDuplicateQueueThrows) {
  queue_->addQueue(0);
  EXPECT_THROW(queue_->addQueue(0), std::invalid_argument);
}

// Test removing queue
TEST_F(MultiUnboundedQueueTest, RemoveQueue) {
  queue_->addQueue(0);
  EXPECT_NO_THROW(queue_->removeQueue(0));
  EXPECT_TRUE(queue_->empty());
}

// Test enqueue to existing queue
TEST_F(MultiUnboundedQueueTest, EnqueueToExistingQueue) {
  queue_->addQueue(0);
  EXPECT_NO_THROW(queue_->enqueue(0, 42));
  EXPECT_EQ(queue_->size(), 1);
}

// Test enqueue to non-existent queue throws exception
TEST_F(MultiUnboundedQueueTest, EnqueueToNonExistentQueueThrows) {
  EXPECT_THROW(queue_->enqueue(0, 42), std::out_of_range);
}

// Test basic enqueue/dequeue functionality
TEST_F(MultiUnboundedQueueTest, BasicEnqueueDequeue) {
  queue_->addQueue(0);
  int val = 42;
  queue_->enqueue(0, std::move(val));

  auto result = queue_->dequeue();
  EXPECT_EQ(result.first, 0);
  EXPECT_EQ(result.second, 42);

  EXPECT_TRUE(queue_->empty());
}

// Test dequeue from empty queue blocks (timeout test)
TEST_F(MultiUnboundedQueueTest, DequeueFromEmptyQueueBlocks) {
  queue_->addQueue(0);

  // Post before dequeue() so the wait below observes a dequeuer that has
  // actually started, not one the scheduler simply has not run yet.
  folly::Baton<> dequeuerStarted;
  auto future = std::async(std::launch::async, [this, &dequeuerStarted]() {
    dequeuerStarted.post();
    return queue_->dequeue();
  });

  dequeuerStarted.wait();
  EXPECT_EQ(
      future.wait_for(std::chrono::seconds(0)), std::future_status::timeout)
      << "dequeue on an empty queue must block until an enqueue";

  // Now enqueue something to unblock
  int val = 123;
  queue_->enqueue(0, std::move(val));

  auto result = future.get();
  EXPECT_EQ(result.first, 0)
      << "the unblocked dequeue must return the enqueued item's queue key";
  EXPECT_EQ(result.second, 123)
      << "the unblocked dequeue must return the enqueued value, proving it "
         "waited for the enqueue instead of returning early on the empty queue";

  EXPECT_TRUE(queue_->empty());
}

// Test round-robin behavior with multiple queues
TEST_F(MultiUnboundedQueueTest, RoundRobinBehavior) {
  queue_->addQueue(0);
  queue_->addQueue(1);
  queue_->addQueue(2);

  // Enqueue values to each queue
  int val1 = 1, val2 = 2, val3 = 3, val4 = 4, val5 = 5;
  queue_->enqueue(0, std::move(val1));
  queue_->enqueue(1, std::move(val2));
  queue_->enqueue(2, std::move(val3));
  queue_->enqueue(0, std::move(val4));
  queue_->enqueue(1, std::move(val5));
  EXPECT_EQ(queue_->size(), 5);

  // Should dequeue in round-robin order based on queue key ordering
  auto result1 = queue_->dequeue();
  auto result2 = queue_->dequeue();
  auto result3 = queue_->dequeue();
  auto result4 = queue_->dequeue();
  auto result5 = queue_->dequeue();

  // Expected order (queue:val) - 0:1, 1:2, 2:3, 0:4, 1:5
  EXPECT_EQ(result1.first, 0);
  EXPECT_EQ(result1.second, 1);

  EXPECT_EQ(result2.first, 1);
  EXPECT_EQ(result2.second, 2);

  EXPECT_EQ(result3.first, 2);
  EXPECT_EQ(result3.second, 3);

  EXPECT_EQ(result4.first, 0);
  EXPECT_EQ(result4.second, 4);

  EXPECT_EQ(result5.first, 1);
  EXPECT_EQ(result5.second, 5);

  EXPECT_TRUE(queue_->empty());
}

// Test queue with only one active queue at a time
TEST_F(MultiUnboundedQueueTest, SingleActiveQueue) {
  queue_->addQueue(0);

  for (int i = 0; i < 5; ++i) {
    int val = i;
    queue_->enqueue(0, std::move(val));
  }

  for (int i = 0; i < 5; ++i) {
    auto result = queue_->dequeue();
    EXPECT_EQ(result.second, i);
  }

  EXPECT_TRUE(queue_->empty());
}

// Test interleaved enqueue/dequeue operations
TEST_F(MultiUnboundedQueueTest, InterleavedOperations) {
  queue_->addQueue(0);
  queue_->addQueue(1);

  int val1 = 1;
  queue_->enqueue(0, std::move(val1));
  auto result1 = queue_->dequeue();
  EXPECT_EQ(result1.first, 0);
  EXPECT_EQ(result1.second, 1);

  int val2 = 2;
  queue_->enqueue(1, std::move(val2));
  auto result2 = queue_->dequeue();
  EXPECT_EQ(result2.first, 1);
  EXPECT_EQ(result2.second, 2);

  int val3 = 3, val4 = 4;
  queue_->enqueue(0, std::move(val3));
  queue_->enqueue(1, std::move(val4));
  auto result3 = queue_->dequeue();
  auto result4 = queue_->dequeue();

  // Should maintain round-robin order
  EXPECT_EQ(result3.first, 0);
  EXPECT_EQ(result3.second, 3);

  EXPECT_EQ(result4.first, 1);
  EXPECT_EQ(result4.second, 4);

  EXPECT_TRUE(queue_->empty());
}

// Test removing queue that has pending items
TEST_F(MultiUnboundedQueueTest, RemoveQueueWithPendingItems) {
  queue_->addQueue(0);
  int val = 42;
  queue_->enqueue(0, std::move(val));
  EXPECT_EQ(queue_->size(), 1);

  // Remove the queue - this should not cause issues
  queue_->removeQueue(0);

  // The pending item should be lost, which is expected behavior
  EXPECT_TRUE(queue_->empty());
}

// Test concurrent enqueues from multiple threads
TEST_F(MultiUnboundedQueueTest, ConcurrentEnqueues) {
  queue_->addQueue(0);
  queue_->addQueue(1);

  const int numThreads = 4;
  const int itemsPerThread = 10;
  const int numItems = numThreads * itemsPerThread;
  std::vector<std::thread> threads;

  threads.reserve(numThreads);
  for (int t = 0; t < numThreads; ++t) {
    threads.emplace_back([this, t]() {
      for (int i = 0; i < itemsPerThread; ++i) {
        int queueKey = (i % 2 == 0) ? 0 : 1;
        int val = t * itemsPerThread + i;
        queue_->enqueue(queueKey, std::move(val));
      }
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }

  EXPECT_EQ(queue_->size(), numThreads * itemsPerThread);

  // Dequeue all items
  for (int i = 0; i < numItems; ++i) {
    queue_->dequeue();
  }

  EXPECT_TRUE(queue_->empty());
}

// Test queue behavior after adding and removing queues dynamically
TEST_F(MultiUnboundedQueueTest, DynamicQueueManagement) {
  queue_->addQueue(0);
  queue_->addQueue(1);
  queue_->addQueue(2);

  int val1 = 1, val2 = 2, val3 = 3;
  queue_->enqueue(0, std::move(val1));
  queue_->enqueue(1, std::move(val2));
  queue_->enqueue(2, std::move(val3));
  EXPECT_EQ(queue_->size(), 3);

  // Remove temp queues
  queue_->removeQueue(0);
  queue_->removeQueue(1);
  EXPECT_EQ(queue_->size(), 1);

  // Add new queue
  queue_->addQueue(3);
  int val4 = 4, val5 = 5;
  queue_->enqueue(3, std::move(val4));
  queue_->enqueue(2, std::move(val5));
  EXPECT_EQ(queue_->size(), 3);

  // Should dequeue from remaining queues
  auto result1 = queue_->dequeue();
  auto result2 = queue_->dequeue();
  auto result3 = queue_->dequeue();

  // Results should come in order of queue keys (queue:val) - 2:3, 3:4, 2:5
  EXPECT_EQ(result1.first, 2);
  EXPECT_EQ(result1.second, 3);

  EXPECT_EQ(result2.first, 3);
  EXPECT_EQ(result2.second, 4);

  EXPECT_EQ(result3.first, 2);
  EXPECT_EQ(result3.second, 5);

  EXPECT_TRUE(queue_->empty());
}

// Test large number of queues
TEST_F(MultiUnboundedQueueTest, ManyQueues) {
  folly::coro::blockingWait([&]() -> folly::coro::Task<void> {
    const int numQueues = 1000;
    // Enough rounds to see the rotation visit every queue repeatedly; depth
    // beyond that exercises folly's UnboundedQueue, not this one.
    const int numItemsPerQueue = 10;
    const int numItems = numQueues * numItemsPerQueue;

    // Add many queues
    for (int i = 0; i < numQueues; ++i) {
      co_await queue_->co_addQueue(i);
    }

    // Enqueue many items to each queue in sequence
    for (int i = 0; i < numItems; ++i) {
      int val = i;
      co_await queue_->co_enqueue(i % numQueues, std::move(val));
    }
    EXPECT_EQ(co_await queue_->co_size(), numItems);

    // Dequeue all items. Since they were inserted in round-robin order, they
    // should dequeue in order as well.
    for (int i = 0; i < numItems; ++i) {
      auto result = co_await queue_->co_dequeue();
      EXPECT_EQ(result.second, i);
      auto size = co_await queue_->co_size();
      EXPECT_EQ(size, numItems - (i + 1));
      if (result.second != i || size != numItems - (i + 1)) {
        LOG(ERROR) << "Aborting test after checking item " << i
                   << " to avoid log overload.";
        co_return;
      }
    }
    EXPECT_TRUE(co_await queue_->co_empty());
  }());
}

} // namespace libsoftmetaroce
