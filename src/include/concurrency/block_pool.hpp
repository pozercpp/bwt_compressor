#pragma once

#include <cstddef>

#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

namespace concurrency {

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
    explicit BlockPool(unsigned count);
    ~BlockPool();
    BlockPool(const BlockPool&) = delete;
    BlockPool& operator=(const BlockPool&) = delete;

    std::future<Bytes> submit(Bytes input, bool compressing);

};

} // namespace concurrency
