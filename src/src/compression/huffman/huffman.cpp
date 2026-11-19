#include <compression/huffman/huffman.hpp>
#include <compression/compression.hpp>

#include <algorithm>
#include <ranges>
#include <array>
#include <functional>
#include <queue>
#include <stdexcept>
#include <utility>

namespace bwt::detail {
namespace {
using Bytes = std::vector<std::byte>;
using Lengths = std::array<unsigned, 256>;
using Codes = std::array<std::vector<bool>, 256>;
constexpr std::size_t max_input = 2 * max_block_size;

template<typename T>
using min_pq = std::priority_queue<T, std::vector<T>, std::greater<T>>;

[[noreturn]] void invalid() { throw std::invalid_argument("Invalid Huffman payload."); }

Lengths build_lengths(std::span<const std::byte> input) {
    std::array<std::size_t, 256> frequencies{};
    for (auto b : input) ++frequencies[std::to_integer<unsigned>(b)];
    struct Node { int left = -1, right = -1, symbol = -1; };
    std::vector<Node> nodes;
    using Item = std::pair<std::size_t, int>;
    min_pq<Item> queue;
    for (unsigned symbol = 0; symbol < 256; ++symbol) {
        if (frequencies[symbol]) {
            queue.emplace(frequencies[symbol], static_cast<int>(nodes.size()));
            nodes.push_back({-1, -1, static_cast<int>(symbol)});
        }
    }
    Lengths lengths{};
    if (queue.empty()) return lengths;
    while (queue.size() > 1) {
        const auto [a_weight, a] = queue.top(); queue.pop();
        const auto [b_weight, b] = queue.top(); queue.pop();
        queue.emplace(a_weight + b_weight, static_cast<int>(nodes.size()));
        nodes.push_back({a, b, -1});
    }
    auto visit = [&](auto&& self, int index, unsigned depth) -> void {
        const auto& node = nodes[index];
        if (node.symbol >= 0) lengths[node.symbol] = std::max(1u, depth);
        else {
            self(self, node.left, depth + 1);
            self(self, node.right, depth + 1);
        }
    };
    visit(visit, queue.top().second, 0);
    return lengths;
}

Codes canonical_codes(const Lengths& lengths) {
    std::vector<std::pair<unsigned, unsigned>> order;
    std::array<unsigned, 256> counts{};
    for (unsigned symbol = 0; symbol < 256; ++symbol) {
        if (lengths[symbol]) {
            if (lengths[symbol] > 255) invalid();
            order.emplace_back(lengths[symbol], symbol);
            ++counts[lengths[symbol]];
        }
    }
    if (order.size() == 1) {
        if (order[0].first != 1) invalid();
    } else if (!order.empty()) {
        // A complete binary tree has at most 256 leaves. Cap open slots
        // before doubling, avoiding overflow on malicious length tables.
        unsigned slots = 1;
        for (unsigned length = 1; length <= 255; ++length) {
            slots *= 2;
            if (counts[length] > slots) invalid();
            slots -= counts[length];
            if (slots > 256) invalid();
        }
        if (slots != 0) invalid();
    }
    std::ranges::sort(order);
    Codes codes;
    std::vector<bool> current;
    for (std::size_t i = 0; i < order.size(); ++i) {
        if (i) {
            std::size_t bit = current.size();
            while (bit && current[bit - 1]) current[--bit] = false;
            if (!bit) invalid();
            current[bit - 1] = true;
        }
        current.resize(order[i].first, false);
        codes[order[i].second] = current;
    }
    return codes;
}
} // namespace

std::vector<std::byte> encode_huffman(std::span<const std::byte> input) {
    if (input.size() > max_input) throw std::length_error("Huffman block too large.");
    const auto lengths = build_lengths(input);
    const auto codes = canonical_codes(lengths);
    Bytes result;
    for (auto length : lengths) result.push_back(static_cast<std::byte>(length));
    unsigned used = 0;
    for (auto b : input) {
        for (bool bit : codes[std::to_integer<unsigned>(b)]) {
            if (used == 0) result.push_back(std::byte{0});
            if (bit) result.back() |= static_cast<std::byte>(1u << (7 - used));
            used = (used + 1) % 8;
        }
    }
    return result;
}

std::vector<std::byte> decode_huffman(std::span<const std::byte> input, std::size_t size) {
    if (size > max_input || input.size() < 256 || input.size() > 256 + 32 * max_input) invalid();
    Lengths lengths{};
    for (unsigned i = 0; i < 256; ++i) lengths[i] = std::to_integer<unsigned>(input[i]);
    const auto codes = canonical_codes(lengths);
    const auto symbols = std::ranges::count_if(lengths, [](auto n) { return n != 0; });
    if ((size == 0) != (symbols == 0)) invalid();
    struct Node { std::array<int, 2> child{-1, -1}; int symbol = -1; };
    std::vector<Node> tree(1);
    for (unsigned symbol = 0; symbol < 256; ++symbol) {
        if (codes[symbol].empty()) continue;
        int node = 0;
        for (bool bit : codes[symbol]) {
            if (tree[node].child[bit] < 0) {
                const auto next = static_cast<int>(tree.size());
                tree[node].child[bit] = next;
                tree.emplace_back();
            }
            node = tree[node].child[bit];
        }
        tree[node].symbol = static_cast<int>(symbol);
    }
    Bytes output;
    output.reserve(size);
    std::size_t position = 256 * 8;
    for (std::size_t i = 0; i < size; ++i) {
        int node = 0;
        while (tree[node].symbol < 0) {
            if (position / 8 == input.size()) invalid();
            const auto byte = std::to_integer<unsigned>(input[position / 8]);
            const auto bit = (byte >> (7 - position % 8)) & 1u;
            ++position;
            node = tree[node].child[bit];
            if (node < 0) invalid();
        }
        output.push_back(static_cast<std::byte>(tree[node].symbol));
    }
    if ((position + 7) / 8 != input.size()) invalid();
    if (position % 8) {
        const auto mask = (1u << (8 - position % 8)) - 1;
        if ((std::to_integer<unsigned>(input.back()) & mask) != 0) invalid();
    }
    return output;
}
} // namespace bwt::detail
