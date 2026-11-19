#include <archive/archive.hpp>

#include <exception>
#include <iostream>
#include <string_view>
#include <vector>

auto main(int argc, char* argv[]) -> signed {
    const auto usage = [](std::ostream& out) {
        out << "Usage:\n"
            << "  compressor compress <archive> <input> [<input> ...]\n"
            << "  compressor decompress <archive> <output-directory-or-legacy-file>\n"
            << "  compressor --help\n"
            << "Existing output files are never overwritten.\n";
    };

    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        usage(std::cout);
        return 0;
    }
    if (argc < 4 || (std::string_view(argv[1]) != "compress" && std::string_view(argv[1]) != "decompress") ||
        (std::string_view(argv[1]) == "decompress" && argc != 4)) {
        usage(std::cerr);
        return 2;
    }
    for (int i = 2; i < argc; ++i) {
        if (std::string_view(argv[i]).starts_with("--")) {
            usage(std::cerr);
            return 2;
        }
    }
    try {
        const bool compressing = std::string_view(argv[1]) == "compress";
        std::uint64_t total;
        if (compressing) {
            std::vector<std::filesystem::path> inputs;
            for (int i = 3; i < argc; ++i) inputs.emplace_back(argv[i]);
            total = archive::create(inputs, argv[2]);
        } else {
            total = archive::extract(argv[2], argv[3]);
        }
        std::cout << (compressing ? "Compressed " : "Decompressed ") << total << " bytes into " << (compressing ? argv[2] : argv[3]) << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
