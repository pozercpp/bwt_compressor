#include <compression.hpp>
#include "arithmetic.hpp"
#include "huffman.hpp"

#include <algorithm>
#include <ranges>
#include <array>
#include <cstdint>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace bwt {
namespace {
constexpr int alphabet_size = 256;
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

void append_count(Bytes& out, std::size_t count) {
    do {
        unsigned b = static_cast<unsigned>(count & 127u);
        count >>= 7;
        out.push_back(static_cast<std::byte>(b | (count ? 128u : 0u)));
    } while (count);
}

// Counts are canonical unsigned LEB128, at most three bytes (1 MiB).
std::size_t read_count(std::span<const std::byte> data, std::size_t& cursor) {
    std::size_t count = 0;
    for (unsigned shift = 0; shift <= 14; shift += 7) {
        if (cursor == data.size()) invalid();
        unsigned b = value(data[cursor++]);
        count |= static_cast<std::size_t>(b & 127u) << shift;
        if ((b & 128u) == 0) {
            if (count == 0 || (shift != 0 && (b & 127u) == 0)) invalid();
            return count;
        }
    }
    invalid();
}

std::array<unsigned char, alphabet_size> make_alphabet() {
    std::array<unsigned char, alphabet_size> alphabet{};
    std::ranges::iota(alphabet, 0);
    return alphabet;
}

auto build_sa(std::span<const std::byte> s) -> std::vector<int> {
    int n = int(s.size());
    std::vector<int> p(n);
    std::vector<int> c(n), cnt(std::max(alphabet_size, n));
    for (int i = 0; i < n; i++) ++cnt[std::to_integer<unsigned char>(s[i])];
    for (int i = 0; i + 1 < alphabet_size; i++) cnt[i + 1] += cnt[i];
    for (int i = 0; i < n; i++) p[--cnt[std::to_integer<unsigned char>(s[i])]] = i;
    c[p[0]] = 0;
    int classes = 1;
    for (int i = 1; i < n; i++) {
        if (s[p[i]] != s[p[i - 1]]) ++classes;
        c[p[i]] = classes - 1;
    }
    std::vector<int> pn(n), cn(n);
    for (int offset = 1; offset < n; offset *= 2) {
        for (int i = 0; i < n; i++) {
            pn[i] = p[i] - offset;
            if (pn[i] < 0) pn[i] += n;
        }
        std::ranges::fill(std::span(cnt).first(classes), 0);
        for (int i = 0; i < n; i++) ++cnt[c[pn[i]]];
        for (int i = 1; i < classes; i++) cnt[i] += cnt[i - 1];
        for (int i = n - 1; i >= 0; i--) p[--cnt[c[pn[i]]]] = pn[i];
        cn[p[0]] = 0;
        classes = 1;
        for (int i = 1; i < n; i++) {
            int mid1 = p[i] + offset;
            if (mid1 >= n) mid1 -= n;
            int mid2 = p[i - 1] + offset;
            if (mid2 >= n) mid2 -= n;
            if (c[p[i]] != c[p[i - 1]] || c[mid1] != c[mid2]) ++classes;
            cn[p[i]] = classes - 1;
        }
        c.swap(cn);
        if (classes == n) break;
    }
    return p;
}

std::pair<Bytes, std::uint32_t> encode_bwt(std::span<const std::byte> input) {
    auto order = build_sa(input);
    Bytes last(input.size());
    std::uint32_t primary = 0;
    for (std::size_t i = 0; i < order.size(); ++i) {
        if (order[i] == 0) primary = static_cast<std::uint32_t>(i);
        const auto previous = order[i] == 0 ? input.size() - 1
                                           : static_cast<std::size_t>(order[i] - 1);
        last[i] = input[previous];
    }
    return {std::move(last), primary};
}

Bytes decode_bwt(std::span<const std::byte> last, std::size_t primary) {
    std::array<std::size_t, alphabet_size> counts{};
    for (auto b : last) ++counts[value(b)];
    std::exclusive_scan(counts.begin(), counts.end(), counts.begin(), std::size_t{0});
    std::vector<std::size_t> next(last.size());
    for (std::size_t i = 0; i < last.size(); ++i) next[i] = counts[value(last[i])]++;
    Bytes result(last.size());
    for (std::size_t i = result.size(); i > 0; --i) {
        result[i - 1] = last[primary];
        primary = next[primary];
    }
    return result;
}

Bytes encode_mtf(std::span<const std::byte> last) {
    auto alphabet = make_alphabet();
    Bytes result;
    result.reserve(last.size());
    for (auto b : last) {
        auto pos = std::ranges::find(alphabet, value(b));
        result.push_back(static_cast<std::byte>(pos - alphabet.begin()));
        std::ranges::rotate(std::ranges::subrange(alphabet.begin(), pos + 1), pos);
    }
    return result;
}

Bytes decode_mtf(std::span<const std::byte> codes) {
    auto alphabet = make_alphabet();
    Bytes result;
    result.reserve(codes.size());
    for (auto b : codes) {
        auto pos = alphabet.begin() + value(b);
        result.push_back(static_cast<std::byte>(*pos));
        std::ranges::rotate(std::ranges::subrange(alphabet.begin(), pos + 1), pos);
    }
    return result;
}

// MTF codes are bytes; consecutive equal codes become (count, code) pairs.
Bytes encode_rle(std::span<const std::byte> codes) {
    Bytes result;
    std::size_t count = 0;
    std::byte previous{};
    for (auto code : codes) {
        if (count && code != previous) {
            append_count(result, count);
            result.push_back(previous);
            count = 0;
        }
        previous = code;
        ++count;
    }
    if (count) {
        append_count(result, count);
        result.push_back(previous);
    }
    return result;
}

Bytes decode_rle_mtf(std::span<const std::byte> payload, std::size_t size) {
    auto alphabet = make_alphabet();
    Bytes last;
    last.reserve(size);
    std::size_t cursor = 0;
    while (cursor < payload.size()) {
        const auto count = read_count(payload, cursor);
        if (count > size - last.size() || cursor == payload.size()) invalid();
        const auto code = value(payload[cursor++]);
        for (std::size_t i = 0; i < count; ++i) {
            last.push_back(static_cast<std::byte>(alphabet[code]));
            auto pos = alphabet.begin() + code;
            std::ranges::rotate(std::ranges::subrange(alphabet.begin(), pos + 1), pos);
        }
    }
    if (last.size() != size) invalid();
    return last;
}
} // namespace

std::vector<std::byte> compress(std::span<const std::byte> input) {
    if (input.size() > max_block_size) {
        throw std::length_error("Input block exceeds the 1 MiB limit.");
    }
    Bytes payload;
    std::uint32_t primary = 0;
    unsigned mode = 0;
    if (!input.empty()) {
        auto [last, index] = encode_bwt(input);
        const auto codes = encode_mtf(last);
        const auto rle = encode_rle(codes);
        auto entropy = detail::encode_arithmetic(rle);
        auto huffman = detail::encode_huffman(rle);
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
            last = decode_rle_mtf(payload, size);
        } else if (mode == 2) {
            last = decode_mtf(detail::decode_arithmetic(payload, size));
        } else {
            if (payload.size() < 4) invalid();
            const auto rle_size = read_u32(payload, 0);
            // Each RLE pair takes at most two bytes per original MTF code.
            if (rle_size < 2 || rle_size > 2 * size) invalid();
            const auto rle = mode == 3 ? detail::decode_arithmetic(payload.subspan(4), rle_size)
                                       : detail::decode_huffman(payload.subspan(4), rle_size);
            last = decode_rle_mtf(rle, size);
        }
        result = decode_bwt(last, primary);
    }
    if (crc32(result) != checksum) invalid();
    return result;
}
} // namespace bwt
