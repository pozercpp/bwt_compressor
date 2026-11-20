// Deterministic validation of saved seeds/findings, without a fuzzing runtime.
#include <archive/archive.hpp>
#include <compression/compression.hpp>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string_view>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    try {
        if (std::string_view(argv[1]) == "bwc1") {
            std::ifstream input(argv[2], std::ios::binary);
            if (!input) return 2;
            std::vector<char> bytes{std::istreambuf_iterator<char>(input), {}};
            (void)bwt::decompress(std::as_bytes(std::span(bytes)));
        } else if (std::string_view(argv[1]) == "bwa1" || std::string_view(argv[1]) == "bwa2") {
            archive::Options options;
            options.threads = 1;
            (void)archive::test(argv[2], options);
        } else { return 2; }
        return 0;
    } catch (const std::invalid_argument&) { return 1; }
      catch (const std::runtime_error&) { return 1; }
}
