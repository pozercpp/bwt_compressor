// Test-only adapter for checking backend worker counts without CLI options.
#include <archive.hpp>

#include <exception>
#include <iostream>
#include <string>
#include <string_view>

int main(int argc, char* argv[]) {
    if (argc != 5) return 2;
    try {
        const archive::Options options{.threads = static_cast<unsigned>(std::stoul(argv[4]))};
        if (std::string_view(argv[1]) == "compress") {
            (void)archive::compress_file(argv[2], argv[3], options);
        } else if (std::string_view(argv[1]) == "decompress") {
            (void)archive::decompress_file(argv[2], argv[3], options);
        } else {
            return 2;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
