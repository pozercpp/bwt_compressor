#include <archive/archive.hpp>
#include <archive/archive_internal.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

// Atomic no-replace publication and a private staging directory on Linux.
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace archive {
namespace {
namespace fs = std::filesystem;
using namespace detail;
constexpr std::size_t max_path_bytes = 4096;
constexpr std::size_t max_entries = 100000;
constexpr std::size_t max_name_bytes = 64 * 1024 * 1024;
constexpr std::array signature2{std::byte{'B'}, std::byte{'W'}, std::byte{'A'}, std::byte{'2'}};

[[noreturn]] void invalid(const char* message) { throw std::runtime_error(message); }

void check_options(Options options) {
    if (options.threads < 1 || options.threads > max_threads) {
        throw std::invalid_argument("Worker count must be between 1 and 4.");
    }
}

// Strict UTF-8, relative components, no ambiguous separators.
void check_name(const std::string& name) {
    if (name.empty() || name.size() > max_path_bytes || name.front() == '/' || name.back() == '/') {
        invalid("Invalid archive path.");
    }
    for (std::size_t i = 0; i < name.size();) {
        auto byte = static_cast<unsigned char>(name[i++]);
        if (byte < 128) {
            if (byte < 32 || byte == 127 || byte == '\\' || byte == ':') invalid("Unsafe archive path.");
            continue;
        }
        unsigned remaining = 0, code = 0, minimum = 0;
        if (byte >= 0xc2 && byte <= 0xdf) { remaining = 1; code = byte & 31; minimum = 0x80; }
        else if (byte >= 0xe0 && byte <= 0xef) { remaining = 2; code = byte & 15; minimum = 0x800; }
        else if (byte >= 0xf0 && byte <= 0xf4) { remaining = 3; code = byte & 7; minimum = 0x10000; }
        else invalid("Archive path is not valid UTF-8.");
        if (remaining > name.size() - i) invalid("Truncated UTF-8 path.");
        while (remaining--) {
            byte = static_cast<unsigned char>(name[i++]);
            if ((byte & 0xc0) != 0x80) invalid("Archive path is not valid UTF-8.");
            code = (code << 6) | (byte & 63);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) invalid("Invalid Unicode path.");
    }
    unsigned depth = 0;
    for (std::size_t begin = 0; begin < name.size();) {
        const auto end = name.find('/', begin);
        const auto part = name.substr(begin, end == std::string::npos ? end : end - begin);
        if (part.empty() || part == "." || part == ".." || ++depth > 256) invalid("Unsafe archive path component.");
        if (end == std::string::npos) break;
        begin = end + 1;
    }
}

std::string path_name(const fs::path& path) {
    const auto utf8 = path.generic_u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

class Names {
    std::map<std::string, bool> records_;
    std::size_t bytes_ = 0;
public:
    void add(const std::string& name, bool directory) {
        check_name(name);
        if (records_.size() >= max_entries || name.size() > max_name_bytes - bytes_) invalid("Too much archive metadata.");
        const auto slash = name.rfind('/');
        if (slash != std::string::npos) {
            const auto parent = records_.find(name.substr(0, slash));
            if (parent == records_.end() || !parent->second) invalid("Archive parent directory is missing or is a file.");
        }
        if (!records_.emplace(name, directory).second) invalid("Duplicate archive path.");
        bytes_ += name.size();
    }
};

struct Entry {
    fs::path source;
    std::string name;
    bool directory;
    std::uint64_t size;
};

std::vector<Entry> collect(std::span<const fs::path> inputs, const fs::path& output) {
    if (inputs.empty()) throw std::invalid_argument("At least one input is required.");
    std::vector<Entry> entries;
    std::size_t name_bytes = 0;
    const auto output_path = fs::weakly_canonical(fs::absolute(output));
    auto add = [&](const fs::path& source, const fs::path& stored) {
        const auto status = fs::symlink_status(source);
        const bool directory = fs::is_directory(status);
        if (!directory && !fs::is_regular_file(status)) invalid("Only ordinary files and directories are supported; links are rejected.");
        const auto name = path_name(stored);
        check_name(name);
        if (entries.size() >= max_entries || name.size() > max_name_bytes - name_bytes) invalid("Too much input metadata.");
        if (fs::weakly_canonical(source) == output_path) invalid("Output archive cannot be one of its inputs.");
        name_bytes += name.size();
        entries.push_back({source, name, directory, directory ? 0 : fs::file_size(source)});
    };
    for (const auto& input : inputs) {
        auto root = fs::absolute(input).lexically_normal();
        while (root.filename().empty() && root != root.root_path()) root = root.parent_path();
        if (root == root.root_path()) invalid("Archiving a filesystem root is not supported.");
        add(root, root.filename());
        if (entries.back().directory) {
            for (const auto& child : fs::recursive_directory_iterator(root)) {
                add(child.path(), root.filename() / child.path().lexically_relative(root));
            }
        }
    }
    std::ranges::sort(entries, {}, &Entry::name);
    Names names;
    for (const auto& entry : entries) names.add(entry.name, entry.directory);
    return entries;
}

// All extracted paths are below a freshly created 0700 directory. The format
// cannot create symlinks. Existing destination trees are never traversed.
class Stage {
    fs::path root_;
    fs::path destination_;
public:
    explicit Stage(const fs::path& output) {
        const auto absolute = fs::absolute(output).lexically_normal();
        if (absolute.filename().empty() || absolute == absolute.root_path()) invalid("Invalid output path.");
        destination_ = fs::canonical(absolute.parent_path()) / absolute.filename();
        if (fs::symlink_status(destination_).type() != fs::file_type::not_found) invalid("Output path already exists.");
        auto pattern = (destination_.parent_path() / ".compressor-XXXXXX").string();
        if (!::mkdtemp(pattern.data())) throw std::system_error(errno, std::generic_category(), "Cannot create staging directory");
        root_ = pattern;
    }
    Stage(const Stage&) = delete;
    Stage& operator=(const Stage&) = delete;
    ~Stage() {
        std::error_code error;
        fs::remove_all(root_, error);
    }
    fs::path payload() const { return root_ / "payload"; }
    void publish() {
        // RENAME_NOREPLACE = 1: even a destination created concurrently wins.
        if (::syscall(SYS_renameat2, AT_FDCWD, payload().c_str(), AT_FDCWD, destination_.c_str(), RENAME_NOREPLACE) != 0) {
            throw std::system_error(errno, std::generic_category(), "Cannot publish output without overwriting");
        }
    }
};

void end_of_input(std::istream& input) {
    if (input.peek() != std::char_traits<char>::eof()) invalid("Trailing archive data.");
    if (input.bad() || !input.eof()) invalid("Failed to read archive ending.");
}

std::string read_name(std::istream& input) {
    const auto size = read_number(input, 4);
    if (size == 0 || size > max_path_bytes) invalid("Invalid archive path length.");
    std::string name(static_cast<std::size_t>(size), '\0');
    read_bytes(input, std::as_writable_bytes(std::span(name)));
    return name;
}
} // namespace

std::uint64_t create(std::span<const fs::path> inputs, const fs::path& output, Options options) {
    check_options(options);
    // Enumerate before creating any temporary output, including for an archive
    // located within an input directory. Only metadata is held in memory.
    const auto entries = collect(inputs, output);
    Stage stage(output);
    std::ofstream out(stage.payload(), std::ios::binary | std::ios::noreplace);
    if (!out) invalid("Cannot create temporary archive.");
    write_bytes(out, signature2);
    std::uint64_t total = 0;
    for (const auto& entry : entries) {
        write_number(out, entry.directory ? 1 : 2, 1);
        write_number(out, entry.name.size(), 4);
        write_bytes(out, std::as_bytes(std::span(entry.name)));
        const auto status = fs::symlink_status(entry.source);
        if (entry.directory) {
            if (!fs::is_directory(status)) invalid("Input directory changed while archiving.");
            continue;
        }
        if (!fs::is_regular_file(status)) invalid("Input file changed while archiving.");
        std::ifstream in(entry.source, std::ios::binary);
        if (!in) invalid("Cannot open input file.");
        write_number(out, entry.size, 8);
        const auto actual = compress_stream(in, out, options.threads);
        if (actual != entry.size) invalid("Input file size changed while archiving.");
        add_size(total, actual);
    }
    write_number(out, 0, 1);
    write_number(out, entries.size(), 8);
    write_number(out, total, 8);
    out.close();
    if (!out) invalid("Cannot finish writing archive.");
    stage.publish();
    return total;
}

std::uint64_t extract(const fs::path& input, const fs::path& output, Options options) {
    check_options(options);
    if (!fs::is_regular_file(fs::symlink_status(input))) invalid("Input archive must be an ordinary file.");
    std::ifstream in(input, std::ios::binary);
    if (!in) invalid("Cannot open archive.");
    std::array<std::byte, 4> signature{};
    read_bytes(in, signature);
    if (signature != signature2) {
        if (signature != std::array{std::byte{'B'}, std::byte{'W'}, std::byte{'A'}, std::byte{'1'}}) invalid("Unsupported archive format.");
        in.seekg(0);
        Stage stage(output);
        std::ofstream out(stage.payload(), std::ios::binary | std::ios::noreplace);
        if (!out) invalid("Cannot create temporary output.");
        const auto total = decompress_stream(in, out, options.threads, true);
        out.close();
        if (!out) invalid("Cannot finish writing output.");
        stage.publish();
        return total;
    }
    Stage stage(output);
    fs::create_directory(stage.payload());
    Names names;
    std::uint64_t total = 0, count = 0;
    for (;;) {
        const auto type = read_number(in, 1);
        if (type == 0) break;
        if (type != 1 && type != 2) invalid("Unsupported archive entry type.");
        const auto name = read_name(in);
        names.add(name, type == 1);
        const auto destination = stage.payload() / fs::path(std::u8string(reinterpret_cast<const char8_t*>(name.data()), name.size()));
        if (type == 1) {
            if (!fs::create_directory(destination)) invalid("Archive directory collision.");
        } else {
            const auto expected_size = read_number(in, 8);
            std::ofstream out(destination, std::ios::binary | std::ios::noreplace);
            if (!out) invalid("Cannot create extracted file.");
            const auto actual = decompress_stream(in, out, options.threads, false, expected_size);
            if (actual != expected_size) invalid("Archive file size mismatch.");
            out.close();
            if (!out) invalid("Cannot finish extracted file.");
            add_size(total, actual);
        }
        ++count;
    }
    if (read_number(in, 8) != count || read_number(in, 8) != total) invalid("Archive totals mismatch.");
    end_of_input(in);
    stage.publish();
    return total;
}
} // namespace archive
