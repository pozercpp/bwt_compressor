#include <compression/compression.hpp>
#include <compression/arithmetic/arithmetic.hpp>
#include <source_location>
#include <string>

#include <algorithm>
#include <ranges>
#include <array>
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string_view>

using Bytes = std::vector<std::byte>;

void require(bool condition, std::source_location location = std::source_location::current()) {
    if (!condition) throw std::runtime_error("Test failed at line " + std::to_string(location.line()));
}

void round_trip(std::span<const std::byte> input) {
    const auto packed = bwt::compress(input);
    const auto restored = bwt::decompress(packed);
    require(std::ranges::equal(input, restored));
    require(packed.size() <= input.size() + 17);
}

void rejects(std::span<const std::byte> data) {
    try {
        (void)bwt::decompress(data);
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error("Invalid block accepted");
}

int main() {
    round_trip({});
    const std::string_view text = "Hello, мир! $ text with spaces\n";
    round_trip(std::as_bytes(std::span(text.data(), text.size())));
    for (unsigned i = 0; i < 256; ++i) round_trip(Bytes{static_cast<std::byte>(i)});
    Bytes all_bytes;
    for (unsigned i = 0; i < 256; ++i) all_bytes.push_back(static_cast<std::byte>(i));
    round_trip(all_bytes);
    // Repeated full alphabet exercises the compressed path with every byte.
    Bytes repeated;
    for (int i = 0; i < 32; ++i) repeated.insert(repeated.end(), all_bytes.begin(), all_bytes.end());
    require(bwt::compress(repeated)[4] != std::byte{0});
    round_trip(repeated);
    for (std::size_t length = 1; length < 200; ++length) {
        Bytes periodic(length);
        for (std::size_t i = 0; i < length; ++i) periodic[i] = static_cast<std::byte>(i % 3);
        round_trip(periodic);
    }
    // Exercise every short binary sequence, including tied cyclic rotations.
    for (unsigned length = 0; length <= 10; ++length) {
        for (unsigned bits = 0; bits < (1u << length); ++bits) {
            Bytes input(length);
            for (unsigned i = 0; i < length; ++i) input[i] = static_cast<std::byte>((bits >> i) & 1u);
            round_trip(input);
        }
    }
    std::mt19937 random(42);
    for (int test = 0; test < 100; ++test) {
        Bytes input(random() % 4096);
        std::ranges::generate(input, [&] { return static_cast<std::byte>(random() % 256); });
        round_trip(input);
    }
    round_trip(Bytes(bwt::max_block_size, std::byte{0}));
    Bytes large_random(bwt::max_block_size);
    std::ranges::generate(large_random, [&] { return static_cast<std::byte>(random() % 256); });
    round_trip(large_random);
    require(bwt::compress(large_random) == bwt::compress(large_random, {.parallel_entropy = false}));
    bool too_large = false;
    try {
        (void)bwt::compress(Bytes(bwt::max_block_size + 1));
    } catch (const std::length_error&) {
        too_large = true;
    }
    require(too_large);

    const auto valid = bwt::compress(Bytes(300, std::byte{42}));
    require(valid[4] == std::byte{3});
    for (std::size_t i = 0; i < valid.size(); ++i) {
        rejects(std::span(valid).first(i));
        for (unsigned bit = 0; bit < 8; ++bit) {
            auto damaged = valid;
            damaged[i] ^= static_cast<std::byte>(1u << bit);
            if (i >= 9 && i < 13) {
                // Equal cyclic rotations can have different valid primary
                // indices. A changed index may still decode identical data.
                try {
                    require(bwt::decompress(damaged) == Bytes(300, std::byte{42}));
                } catch (const std::invalid_argument&) {
                }
            } else {
                rejects(damaged);
            }
        }
    }
    auto extra = valid;
    extra.push_back(std::byte{0});
    rejects(extra);
    auto invalid_count = valid;
    invalid_count[17] = std::byte{0};
    rejects(invalid_count);
    invalid_count.resize(21);
    std::ranges::fill(std::span(invalid_count).subspan(17), std::byte{255});
    rejects(invalid_count);
    auto oversized = valid;
    std::ranges::fill(std::span(oversized).subspan(5, 4), std::byte{255});
    rejects(oversized);
    auto bad_primary = valid;
    std::ranges::fill(std::span(bad_primary).subspan(9, 4), std::byte{255});
    rejects(bad_primary);
    const auto raw = bwt::compress(all_bytes);
    require(raw[4] == std::byte{0});
    auto corrupt_raw = raw;
    corrupt_raw.back() ^= std::byte{1};
    rejects(corrupt_raw);
    // Arbitrary data must not cause out-of-bounds access or unbounded allocation.
    for (int test = 0; test < 1000; ++test) {
        Bytes junk(random() % 100);
        std::ranges::generate(junk, [&] { return static_cast<std::byte>(random() % 256); });
        rejects(junk);
    }
    // A saved BWC1 mode-1 block produced before arithmetic support existed.
    const std::string_view legacy_hex = "42574331012c01000058000000183dc3b3012aab0200";
    Bytes legacy;
    for (std::size_t i = 0; i < legacy_hex.size(); i += 2) {
        legacy.push_back(static_cast<std::byte>(std::stoul(std::string(legacy_hex.substr(i, 2)), nullptr, 16)));
    }
    require(bwt::decompress(legacy) == Bytes(300, std::byte{42}));

    const std::string_view legacy2_hex = "4257433102000100001e0000007fe58530020202194546cda5d90bda30a77e835b29832d659fc96aa10b1e1cec2166d20b280ea14d9a235bd2a61fcde59a5197f6f61cfe448a2af4a763d1322f0c3ef0608406c4476332dd2f9af252cf74155a77463cef2c0d90c221b88325cc68bd6f10387aa9670f9af20f2a883759422a0d2b38c96b7da90e139ffaef7022f27200000000";
    Bytes legacy2, expected2;
    for (std::size_t i = 0; i < legacy2_hex.size(); i += 2) {
        legacy2.push_back(static_cast<std::byte>(std::stoul(std::string(legacy2_hex.substr(i, 2)), nullptr, 16)));
    }
    std::uint32_t state = 1;
    for (unsigned i = 0; i < 256; ++i) {
        state = state * 1664525u + 1013904223u;
        expected2.push_back(static_cast<std::byte>((state >> 24) % 4));
    }
    require(bwt::decompress(legacy2) == expected2);

    // Exercise the arithmetic codec directly, including frequency rescaling,
    // even for data where the public API would prefer raw storage or RLE.
    for (unsigned alphabet : {1u, 2u, 17u, 256u}) {
        Bytes source(70000);
        std::ranges::generate(source, [&] { return static_cast<std::byte>(random() % alphabet); });
        const auto encoded = bwt::detail::encode_arithmetic(source);
        require(bwt::detail::decode_arithmetic(encoded, source.size()) == source);
    }
    // Worst-case RLE byte count is twice the original block size.
    Bytes expanded(2 * bwt::max_block_size);
    for (std::size_t i = 0; i < expanded.size(); ++i) expanded[i] = static_cast<std::byte>(i % 2);
    const auto expanded_encoded = bwt::detail::encode_arithmetic(expanded);
    require(bwt::detail::decode_arithmetic(expanded_encoded, expanded.size()) == expanded);
    require(bwt::detail::decode_arithmetic(bwt::detail::encode_arithmetic({}), 0).empty());
    Bytes biased(65536);
    std::ranges::generate(biased, [&] { return static_cast<std::byte>(random() % 8); });
    const auto arithmetic_block = bwt::compress(biased);
    require(arithmetic_block[4] == std::byte{3} || arithmetic_block[4] == std::byte{4});
    require(bwt::decompress(arithmetic_block) == biased);
    for (std::size_t cut : {std::size_t{17}, arithmetic_block.size() / 2, arithmetic_block.size() - 1}) {
        rejects(std::span(arithmetic_block).first(cut));
    }
    for (std::size_t i : {std::size_t{17}, arithmetic_block.size() / 2, arithmetic_block.size() - 1}) {
        auto damaged = arithmetic_block;
        damaged[i] ^= std::byte{1};
        rejects(damaged);
    }
    auto invalid_rle_size = arithmetic_block;
    std::ranges::fill(std::span(invalid_rle_size).subspan(17, 4), std::byte{255});
    rejects(invalid_rle_size);
    std::ranges::fill(std::span(invalid_rle_size).subspan(17, 4), std::byte{0});
    rejects(invalid_rle_size);
    // Valid arithmetic stream containing invalid RLE (zero run length).
    auto invalid_rle = arithmetic_block;
    invalid_rle.resize(17);
    invalid_rle[4] = std::byte{3};
    invalid_rle.insert(invalid_rle.end(), {std::byte{2}, std::byte{0}, std::byte{0}, std::byte{0}});
    const auto bad_rle_payload = bwt::detail::encode_arithmetic(Bytes{std::byte{0}, std::byte{0}});
    invalid_rle.insert(invalid_rle.end(), bad_rle_payload.begin(), bad_rle_payload.end());
    rejects(invalid_rle);
    auto trailing = arithmetic_block;
    trailing.push_back(std::byte{0});
    rejects(trailing);
    auto unknown = arithmetic_block;
    unknown[4] = std::byte{5};
    rejects(unknown);
    std::cout << "Arithmetic sample: " << biased.size() << " -> " << arithmetic_block.size() << " bytes\n";
    std::cout << "All BWT library tests passed\n";
}
