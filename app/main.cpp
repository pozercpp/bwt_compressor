#include <compression.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
constexpr std::array magic{std::byte{'B'}, std::byte{'W'}, std::byte{'A'}, std::byte{'1'}};
constexpr std::size_t block_header_size = 17;

void usage(std::ostream& out) {
    out << "Usage:\n"
        << "  compressor compress <input-file> <archive>\n"
        << "  compressor decompress <archive> <output-file>\n"
        << "  compressor --help\n"
        << "Existing output files are never overwritten.\n";
}

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

std::uint64_t compress_file(std::istream& in, std::ostream& out) {
    write_bytes(out, magic);
    std::vector<std::byte> buffer(bwt::max_block_size);
    std::uint64_t total = 0;
    for (;;) {
        in.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const auto count = static_cast<std::size_t>(in.gcount());
        if (in.bad() || (in.fail() && !in.eof())) throw std::runtime_error("Failed to read input file.");
        if (count) {
            const auto encoded = bwt::compress(std::span(buffer).first(count));
            write_number(out, encoded.size(), 4);
            write_bytes(out, encoded);
            add_size(total, count);
        }
        if (in.eof()) break;
    }
    write_number(out, 0, 4); // End marker, required even for an empty archive.
    write_number(out, total, 8);
    return total;
}

std::uint64_t decompress_file(std::istream& in, std::ostream& out) {
    std::array<std::byte, 4> signature{};
    read_bytes(in, signature);
    if (!std::ranges::equal(signature, magic)) throw std::runtime_error("Unsupported archive format; expected BWA1.");
    std::uint64_t total = 0;
    for (;;) {
        const auto size = read_number(in, 4);
        if (size == 0) break;
        if (size < block_header_size || size > bwt::max_block_size + block_header_size) {
            throw std::runtime_error("Invalid archive block size.");
        }
        std::vector<std::byte> encoded(static_cast<std::size_t>(size));
        read_bytes(in, encoded);
        const auto decoded = bwt::decompress(encoded);
        if (decoded.empty()) throw std::runtime_error("Unexpected empty archive block.");
        write_bytes(out, decoded);
        add_size(total, decoded.size());
    }
    if (read_number(in, 8) != total) throw std::runtime_error("Archive size check failed.");
    if (in.peek() != std::char_traits<char>::eof()) throw std::runtime_error("Unexpected trailing archive data.");
    if (in.bad() || !in.eof()) throw std::runtime_error("Failed to read archive ending.");
    return total;
}
} // namespace

int main(int argc, char* argv[]) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        usage(std::cout);
        return 0;
    }
    if (argc != 4 || (std::string_view(argv[1]) != "compress" && std::string_view(argv[1]) != "decompress")) {
        usage(std::cerr);
        return 2;
    }
    try {
        const std::filesystem::path input_path(argv[2]);
        if (!std::filesystem::is_regular_file(input_path)) throw std::runtime_error("Input must be an existing regular file.");
        std::ifstream input(input_path, std::ios::binary);
        if (!input) throw std::runtime_error("Cannot open input file: " + input_path.string());
        OutputFile output(argv[3]);
        const bool compressing = std::string_view(argv[1]) == "compress";
        const auto total = compressing ? compress_file(input, output.stream())
                                      : decompress_file(input, output.stream());
        output.finish();
        std::cout << (compressing ? "Compressed " : "Decompressed ") << total << " bytes into " << argv[3] << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
