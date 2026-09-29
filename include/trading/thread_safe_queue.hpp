#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace trading {

// Bounded FIFO. Closing wakes all waiters; queued values remain available to drain.
template <typename T>
class ThreadSafeQueue {
public:
    explicit ThreadSafeQueue(std::size_t capacity = 256) : capacity_(capacity) {
        if (capacity == 0) { throw std::invalid_argument("Queue capacity must be positive"); }
    }
    ThreadSafeQueue(const ThreadSafeQueue&) = delete;
    ThreadSafeQueue& operator=(const ThreadSafeQueue&) = delete;

    bool push(T value) {
        std::unique_lock lock(mutex_);
        writable_.wait(lock, [this] { return closed_ || queue_.size() < capacity_; });
        if (closed_) { return false; }
        queue_.push_back(std::move(value));
        readable_.notify_one();
        return true;
    }

    std::optional<T> pop() {
        std::unique_lock lock(mutex_);
        readable_.wait(lock, [this] { return closed_ || !queue_.empty(); });
        if (queue_.empty()) { return std::nullopt; }
        T value = std::move(queue_.front());
        queue_.pop_front();
        writable_.notify_one();
        return value;
    }

    void close() {
        std::lock_guard lock(mutex_);
        closed_ = true;
        readable_.notify_all();
        writable_.notify_all();
    }

private:
    const std::size_t capacity_;
    std::mutex mutex_;
    std::condition_variable readable_;
    std::condition_variable writable_;
    std::deque<T> queue_;
    bool closed_{};
};

} // namespace trading
