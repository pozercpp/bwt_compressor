#pragma once

#include <cstdint>
#include <filesystem>
#include <span>

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

// BWA2: store files/directories under their basenames. Reject links, special
// files and duplicate stored paths. Output must not exist. Return total bytes.
[[nodiscard]] std::uint64_t create(std::span<const std::filesystem::path> inputs,
                                  const std::filesystem::path& output,
                                  Options options = {});

// Detect format: BWA2 extracts into a new directory; legacy BWA1 into a new
// file. Publish only after all records have been validated successfully.
[[nodiscard]] std::uint64_t extract(const std::filesystem::path& input,
                                   const std::filesystem::path& output,
                                   Options options = {});

} // namespace archive
