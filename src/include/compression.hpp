#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace bwt {

inline constexpr std::size_t max_block_size = 1024 * 1024;

struct CompressionOptions {
    // Disable when an outer pool already processes multiple blocks.
    bool parallel_entropy = true;
};

// Compress one block of arbitrary bytes. Throws std::length_error above
// max_block_size. The result includes format metadata and a CRC-32 checksum.
[[nodiscard]] std::vector<std::byte> compress(std::span<const std::byte> input);
[[nodiscard]] std::vector<std::byte> compress(std::span<const std::byte> input,
                                             CompressionOptions options);

// Decode one complete block. Throws std::invalid_argument on invalid format
// or checksum mismatch. Decoded size never exceeds max_block_size.
[[nodiscard]] std::vector<std::byte> decompress(std::span<const std::byte> encoded);

} // namespace bwt
