#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace bwt::detail {
std::vector<std::byte> encode_arithmetic(std::span<const std::byte> input);
std::vector<std::byte> decode_arithmetic(std::span<const std::byte> input, std::size_t size);
} // namespace bwt::detail
