#include <archive/archive.hpp>
#include <cli/progress_display.hpp>
#include <cli/password_input.hpp>
#include <charconv>
#include <optional>

#include <exception>
#include <iostream>
#include <string_view>
#include <vector>

auto main(int argc, char* argv[]) -> signed {
    const auto usage = [](std::ostream& out) {
        out << "Usage:\n"
            << "  compressor compress [--encrypt] [--password-fd N] [--allow-fifo] <archive> <input> [<input> ...]\n"
            << "  compressor decompress [--password-fd N] [--allow-fifo] <archive> <output-directory-or-legacy-file>\n"
            << "  compressor list [--password-fd N] <archive>\n"
            << "  compressor test [--password-fd N] <archive>\n"
            << "  compressor --help\n"
            << "list: show type, original size and path; does NOT decode blocks or verify block CRC; BWA6 archive CRC is verified.\n"
            << "test: fully decode and verify CRC, sizes, paths, structure and totals.\n"
            << "list/test create no files or directories. BWA1 paths are shown as (unnamed).\n"
            << "BWA6 preserves internal links; FIFO creation requires --allow-fifo.\n"
            << "Progress and statistics go to stderr; dynamic progress requires a terminal.\n"
            << "--encrypt writes authenticated BWE1 (Argon2id + XChaCha20-Poly1305).\n"
            << "BWE1 is detected on reading. Password: terminal without echo, or one line from --password-fd N.\n"
            << "Existing output files are never overwritten.\n";
    };
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        usage(std::cout);
        return 0;
    }
    const std::string_view command = argc > 1 ? argv[1] : "";
    archive::Options options;
    std::optional<int> password_fd;
    std::vector<const char*> paths;
    for (int i = 2; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--allow-fifo" && !options.allow_fifo &&
            (command == "compress" || command == "decompress")) {
            options.allow_fifo = true;
        } else if (arg == "--encrypt" && command == "compress" && !options.encrypt) {
            options.encrypt = true;
        } else if (arg == "--password-fd" && !password_fd && i + 1 < argc) {
            std::string_view number(argv[++i]);
            int fd = -1;
            const auto parsed = std::from_chars(number.data(), number.data() + number.size(), fd);
            if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size() || fd < 0) {
                usage(std::cerr);
                return 2;
            }
            password_fd = fd;
        } else if (arg.starts_with("--")) {
            usage(std::cerr);
            return 2;
        } else paths.push_back(argv[i]);
    }
    const bool valid = (command == "compress" && paths.size() >= 2) ||
                       (command == "decompress" && paths.size() == 2) ||
                       ((command == "list" || command == "test") && paths.size() == 1);
    if (!valid || (command == "compress" && password_fd && !options.encrypt)) { usage(std::cerr); return 2; }
    options.password = [password_fd](archive::Password& password, bool creating) {
        cli::read_password(password, creating, password_fd);
    };
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
            }, options);
            return 0;
        }
        if (command == "test") {
            const auto total = archive::test(paths[0], options);
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