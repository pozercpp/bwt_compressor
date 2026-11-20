#include "common.hpp"
#include <compression/compression.hpp>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > bwt::max_block_size + 17) return 0;
    fuzz::accepted([&] {
        const auto decoded = bwt::decompress(std::as_bytes(std::span(data, size)));
        if (decoded.size() > bwt::max_block_size) std::abort();
    });
    return 0;
}