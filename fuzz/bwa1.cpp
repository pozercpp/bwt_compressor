#include "common.hpp"
#include <archive/archive_internal.hpp>
#include <sstream>
#include <streambuf>

namespace {
class Discard : public std::streambuf {
    std::streamsize xsputn(const char*, std::streamsize count) override { return count; }
    int_type overflow(int_type value) override { return traits_type::not_eof(value); }
};
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > fuzz::max_archive_input) return 0;
    const std::string bytes(reinterpret_cast<const char*>(data), size);
    fuzz::accepted([&] {
        std::istringstream input(bytes);
        (void)archive::detail::list_stream(input, true);
    });
    fuzz::accepted([&] {
        std::istringstream input(bytes);
        Discard buffer;
        std::ostream output(&buffer);
        (void)archive::detail::decompress_stream(input, output, 1, true, fuzz::max_output);
    });
    return 0;
}
