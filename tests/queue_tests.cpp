#include "trading/thread_safe_queue.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <thread>

using namespace std::chrono_literals;
using trading::ThreadSafeQueue;

TEST(Queue, PreservesFifoAndDrainsAfterClose) {
    ThreadSafeQueue<int> queue(2);
    ASSERT_TRUE(queue.push(11));
    ASSERT_TRUE(queue.push(22));
    queue.close();
    queue.close();
    EXPECT_FALSE(queue.push(33));
    EXPECT_EQ(queue.pop(), 11);
    EXPECT_EQ(queue.pop(), 22);
    EXPECT_FALSE(queue.pop().has_value());
}

TEST(Queue, ClosingWakesAnEmptyConsumer) {
    ThreadSafeQueue<int> queue(1);
    auto consumer = std::async(std::launch::async, [&] { return queue.pop(); });
    EXPECT_EQ(consumer.wait_for(20ms), std::future_status::timeout);
    queue.close();
    ASSERT_EQ(consumer.wait_for(2s), std::future_status::ready);
    EXPECT_FALSE(consumer.get().has_value());
}

TEST(Queue, ClosingWakesAProducerOnAFullQueue) {
    ThreadSafeQueue<int> queue(1);
    ASSERT_TRUE(queue.push(1));
    auto producer = std::async(std::launch::async, [&] { return queue.push(2); });
    EXPECT_EQ(producer.wait_for(20ms), std::future_status::timeout);
    queue.close();
    ASSERT_EQ(producer.wait_for(2s), std::future_status::ready);
    EXPECT_FALSE(producer.get());
    EXPECT_EQ(queue.pop(), 1);
}

TEST(Queue, TransfersAcrossThreadsWithBackpressure) {
    ThreadSafeQueue<int> queue(3);
    std::jthread producer([&] {
        for (int value = 0; value < 10'000; ++value) {
            if (!queue.push(value)) { return; }
        }
        queue.close();
    });
    int expected = 0;
    while (auto value = queue.pop()) { EXPECT_EQ(*value, expected++); }
    EXPECT_EQ(expected, 10'000);
}

TEST(Queue, RejectsZeroCapacity) {
    EXPECT_THROW(ThreadSafeQueue<int> queue(0), std::invalid_argument);
}
