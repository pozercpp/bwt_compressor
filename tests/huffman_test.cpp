#include <compression/compression.hpp>
#include <compression/huffman/huffman.hpp>

#include <algorithm>
#include <ranges>
#include <iostream>
#include <random>
#include <source_location>
#include <stdexcept>
#include <string>

using Bytes = std::vector<std::byte>;
void require(bool condition, std::source_location loc = std::source_location::current()) {
    if (!condition) throw std::runtime_error("Huffman test failed at " + std::to_string(loc.line()));
}
void round_trip(const Bytes& data) {
    const auto encoded = bwt::detail::encode_huffman(data);
    require(bwt::detail::decode_huffman(encoded, data.size()) == data);
}
void rejects(const Bytes& encoded, std::size_t size) {
    try { (void)bwt::detail::decode_huffman(encoded, size); }
    catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("Invalid Huffman stream accepted");
}
int main() {
    round_trip({});
    for (unsigned i = 0; i < 256; ++i) round_trip(Bytes(17, static_cast<std::byte>(i)));
    // Known canonical codes: all symbols equally frequent => 8-bit code = symbol.
    Bytes all;
    for (unsigned i = 0; i < 256; ++i) all.push_back(static_cast<std::byte>(i));
    Bytes golden(256, std::byte{8});
    golden.insert(golden.end(), all.begin(), all.end());
    require(bwt::detail::encode_huffman(all) == golden);
    require(bwt::detail::decode_huffman(golden, 256) == all);
    // Independent two-symbol table: A=0, B=1; ABBA => 0110xxxx.
    Bytes abba(256);
    abba[65] = abba[66] = std::byte{1};
    abba.push_back(std::byte{0x60});
    require(bwt::detail::decode_huffman(abba, 4) == Bytes({std::byte{65},std::byte{66},std::byte{66},std::byte{65}}));
    auto damaged = abba;
    damaged.back() |= std::byte{1};
    rejects(damaged, 4); // nonzero padding
    damaged = abba; damaged.push_back(std::byte{0}); rejects(damaged, 4);
    for (std::size_t cut = 0; cut < abba.size(); ++cut) {
        rejects(Bytes(abba.begin(), abba.begin() + cut), 4);
    }
    Bytes table(256);
    rejects(table, 1); // empty alphabet
    table[0] = std::byte{2}; rejects(table, 1); // singleton must have length 1
    table[0] = table[1] = table[2] = std::byte{1}; rejects(table, 1); // oversubscribed
    table.assign(256, std::byte{0});
    table[0] = table[1] = std::byte{2}; rejects(table, 1); // incomplete
    table[0] = table[1] = std::byte{255}; rejects(table, 1); // pathological incomplete
    table.assign(256, std::byte{0});
    table[0] = std::byte{1}; table.push_back(std::byte{128}); rejects(table, 1); // invalid branch
    rejects(abba, 0);
    rejects(abba, 2 * bwt::max_block_size + 1);

    // A valid complete tree with maximum-length 255-bit codes.
    // The last symbol's canonical code is 255 one bits.
    Bytes deep;
    for (unsigned depth = 1; depth <= 254; ++depth) deep.push_back(static_cast<std::byte>(depth));
    deep.push_back(std::byte{255}); deep.push_back(std::byte{255});
    deep.insert(deep.end(), 31, std::byte{255});
    deep.push_back(std::byte{254});
    require(bwt::detail::decode_huffman(deep, 1) == Bytes{std::byte{255}});

    std::mt19937 random(123);
    for (unsigned alphabet : {2u, 8u, 64u, 256u}) {
        for (unsigned n : {7u, 100u, 70000u}) {
            Bytes input(n);
            std::ranges::generate(input, [&] { return static_cast<std::byte>(random() % alphabet); });
            round_trip(input);
        }
    }
    Bytes maximum(2 * bwt::max_block_size);
    std::ranges::generate(maximum, [&] { return static_cast<std::byte>(random() % 256); });
    round_trip(maximum);
    bool too_large = false;
    try { (void)bwt::detail::encode_huffman(Bytes(2 * bwt::max_block_size + 1)); }
    catch (const std::length_error&) { too_large = true; }
    require(too_large);

    // Public API must actually select mode 4 and restore the whole chain.
    std::mt19937 sample_random(123);
    Bytes sample(16384);
    std::ranges::generate(sample, [&] { return static_cast<std::byte>(sample_random() % 8); });
    const auto block = bwt::compress(sample);
    require(block[4] == std::byte{4});
    require(bwt::decompress(block) == sample);
    // Corrupted Huffman table and content must be rejected by the full decoder.
    for (std::size_t index : {std::size_t{21}, block.size() - 1}) {
        auto broken = block;
        broken[index] ^= std::byte{1};
        bool rejected = false;
        try { (void)bwt::decompress(broken); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected);
    }
    std::cout << "Huffman tests passed; mode 4 sample: " << sample.size() << " -> " << block.size() << " bytes\n";
}
