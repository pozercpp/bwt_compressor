#pragma once

#include <cstdint>
#include <filesystem>

namespace archive {

inline constexpr unsigned max_threads = 4;
[[nodiscard]] unsigned default_thread_count() noexcept;

struct Options {
    unsigned threads = default_thread_count();
};

// Process a single file using the BWA1 container. Return its uncompressed size.
// The output must not exist. On a handled failure, remove the incomplete output.
// Invalid thread counts throw std::invalid_argument; I/O and format errors
// propagate as standard exceptions. No console I/O is performed by the library.
[[nodiscard]] std::uint64_t compress_file(const std::filesystem::path& input,
                                         const std::filesystem::path& output,
                                         Options options = {});
[[nodiscard]] std::uint64_t decompress_file(const std::filesystem::path& input,
                                           const std::filesystem::path& output,
                                           Options options = {});

} // namespace archive
