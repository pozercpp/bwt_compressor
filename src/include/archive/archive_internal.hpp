#pragma once

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
std::uint64_t compress_stream(std::istream&, std::ostream&, unsigned threads);
std::uint64_t decompress_stream(std::istream&, std::ostream&, unsigned threads, bool standalone,
    std::uint64_t max_output = std::numeric_limits<std::uint64_t>::max());
} // namespace archive::detail
