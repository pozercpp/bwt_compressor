#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace bwt::detail {
// Payload: 256 canonical code lengths followed by MSB-first coded bits.
std::vector<std::byte> encode_huffman(std::span<const std::byte> input);
std::vector<std::byte> decode_huffman(std::span<const std::byte> input, std::size_t size);
} // namespace bwt::detail
