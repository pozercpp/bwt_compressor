#pragma once

#include <cstdint>
#include <chrono>
#include <optional>
#include <filesystem>
#include <functional>
#include <string>
#include <span>

namespace archive {

inline constexpr unsigned max_threads = 4;
[[nodiscard]] unsigned default_thread_count() noexcept;

// Logical file bytes count each hard-link name; archive sizes include framing.
struct Progress {
    std::uint64_t processed_bytes = 0;
    std::optional<std::uint64_t> total_bytes;
    std::uint64_t input_bytes = 0;
    std::optional<std::uint64_t> result_bytes;
    std::chrono::duration<double> elapsed{};
    bool completed = false;
};
using ProgressCallback = std::function<void(const Progress&)>;

struct Options {
    unsigned threads = default_thread_count();
    bool allow_fifo = false;
    // create/extract and legacy file APIs: synchronous, on the calling thread.
    // Called initially, after written blocks/link records, and after publication.
    // Exceptions propagate; a final callback exception leaves the published output.
    // list/test do not report progress. Callbacks must not modify operation files.
    ProgressCallback progress;

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

// Listing metadata is reported incrementally; earlier callbacks can precede an
// error. BWA1 has one unnamed file (empty path); directories have size zero.
enum class EntryType { directory = 1, file = 2, symlink = 3, hardlink = 4, fifo = 5 };

struct EntryInfo {
    std::string path;
    bool directory;
    std::uint64_t size;
    EntryType type = EntryType::file;
    std::string target;
};
using EntryVisitor = std::function<void(const EntryInfo&)>;

// Read metadata/framing without decoding payloads or checking CRC. No writes.
[[nodiscard]] std::uint64_t list(const std::filesystem::path& input,
                                const EntryVisitor& visitor);
// Fully decode and validate, discarding output. No filesystem writes.
[[nodiscard]] std::uint64_t test(const std::filesystem::path& input,
                                Options options = {});

// BWA4: preserve rwx/mtime, internal symlinks and regular-file hard links.
// FIFO requires allow_fifo. Devices/sockets are rejected. Output must not exist.
// Return logical bytes, counting each hard-link name.
[[nodiscard]] std::uint64_t create(std::span<const std::filesystem::path> inputs,
                                  const std::filesystem::path& output,
                                  Options options = {});

// Detect format: BWA2/BWA3/BWA4 extract into a new directory; legacy BWA1 into a new
// file. Publish only after all records have been validated successfully.
[[nodiscard]] std::uint64_t extract(const std::filesystem::path& input,
                                   const std::filesystem::path& output,
                                   Options options = {});

} // namespace archive
