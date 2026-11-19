#include <archive.hpp>

#include <exception>
#include <iostream>
#include <string_view>

int main(int argc, char* argv[]) {
    const auto usage = [](std::ostream& out) {
        out << "Usage:\n"
            << "  compressor compress <input-file> <archive>\n"
            << "  compressor decompress <archive> <output-file>\n"
            << "  compressor --help\n"
            << "Existing output files are never overwritten.\n";
    };

    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        usage(std::cout);
        return 0;
    }
    if (argc != 4 || (std::string_view(argv[1]) != "compress" && std::string_view(argv[1]) != "decompress")) {
        usage(std::cerr);
        return 2;
    }
    try {
        const bool compressing = std::string_view(argv[1]) == "compress";
        const auto total = compressing ? archive::compress_file(argv[2], argv[3])
                                      : archive::decompress_file(argv[2], argv[3]);
        std::cout << (compressing ? "Compressed " : "Decompressed ") << total << " bytes into " << argv[3] << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
