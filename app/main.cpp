#include <archive/archive.hpp>
#include <cli/progress_display.hpp>

#include <exception>
#include <iostream>
#include <string_view>
#include <vector>

auto main(int argc, char* argv[]) -> signed {
    const auto usage = [](std::ostream& out) {
        out << "Usage:\n"
            << "  compressor compress [--allow-fifo] <archive> <input> [<input> ...]\n"
            << "  compressor decompress [--allow-fifo] <archive> <output-directory-or-legacy-file>\n"
            << "  compressor list <archive>\n"
            << "  compressor test <archive>\n"
            << "  compressor --help\n"
            << "list: show type, original size and path; does NOT verify contents or CRC.\n"
            << "test: fully decode and verify CRC, sizes, paths, structure and totals.\n"
            << "list/test create no files or directories. BWA1 paths are shown as (unnamed).\n"
            << "BWA4 preserves internal links; FIFO creation requires --allow-fifo.\n"
            << "Progress and statistics go to stderr; dynamic progress requires a terminal.\n"
            << "Existing output files are never overwritten.\n";
    };

    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        usage(std::cout);
        return 0;
    }
    const std::string_view command = argc > 1 ? argv[1] : "";
    archive::Options options;
    std::vector<const char*> paths;
    for (int i = 2; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--allow-fifo" && !options.allow_fifo &&
            (command == "compress" || command == "decompress")) {
            options.allow_fifo = true;
        } else if (arg.starts_with("--")) {
            usage(std::cerr);
            return 2;
        } else paths.push_back(argv[i]);
    }
    const bool valid = (command == "compress" && paths.size() >= 2) ||
                       (command == "decompress" && paths.size() == 2) ||
                       ((command == "list" || command == "test") && paths.size() == 1);
    if (!valid) { usage(std::cerr); return 2; }
    cli::ProgressDisplay display;
    options.progress = [&](const archive::Progress& progress) { display(progress); };
    try {
        if (command == "list") {
            (void)archive::list(paths[0], [](const archive::EntryInfo& entry) {
                const char* kind = "file";
                switch (entry.type) {
                    case archive::EntryType::directory: kind = "directory"; break;
                    case archive::EntryType::symlink: kind = "symlink"; break;
                    case archive::EntryType::hardlink: kind = "hardlink"; break;
                    case archive::EntryType::fifo: kind = "fifo"; break;
                    case archive::EntryType::file: break;
                }
                std::cout << kind << '\t'
                          << entry.size << '\t'
                          << (entry.path.empty() ? "(unnamed)" : entry.path);
                if (!entry.target.empty()) std::cout << '\t' << entry.target;
                std::cout << '\n';
            });
            return 0;
        }
        if (command == "test") {
            const auto total = archive::test(paths[0]);
            std::cout << "Verified " << total << " bytes.\n";
            return 0;
        }
        const bool compressing = std::string_view(argv[1]) == "compress";
        std::uint64_t total;
        if (compressing) {
            std::vector<std::filesystem::path> inputs;
            for (std::size_t i = 1; i < paths.size(); ++i) inputs.emplace_back(paths[i]);
            total = archive::create(inputs, paths[0], options);
        } else {
            total = archive::extract(paths[0], paths[1], options);
        }
        std::cout << (compressing ? "Compressed " : "Decompressed ") << total << " bytes into " << (compressing ? paths[0] : paths[1]) << '\n';
        return 0;
    } catch (const std::exception& error) {
        display.clear();
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
