#pragma once

#include <compression.hpp>

#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace archive::detail {

// The caller limits submitted-but-unwritten blocks and consumes futures in order.
class BlockPool {
public:
    using Bytes = std::vector<std::byte>;
private:
    std::mutex mutex_;
    std::condition_variable_any ready_;
    std::deque<std::packaged_task<Bytes()>> tasks_;
    // Destroy workers before the queue and synchronization objects.
    std::vector<std::jthread> workers_;
public:
    explicit BlockPool(unsigned count) {
        if (count == 0) throw std::invalid_argument("Worker count must be positive.");
        workers_.reserve(count);
        for (unsigned i = 0; i < count; ++i) {
            workers_.emplace_back([this](std::stop_token stop) {
                for (;;) {
                    std::packaged_task<Bytes()> task;
                    {
                        std::unique_lock lock(mutex_);
                        ready_.wait(lock, stop, [this] { return !tasks_.empty(); });
                        if (stop.stop_requested()) return;
                        task = std::move(tasks_.front());
                        tasks_.pop_front();
                    }
                    task(); // Exceptions are delivered through the future.
                }
            });
        }
    }
    ~BlockPool() {
        // Drop queued work on errors; running tasks finish before destruction.
        for (auto& worker : workers_) worker.request_stop();
        ready_.notify_all();
    }
    BlockPool(const BlockPool&) = delete;
    BlockPool& operator=(const BlockPool&) = delete;

    std::future<Bytes> submit(Bytes input, bool compressing) {
        std::packaged_task<Bytes()> task([input = std::move(input), compressing] {
            if (compressing) return bwt::compress(input, {.parallel_entropy = false});
            return bwt::decompress(input);
        });
        auto result = task.get_future();
        {
            std::lock_guard lock(mutex_);
            tasks_.push_back(std::move(task));
        }
        ready_.notify_one();
        return result;
    }
};

} // namespace archive::detail
