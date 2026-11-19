#include <archive/archive.hpp>

#include <exception>
#include <iomanip>
#include <sstream>
#include <unistd.h>
#include <iostream>
#include <string_view>
#include <vector>

namespace {
class ProgressDisplay {
    bool dynamic_ = ::isatty(STDERR_FILENO) && ::isatty(STDOUT_FILENO);
    bool visible_ = false;
    std::size_t width_ = 0;
    std::chrono::duration<double> last_{};
public:
    void clear() {
        if (visible_) {
            std::cerr << '\r' << std::string(width_, ' ') << '\r';
            visible_ = false;
        }
    }
    void operator()(const archive::Progress& progress) {
        if (!progress.completed && (!dynamic_ || (visible_ && progress.elapsed - last_ < std::chrono::milliseconds(200)))) return;
        const auto seconds = progress.elapsed.count();
        const auto speed = seconds > 0 ? progress.processed_bytes / seconds / (1024 * 1024) : 0;
        std::ostringstream line;
        line << (progress.completed ? "Statistics: " : "Progress: ")
             << "processed " << progress.processed_bytes << " B";
        if (progress.total_bytes) {
            const auto percent = *progress.total_bytes == 0 ? (progress.completed ? 100.0 : 0.0)
                : 100.0 * static_cast<double>(progress.processed_bytes) / *progress.total_bytes;
            line << " / " << *progress.total_bytes << " B (" << std::fixed << std::setprecision(1) << percent << "%)";
        }
        line << "; time " << std::fixed << std::setprecision(3) << seconds << " s"
             << "; average " << std::setprecision(2) << speed << " MiB/s"
             << "; input " << progress.input_bytes << " B; result ";
        if (progress.result_bytes) line << *progress.result_bytes << " B";
        else line << "unknown";
        if (progress.completed) {
            clear();
            std::cerr << line.str() << '\n';
        } else {
            const auto text = line.str();
            std::cerr << '\r' << text << std::string(width_ > text.size() ? width_ - text.size() : 0, ' ') << std::flush;
            width_ = text.size();
            visible_ = true;
            last_ = progress.elapsed;
        }
    }
};
} // namespace

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
    ProgressDisplay display;
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
