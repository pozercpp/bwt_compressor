#include <archive/archive.hpp>
#include <archive/archive_internal.hpp>
#include <concurrency/block_pool.hpp>

#include <compression/compression.hpp>
#include <algorithm>
#include <array>
#include <deque>
#include <fstream>
#include <future>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace archive {
namespace detail {
using concurrency::BlockPool;
constexpr std::array magic{std::byte{'B'}, std::byte{'W'}, std::byte{'A'}, std::byte{'1'}};
constexpr std::size_t block_header_size = 17;

void write_bytes(std::ostream& out, std::span<const std::byte> bytes) {
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error("Failed to write output file.");
}

void read_bytes(std::istream& in, std::span<std::byte> bytes) {
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!in) throw std::runtime_error("Truncated archive or input read error.");
}

void write_number(std::ostream& out, std::uint64_t value, unsigned width) {
    std::array<std::byte, 8> bytes{};
    for (unsigned i = 0; i < width; ++i) bytes[i] = static_cast<std::byte>((value >> (8 * i)) & 255u);
    write_bytes(out, std::span(bytes).first(width));
}

std::uint64_t read_number(std::istream& in, unsigned width) {
    std::array<std::byte, 8> bytes{};
    read_bytes(in, std::span(bytes).first(width));
    std::uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i) value |= std::to_integer<std::uint64_t>(bytes[i]) << (8 * i);
    return value;
}

void add_size(std::uint64_t& total, std::size_t size) {
    if (size > std::numeric_limits<std::uint64_t>::max() - total) {
        throw std::runtime_error("File size exceeds the archive format limit.");
    }
    total += size;
}

// Exclusive creation also protects against same-path inputs and existing links.
// On a handled error, remove the incomplete output created by this operation.
class OutputFile {
    std::filesystem::path path_;
    std::ofstream stream_;
    bool complete_ = false;
public:
    explicit OutputFile(const std::filesystem::path& path)
        : path_(path), stream_(path, std::ios::binary | std::ios::out | std::ios::noreplace) {
        if (!stream_) throw std::runtime_error("Cannot create output file (it may already exist): " + path.string());
    }
    OutputFile(const OutputFile&) = delete;
    OutputFile& operator=(const OutputFile&) = delete;
    ~OutputFile() {
        if (!complete_) {
            stream_.close();
            std::error_code error;
            std::filesystem::remove(path_, error);
        }
    }
    std::ostream& stream() { return stream_; }
    void finish() {
        stream_.close();
        if (!stream_) throw std::runtime_error("Failed to finish writing output file.");
        complete_ = true;
    }
};

std::uint64_t compress_stream(std::istream& in, std::ostream& out, unsigned threads) {
    write_bytes(out, magic);
    BlockPool pool(threads);
    std::deque<std::future<BlockPool::Bytes>> pending;
    auto write_next = [&] {
        auto encoded = pending.front().get();
        write_number(out, encoded.size(), 4);
        write_bytes(out, encoded);
        pending.pop_front();
    };
    std::uint64_t total = 0;
    for (;;) {
        if (pending.size() == 2 * threads) write_next();
        std::vector<std::byte> buffer(bwt::max_block_size);
        in.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const auto count = static_cast<std::size_t>(in.gcount());
        if (in.bad() || (in.fail() && !in.eof())) throw std::runtime_error("Failed to read input file.");
        if (count) {
            buffer.resize(count);
            pending.push_back(pool.submit(std::move(buffer), true));
            add_size(total, count);
        }
        if (in.eof()) break;
    }
    while (!pending.empty()) write_next();
    write_number(out, 0, 4); // End marker, required even for an empty archive.
    write_number(out, total, 8);
    return total;
}

std::uint64_t decompress_stream(std::istream& in, std::ostream& out, unsigned threads, bool standalone, std::uint64_t max_output) {
    std::array<std::byte, 4> signature{};
    read_bytes(in, signature);
    if (!std::ranges::equal(signature, magic)) throw std::runtime_error("Unsupported archive format; expected BWA1.");
    BlockPool pool(threads);
    std::deque<std::future<BlockPool::Bytes>> pending;
    std::uint64_t total = 0;
    auto write_next = [&] {
        auto decoded = pending.front().get();
        if (decoded.empty()) throw std::runtime_error("Unexpected empty archive block.");
        if (decoded.size() > max_output - total) throw std::runtime_error("Decoded file exceeds its declared size.");
        write_bytes(out, decoded);
        add_size(total, decoded.size());
        pending.pop_front();
    };
    for (;;) {
        if (pending.size() == 2 * threads) write_next();
        const auto size = read_number(in, 4);
        if (size == 0) break;
        if (size < block_header_size || size > bwt::max_block_size + block_header_size) {
            throw std::runtime_error("Invalid archive block size.");
        }
        std::vector<std::byte> encoded(static_cast<std::size_t>(size));
        read_bytes(in, encoded);
        pending.push_back(pool.submit(std::move(encoded), false));
    }
    while (!pending.empty()) write_next();
    if (read_number(in, 8) != total) throw std::runtime_error("Archive size check failed.");
    if (standalone && in.peek() != std::char_traits<char>::eof()) throw std::runtime_error("Unexpected trailing archive data.");
    if (standalone && (in.bad() || !in.eof())) throw std::runtime_error("Failed to read archive ending.");
    return total;
}

std::uint64_t process_file(const std::filesystem::path& input_path,
                           const std::filesystem::path& output_path,
                           Options options, bool compressing) {
    if (options.threads < 1 || options.threads > max_threads) {
        throw std::invalid_argument("Worker count must be between 1 and 4.");
    }
    if (!std::filesystem::is_regular_file(input_path)) {
        throw std::runtime_error("Input must be an existing regular file.");
    }
    std::ifstream input(input_path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open input file: " + input_path.string());
    OutputFile output(output_path);
    const auto total = compressing ? compress_stream(input, output.stream(), options.threads)
                                  : decompress_stream(input, output.stream(), options.threads, true);
    output.finish();
    return total;
}
} // namespace

unsigned default_thread_count() noexcept {
    return std::clamp(std::thread::hardware_concurrency(), 1u, max_threads);
}

std::uint64_t compress_file(const std::filesystem::path& input,
                            const std::filesystem::path& output, Options options) {
    return detail::process_file(input, output, options, true);
}

std::uint64_t decompress_file(const std::filesystem::path& input,
                              const std::filesystem::path& output, Options options) {
    return detail::process_file(input, output, options, false);
}
} // namespace archive
