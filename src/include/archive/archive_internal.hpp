#pragma once

#include <archive/archive.hpp>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <limits>
#include <span>

namespace archive::detail {
void write_bytes(std::ostream&, std::span<const std::byte>);
void read_bytes(std::istream&, std::span<std::byte>);
void write_number(std::ostream&, std::uint64_t, unsigned width);
std::uint64_t read_number(std::istream&, unsigned width);
void add_size(std::uint64_t&, std::size_t);
class ProgressTracker {
    ProgressCallback callback_;
    std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
    Progress state_;
    void emit() {
        if (!callback_) return;
        state_.elapsed = std::chrono::steady_clock::now() - started_;
        callback_(state_);
    }
public:
    explicit ProgressTracker(const ProgressCallback& callback) : callback_(callback) {}
    void start(std::uint64_t input, std::optional<std::uint64_t> total) {
        state_.input_bytes = input;
        state_.total_bytes = total;
        emit();
    }
    void advance(std::uint64_t bytes) {
        add_size(state_.processed_bytes, bytes);
        emit();
    }
    void finish(std::uint64_t result) {
        state_.total_bytes = state_.processed_bytes;
        state_.result_bytes = result;
        state_.completed = true;
        emit();
    }
};
std::uint64_t list_stream(std::istream&, bool standalone);
std::uint64_t compress_stream(std::istream&, std::ostream&, unsigned threads, ProgressTracker* progress = nullptr);
std::uint64_t decompress_stream(std::istream&, std::ostream&, unsigned threads, bool standalone,
    std::uint64_t max_output = std::numeric_limits<std::uint64_t>::max(),
    ProgressTracker* progress = nullptr);
} // namespace archive::detail
