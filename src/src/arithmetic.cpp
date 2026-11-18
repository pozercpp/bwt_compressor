#include "arithmetic.hpp"
#include <compression.hpp>

#include <algorithm>
#include <ranges>
#include <array>
#include <cstdint>
#include <numeric>
#include <stdexcept>

namespace bwt::detail {
namespace {
using Bytes = std::vector<std::byte>;
constexpr std::uint64_t full = std::uint64_t{1} << 32;
constexpr auto half = full / 2;
constexpr auto quarter = half / 2;

[[noreturn]] void invalid() {
    throw std::invalid_argument("Invalid arithmetic payload.");
}

// Cumulative frequencies: initially one per byte, updated after each symbol.
struct Model {
    std::array<std::uint32_t, 257> cumulative{};
    Model() { std::ranges::iota(cumulative, 0u); }
    void update(unsigned symbol) {
        for (unsigned i = symbol + 1; i <= 256; ++i) ++cumulative[i];
        if (cumulative.back() == 16384) {
            std::uint32_t previous = 0, sum = 0;
            for (unsigned i = 1; i <= 256; ++i) {
                const auto frequency = cumulative[i] - previous;
                previous = cumulative[i];
                sum += (frequency + 1) / 2;
                cumulative[i] = sum;
            }
        }
    }
};

struct BitWriter {
    Bytes bytes;
    unsigned used = 0;
    void put(unsigned bit) {
        if (used == 0) bytes.push_back(std::byte{0});
        bytes.back() |= static_cast<std::byte>(bit << (7 - used));
        used = (used + 1) % 8;
    }
};

struct BitReader {
    std::span<const std::byte> bytes;
    std::size_t position = 0;
    unsigned get() {
        if (position / 8 == bytes.size()) invalid();
        const auto byte = std::to_integer<unsigned>(bytes[position / 8]);
        const auto bit = (byte >> (7 - position % 8)) & 1u;
        ++position;
        return bit;
    }
};
} // namespace

std::vector<std::byte> encode_arithmetic(std::span<const std::byte> input) {
    if (input.size() > 2 * max_block_size) throw std::length_error("Arithmetic block too large.");
    Model model;
    BitWriter writer;
    std::uint64_t low = 0, high = full - 1;
    std::size_t pending = 0;
    auto emit = [&](unsigned bit) {
        writer.put(bit);
        while (pending) {
            writer.put(bit ^ 1u);
            --pending;
        }
    };
    for (auto byte : input) {
        const auto symbol = std::to_integer<unsigned>(byte);
        const auto range = high - low + 1;
        high = low + range * model.cumulative[symbol + 1] / model.cumulative.back() - 1;
        low += range * model.cumulative[symbol] / model.cumulative.back();
        for (;;) {
            if (high < half) {
                emit(0);
            } else if (low >= half) {
                emit(1);
                low -= half;
                high -= half;
            } else if (low >= quarter && high < 3 * quarter) {
                ++pending;
                low -= quarter;
                high -= quarter;
            } else {
                break;
            }
            low *= 2;
            high = high * 2 + 1;
        }
        model.update(symbol);
    }
    ++pending;
    emit(low < quarter ? 0 : 1);
    // Explicit lookahead avoids treating truncated streams as implicit zeros.
    for (unsigned i = 0; i < 32; ++i) writer.put(0);
    return writer.bytes;
}

std::vector<std::byte> decode_arithmetic(std::span<const std::byte> input, std::size_t size) {
    if (size > 2 * max_block_size || input.size() > 4 * max_block_size + 8) invalid();
    Model model;
    BitReader reader{input};
    std::uint64_t low = 0, high = full - 1, code = 0;
    for (unsigned i = 0; i < 32; ++i) code = code * 2 + reader.get();
    Bytes output;
    output.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
        if (code < low || code > high) invalid();
        const auto range = high - low + 1;
        const auto scaled = ((code - low + 1) * model.cumulative.back() - 1) / range;
        const auto it = std::ranges::upper_bound(model.cumulative, scaled);
        if (it == model.cumulative.begin() || it == model.cumulative.end()) invalid();
        const auto symbol = static_cast<unsigned>(it - model.cumulative.begin() - 1);
        high = low + range * model.cumulative[symbol + 1] / model.cumulative.back() - 1;
        low += range * model.cumulative[symbol] / model.cumulative.back();
        for (;;) {
            if (high < half) {
                // No offset adjustment needed.
            } else if (low >= half) {
                low -= half;
                high -= half;
                code -= half;
            } else if (low >= quarter && high < 3 * quarter) {
                low -= quarter;
                high -= quarter;
                code -= quarter;
            } else {
                break;
            }
            low *= 2;
            high = high * 2 + 1;
            code = code * 2 + reader.get();
        }
        output.push_back(static_cast<std::byte>(symbol));
        model.update(symbol);
    }
    // Require the canonical termination and padding, and reject trailing data.
    const auto canonical = encode_arithmetic(output);
    if (!std::ranges::equal(canonical, input)) invalid();
    return output;
}
} // namespace bwt::detail
