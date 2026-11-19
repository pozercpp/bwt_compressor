#include <compression/compression.hpp>
#include <compression/bwt_mtf_rle/transforms.hpp>
#include <compression/arithmetic/arithmetic.hpp>
#include <compression/huffman/huffman.hpp>

#include <algorithm>
#include <future>
#include <ranges>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace bwt {
namespace {
constexpr std::size_t header_size = 17;
constexpr std::array magic{std::byte{'B'}, std::byte{'W'}, std::byte{'C'}, std::byte{'1'}};

using Bytes = std::vector<std::byte>;

[[noreturn]] void invalid() {
    throw std::invalid_argument("Invalid or corrupted BWT block.");
}

unsigned value(std::byte b) { return std::to_integer<unsigned>(b); }

std::uint32_t crc32(std::span<const std::byte> data) {
    std::uint32_t crc = 0xffffffffu;
    for (auto b : data) {
        crc ^= value(b);
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xedb88320u : 0u);
        }
    }
    return ~crc;
}

void append_u32(Bytes& out, std::uint32_t n) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::byte>((n >> shift) & 255u));
    }
}

std::uint32_t read_u32(std::span<const std::byte> data, std::size_t offset) {
    std::uint32_t n = 0;
    for (unsigned i = 0; i < 4; ++i) {
        n |= static_cast<std::uint32_t>(value(data[offset + i])) << (8 * i);
    }
    return n;
}

} // namespace

std::vector<std::byte> compress(std::span<const std::byte> input) {
    return compress(input, {});
}

std::vector<std::byte> compress(std::span<const std::byte> input, CompressionOptions options) {
    if (input.size() > max_block_size) {
        throw std::length_error("Input block exceeds the 1 MiB limit.");
    }
    Bytes payload;
    std::uint32_t primary = 0;
    unsigned mode = 0;
    if (!input.empty()) {
        auto [last, index] = detail::encode_bwt(input);
        const auto codes = detail::encode_mtf(last);
        const auto rle = detail::encode_rle(codes);
        Bytes entropy, huffman;
        // Small RLE streams do not justify creating a worker thread.
        constexpr std::size_t parallel_threshold = 64 * 1024;
        if (options.parallel_entropy && rle.size() >= parallel_threshold) {
            // The future is destroyed before rle, including on exceptions:
            // the worker always finishes while the shared input is alive.
            auto arithmetic = std::async(std::launch::async, [&rle] {
                return detail::encode_arithmetic(rle);
            });
            huffman = detail::encode_huffman(rle);
            entropy = arithmetic.get();
        } else {
            entropy = detail::encode_arithmetic(rle);
            huffman = detail::encode_huffman(rle);
        }
        unsigned candidate = 3;
        if (huffman.size() < entropy.size()) {
            entropy = std::move(huffman);
            candidate = 4;
        }
        // Both entropy decoders need the intermediate RLE byte count.
        append_u32(payload, static_cast<std::uint32_t>(rle.size()));
        payload.insert(payload.end(), entropy.begin(), entropy.end());
        if (payload.size() < input.size()) {
            primary = index;
            mode = candidate;
        }
    }
    if (mode == 0) payload.assign(input.begin(), input.end());
    Bytes result;
    result.reserve(header_size + payload.size());
    result.insert(result.end(), magic.begin(), magic.end());
    result.push_back(static_cast<std::byte>(mode));
    append_u32(result, static_cast<std::uint32_t>(input.size()));
    append_u32(result, primary);
    append_u32(result, crc32(input));
    result.insert(result.end(), payload.begin(), payload.end());
    return result;
}

std::vector<std::byte> decompress(std::span<const std::byte> encoded) {
    if (encoded.size() < header_size || encoded.size() > header_size + max_block_size) invalid();
    if (!std::ranges::equal(magic, encoded.first(magic.size()))) invalid();
    const auto mode = value(encoded[4]);
    const auto size = read_u32(encoded, 5);
    const auto primary = read_u32(encoded, 9);
    const auto checksum = read_u32(encoded, 13);
    if (size > max_block_size || mode > 4) invalid();
    const auto payload = encoded.subspan(header_size);
    Bytes result;
    if (mode == 0) {
        if (primary != 0 || payload.size() != size) invalid();
        result.assign(payload.begin(), payload.end());
    } else {
        if (size == 0 || primary >= size || payload.size() >= size) invalid();
        Bytes last;
        if (mode == 1) {
            last = detail::decode_rle_mtf(payload, size);
        } else if (mode == 2) {
            last = detail::decode_mtf(detail::decode_arithmetic(payload, size));
        } else {
            if (payload.size() < 4) invalid();
            const auto rle_size = read_u32(payload, 0);
            // Each RLE pair takes at most two bytes per original MTF code.
            if (rle_size < 2 || rle_size > 2 * size) invalid();
            const auto rle = mode == 3 ? detail::decode_arithmetic(payload.subspan(4), rle_size)
                                       : detail::decode_huffman(payload.subspan(4), rle_size);
            last = detail::decode_rle_mtf(rle, size);
        }
        result = detail::decode_bwt(last, primary);
    }
    if (crc32(result) != checksum) invalid();
    return result;
}
} // namespace bwt
