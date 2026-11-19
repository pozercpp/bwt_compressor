#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace bwt::detail {
// Internal transforms. The block layer validates sizes and the primary index.
// encode_bwt requires a nonempty input of at most max_block_size bytes.
std::pair<std::vector<std::byte>, std::uint32_t> encode_bwt(std::span<const std::byte> input);
std::vector<std::byte> decode_bwt(std::span<const std::byte> last, std::size_t primary);
std::vector<std::byte> encode_mtf(std::span<const std::byte> last);
std::vector<std::byte> decode_mtf(std::span<const std::byte> codes);
std::vector<std::byte> encode_rle(std::span<const std::byte> codes);
std::vector<std::byte> decode_rle_mtf(std::span<const std::byte> payload, std::size_t size);
} // namespace bwt::detail
