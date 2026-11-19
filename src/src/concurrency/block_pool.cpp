#include <concurrency/block_pool.hpp>
#include <compression/compression.hpp>

#include <stdexcept>
#include <utility>

namespace concurrency {

BlockPool::BlockPool(unsigned count) {
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
BlockPool::~BlockPool() {
    // Drop queued work on errors; running tasks finish before destruction.
    for (auto& worker : workers_) worker.request_stop();
    ready_.notify_all();
}
std::future<BlockPool::Bytes> BlockPool::submit(Bytes input, bool compressing) {
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

} // namespace concurrency
