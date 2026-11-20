#include <archive/archive.hpp>
#include <archive/archive_internal.hpp>
#include <concurrency/block_pool.hpp>
#include <compression/compression.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <bit>
#include <limits>
#include <cstdlib>
#include <deque>
#include <dirent.h>
#include <fstream>
#include <map>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <streambuf>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>
#include <utility>

// Atomic no-replace publication and a private staging directory on Linux.
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <unistd.h>

namespace archive {
namespace {
namespace fs = std::filesystem;
using namespace detail;
constexpr std::size_t max_path_bytes = 4096;
constexpr std::size_t max_entries = 100000;
constexpr std::size_t max_name_bytes = 64 * 1024 * 1024;
constexpr std::size_t solid_limit = 1024 * 1024;
constexpr std::size_t solid_entries = 4096;
constexpr std::array signature5{std::byte{'B'}, std::byte{'W'}, std::byte{'A'}, std::byte{'5'}};
constexpr std::array signature4{std::byte{'B'}, std::byte{'W'}, std::byte{'A'}, std::byte{'4'}};
constexpr std::array signature3{std::byte{'B'}, std::byte{'W'}, std::byte{'A'}, std::byte{'3'}};
constexpr std::array signature2{std::byte{'B'}, std::byte{'W'}, std::byte{'A'}, std::byte{'2'}};

[[noreturn]] void invalid(const char* message) { throw std::runtime_error(message); }

void check_options(Options options) {
    if (options.threads < 1 || options.threads > max_threads) {
        throw std::invalid_argument("Worker count must be between 1 and 4.");
    }
}

// Strict UTF-8, relative components, no ambiguous separators.
void check_name(const std::string& name, bool target = false) {
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
        if (part.empty() || (!target && (part == "." || part == "..")) || ++depth > 256) invalid("Unsafe archive path component.");
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

// BWA3 stores only rwx bits, signed Unix seconds and nanoseconds.
struct Metadata {
    std::uint32_t mode;
    std::int64_t seconds;
    std::uint32_t nanos;
};

Metadata read_metadata(std::istream& in, bool symlink = false) {
    const auto mode = symlink ? 0 : read_number(in, 4);
    const auto seconds = std::bit_cast<std::int64_t>(read_number(in, 8));
    const auto nanos = read_number(in, 4);
    if (mode > 0777 || nanos >= 1000000000) invalid("Invalid archive metadata.");
    return {static_cast<std::uint32_t>(mode), seconds, static_cast<std::uint32_t>(nanos)};
}

[[noreturn]] void system_error(const char* message) {
    throw std::system_error(errno, std::generic_category(), message);
}

class Fd {
    int fd_ = -1;
public:
    explicit Fd(int fd = -1) : fd_(fd) {}
    ~Fd() { if (fd_ >= 0) ::close(fd_); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) { if (fd_ >= 0) ::close(fd_); fd_ = std::exchange(other.fd_, -1); }
        return *this;
    }
    int get() const { return fd_; }
    void finish() {
        if (::close(std::exchange(fd_, -1)) != 0) system_error("Cannot close file");
    }
};

Fd open_at(int parent, const std::string& name, int flags, mode_t mode = 0) {
    const int fd = ::openat(parent, name.c_str(), flags | O_NOFOLLOW | O_CLOEXEC, mode);
    if (fd < 0) system_error("Cannot open file without following links");
    return Fd(fd);
}

// All components are opened separately. No operation can traverse a symlink.
struct Parent { Fd fd; std::string leaf; };
Parent parent_at(int root, const std::string& name) {
    auto current = open_at(root, ".", O_RDONLY | O_DIRECTORY);
    std::size_t start = 0;
    for (;;) {
        const auto slash = name.find('/', start);
        if (slash == std::string::npos) return {std::move(current), name.substr(start)};
        current = open_at(current.get(), name.substr(start, slash - start), O_RDONLY | O_DIRECTORY);
        start = slash + 1;
    }
}

// Adapt existing streaming codecs to already-verified descriptors, never reopen
// a file by pathname. Buffers own no descriptors; Fd controls their lifetime.
class FdBuffer : public std::streambuf {
    int fd_;
    std::array<char, 8192> bytes_{};
    int_type underflow() override {
        ssize_t count;
        do { count = ::read(fd_, bytes_.data(), bytes_.size()); } while (count < 0 && errno == EINTR);
        if (count < 0) system_error("Cannot read source file");
        if (!count) return traits_type::eof();
        setg(bytes_.data(), bytes_.data(), bytes_.data() + count);
        return traits_type::to_int_type(*gptr());
    }
    std::streamsize xsputn(const char* data, std::streamsize count) override {
        std::streamsize done = 0;
        while (done < count) {
            const auto written = ::write(fd_, data + done, static_cast<std::size_t>(count - done));
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) system_error("Cannot write extracted file");
            done += written;
        }
        return done;
    }
public:
    explicit FdBuffer(int fd) : fd_(fd) {}
};

struct Entry {
    fs::path source;
    std::string name;
    unsigned type = 0;
    std::uint64_t size = 0;
    Metadata meta{};
    std::string target;
    std::uint64_t reference = 0;
    dev_t device{};
    ino_t inode{};
};

std::deque<std::string> components(const std::string& path) {
    std::deque<std::string> result;
    for (std::size_t start = 0; start < path.size();) {
        const auto end = path.find('/', start);
        result.push_back(path.substr(start, end == std::string::npos ? end : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return result;
}

// Resolve in the archive namespace, not in the host filesystem. In particular,
// expand symlinks BEFORE processing subsequent '..' components.
void validate_links(const std::vector<Entry>& entries) {
    std::map<std::string, const Entry*> records;
    std::size_t bytes = 0;
    for (const auto& entry : entries) {
        for (const auto* str : {&entry.name, &entry.target}) {
            if (str == &entry.target && entry.type != 3) continue;
            if (str->size() > max_name_bytes - bytes) invalid("Too much archive metadata.");
            bytes += str->size();
        }
        records.emplace(entry.name, &entry);
    }
    std::size_t budget = 10000000;
    for (const auto& entry : entries) {
        if (entry.type != 3) continue;
        check_name(entry.target, true);
        std::string resolved;
        const auto slash = entry.name.rfind('/');
        if (slash != std::string::npos) resolved = entry.name.substr(0, slash);
        auto todo = components(entry.target);
        unsigned expansions = 1; // Include the link being validated.
        while (!todo.empty()) {
            if (!budget--) invalid("Symlink resolution budget exceeded.");
            auto part = std::move(todo.front());
            todo.pop_front();
            if (part == ".") continue;
            if (part == "..") {
                if (resolved.empty()) invalid("Symlink escapes archive root.");
                const auto parent = resolved.rfind('/');
                resolved = parent == std::string::npos ? "" : resolved.substr(0, parent);
                continue;
            }
            const auto candidate = resolved.empty() ? part : resolved + '/' + part;
            if (candidate.size() > max_path_bytes) invalid("Resolved symlink path too long.");
            const auto found = records.find(candidate);
            if (found == records.end()) invalid("Dangling symlink.");
            const auto& target = *found->second;
            if (target.type == 3) {
                if (++expansions > 40) invalid("Symlink cycle or chain too long.");
                check_name(target.target, true);
                auto prefix = components(target.target);
                todo.insert(todo.begin(), prefix.begin(), prefix.end());
            } else {
                if (target.type == 5) invalid("Symlink to FIFO is not supported.");
                if (!todo.empty() && target.type != 1) invalid("Symlink traverses a non-directory.");
                resolved = candidate;
            }
        }
    }
}

unsigned entry_type(mode_t mode) {
    if (S_ISDIR(mode)) return 1;
    if (S_ISREG(mode)) return 2;
    if (S_ISLNK(mode)) return 3;
    if (S_ISFIFO(mode)) return 5;
    invalid("Devices, sockets and unknown file types are not supported.");
}

Parent source_parent(const fs::path& path) {
    auto root = open_at(AT_FDCWD, "/", O_RDONLY | O_DIRECTORY);
    return parent_at(root.get(), path.relative_path().string());
}

std::string read_link(int parent, const std::string& name) {
    std::array<char, max_path_bytes + 1> data{};
    const auto size = ::readlinkat(parent, name.c_str(), data.data(), data.size());
    if (size < 0) system_error("Cannot read symlink");
    if (static_cast<std::size_t>(size) > max_path_bytes) invalid("Symlink target too long.");
    return {data.data(), static_cast<std::size_t>(size)};
}

void check_source(const Entry& entry, const struct stat& status) {
    if (status.st_dev != entry.device || status.st_ino != entry.inode ||
        entry_type(status.st_mode) != (entry.type == 4 ? 2 : entry.type) ||
        ((entry.type == 2 || entry.type == 4) && static_cast<std::uint64_t>(status.st_size) != entry.size) ||
        (status.st_mode & 0777) != entry.meta.mode ||
        (entry.type != 1 && (status.st_mtim.tv_sec != entry.meta.seconds || status.st_mtim.tv_nsec != entry.meta.nanos))) {
        invalid("Input changed while archiving.");
    }
}

std::vector<Entry> collect(std::span<const fs::path> inputs, const fs::path& output, Options options) {
    if (inputs.empty()) throw std::invalid_argument("At least one input is required.");
    std::vector<Entry> entries;
    std::size_t name_bytes = 0;
    const auto output_path = fs::weakly_canonical(fs::absolute(output));
    auto add = [&](const fs::path& source, const fs::path& stored) {
        auto parent = source_parent(source);
        struct stat status{};
        if (::fstatat(parent.fd.get(), parent.leaf.c_str(), &status, AT_SYMLINK_NOFOLLOW)) system_error("Cannot stat input");
        const auto type = entry_type(status.st_mode);
        if (type == 5 && !options.allow_fifo) invalid("FIFO requires --allow-fifo.");
        const auto name = path_name(stored);
        check_name(name);
        if (entries.size() >= max_entries || name.size() > max_name_bytes - name_bytes) invalid("Too much input metadata.");
        if (source == output_path) invalid("Output archive cannot be one of its inputs.");
        name_bytes += name.size();
        Entry entry{source, name, type, type == 2 ? static_cast<std::uint64_t>(status.st_size) : 0,
            {static_cast<std::uint32_t>(status.st_mode & 0777), status.st_mtim.tv_sec,
             static_cast<std::uint32_t>(status.st_mtim.tv_nsec)}, "", 0, status.st_dev, status.st_ino};
        if (type == 3) {
            entry.target = read_link(parent.fd.get(), parent.leaf);
            check_name(entry.target, true);
            if (entry.target.size() > max_name_bytes - name_bytes) invalid("Too much input metadata.");
            name_bytes += entry.target.size();
        }
        entries.push_back(std::move(entry));
    };
    // Descriptor-based enumeration avoids following a directory replaced by a
    // symlink. Recheck identity when opening a directory collected by add().
    auto descend = [&](auto&& self, const fs::path& source, const fs::path& stored) -> void {
        auto parent = source_parent(source);
        auto fd = open_at(parent.fd.get(), parent.leaf, O_RDONLY | O_DIRECTORY);
        struct stat status{};
        if (::fstat(fd.get(), &status)) system_error("Cannot stat directory");
        check_source(entries.back(), status);
        const int scan_fd = ::dup(fd.get());
        if (scan_fd < 0) system_error("Cannot duplicate directory descriptor");
        DIR* raw = ::fdopendir(scan_fd);
        if (!raw) {
            const auto error = errno;
            ::close(scan_fd);
            errno = error;
            system_error("Cannot enumerate directory");
        }
        const auto close_dir = [](DIR* dir) { ::closedir(dir); };
        std::unique_ptr<DIR, decltype(close_dir)> dir(raw, close_dir);
        for (;;) {
            errno = 0;
            const auto* child = ::readdir(dir.get());
            if (!child) { if (errno) system_error("Cannot enumerate directory"); break; }
            const std::string name = child->d_name;
            if (name == "." || name == "..") continue;
            add(source / name, stored / name);
            if (entries.back().type == 1) self(self, source / name, stored / name);
        }
    };
    for (const auto& input : inputs) {
        auto root = fs::absolute(input).lexically_normal();
        while (root.filename().empty() && root != root.root_path()) root = root.parent_path();
        if (root == root.root_path()) invalid("Archiving a filesystem root is not supported.");
        add(root, root.filename());
        if (entries.back().type == 1) descend(descend, root, root.filename());
    }
    std::ranges::sort(entries, {}, &Entry::name);
    Names names;
    std::map<std::pair<dev_t, ino_t>, std::size_t> inodes;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        auto& entry = entries[i];
        names.add(entry.name, entry.type == 1);
        if (entry.type == 2) {
            const auto [it, added] = inodes.emplace(std::pair{entry.device, entry.inode}, i);
            if (!added) { entry.type = 4; entry.reference = it->second; }
        }
    }
    validate_links(entries);
    return entries;
}

// Best-effort cleanup of our private tree; never follow a link, including when
// reopening restrictive directories after metadata restoration.
void remove_tree_at(int parent, const char* name) noexcept {
    struct stat status{};
    if (::fstatat(parent, name, &status, AT_SYMLINK_NOFOLLOW)) return;
    if (!S_ISDIR(status.st_mode)) { ::unlinkat(parent, name, 0); return; }
    if (::fchmodat(parent, name, 0700, AT_SYMLINK_NOFOLLOW)) return;
    const int fd = ::openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return;
    DIR* dir = ::fdopendir(fd);
    if (!dir) { ::close(fd); return; }
    while (const auto* entry = ::readdir(dir)) {
        if (std::string_view(entry->d_name) != "." && std::string_view(entry->d_name) != "..")
            remove_tree_at(fd, entry->d_name);
    }
    ::closedir(dir);
    ::unlinkat(parent, name, AT_REMOVEDIR);
}

// Private staging tree; publication never replaces an existing destination.
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
        // mkdtemp's 0700 can be reduced by a restrictive process umask.
        if (::chmod(root_.c_str(), 0700) != 0) {
            const auto error = errno;
            ::rmdir(root_.c_str());
            throw std::system_error(error, std::generic_category(), "Cannot prepare staging directory");
        }
    }
    Stage(const Stage&) = delete;
    Stage& operator=(const Stage&) = delete;
    ~Stage() { remove_tree_at(AT_FDCWD, root_.c_str()); }
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
// A sink with no storage: decoded blocks are released by decompress_stream.
class DiscardBuffer : public std::streambuf {
    std::streamsize xsputn(const char*, std::streamsize count) override { return count; }
    int_type overflow(int_type ch) override { return traits_type::not_eof(ch); }
};

unsigned read_format(std::istream& in) {
    std::array<std::byte, 4> signature{};
    read_bytes(in, signature);
    if (signature == signature5) return 5;
    if (signature == signature4) return 4;
    if (signature == signature3) return 3;
    if (signature == signature2) return 2;
    if (signature != std::array{std::byte{'B'}, std::byte{'W'}, std::byte{'A'}, std::byte{'1'}}) {
        invalid("Unsupported archive format.");
    }
    in.seekg(0);
    if (!in) invalid("Cannot rewind archive.");
    return 1;
}

std::ifstream open_archive(const fs::path& input) {
    if (!fs::is_regular_file(fs::symlink_status(input))) invalid("Input archive must be an ordinary file.");
    std::ifstream in(input, std::ios::binary);
    if (!in) invalid("Cannot open archive.");
    return in;
}

void apply_metadata(int root, const Entry& entry) {
    if (!std::in_range<time_t>(entry.meta.seconds)) invalid("Modification time is out of range.");
    auto parent = parent_at(root, entry.name);
    const timespec times[2]{{0, UTIME_OMIT}, {static_cast<time_t>(entry.meta.seconds), entry.meta.nanos}};
    if (entry.type == 3) {
        if (::utimensat(parent.fd.get(), parent.leaf.c_str(), times, AT_SYMLINK_NOFOLLOW)) system_error("Cannot restore symlink time");
    } else {
        auto fd = open_at(parent.fd.get(), parent.leaf, O_RDONLY | O_NONBLOCK);
        if (::futimens(fd.get(), times) || ::fchmod(fd.get(), entry.meta.mode)) system_error("Cannot restore metadata");
    }
}

void materialize_links(int root, const std::vector<Entry>& entries) {
    // Hard links and FIFO first, symbolic links last. No data is ever written
    // through a link or a special file.
    for (unsigned phase : {4u, 5u, 3u}) {
        for (const auto& entry : entries) {
            if (entry.type != phase) continue;
            auto parent = parent_at(root, entry.name);
            if (phase == 4) {
                auto source = parent_at(root, entries[entry.reference].name);
                struct stat status{};
                if (::fstatat(source.fd.get(), source.leaf.c_str(), &status, AT_SYMLINK_NOFOLLOW) || !S_ISREG(status.st_mode))
                    invalid("Hard link source is not a regular file.");
                if (::linkat(source.fd.get(), source.leaf.c_str(), parent.fd.get(), parent.leaf.c_str(), 0)) system_error("Cannot create hard link");
            } else if (phase == 5) {
                if (::mkfifoat(parent.fd.get(), parent.leaf.c_str(), 0600) ||
                    ::fchmodat(parent.fd.get(), parent.leaf.c_str(), 0600, AT_SYMLINK_NOFOLLOW)) system_error("Cannot create FIFO");
            } else if (::symlinkat(entry.target.c_str(), parent.fd.get(), parent.leaf.c_str())) system_error("Cannot create symlink");
        }
    }
}

// Shared record validation for extraction, listing and full verification.
// root == -1 means read-only inspection; no filesystem objects are created.
std::uint64_t walk_records(std::istream& in, int root, bool verify, Options options,
                          const EntryVisitor& visitor, unsigned format, ProgressTracker* progress = nullptr) {
    Names names;
    std::vector<Entry> entries;
    std::size_t metadata_bytes = 0;
    std::uint64_t total = 0;
    DiscardBuffer buffer;
    std::ostream discard(&buffer);
    for (;;) {
        const auto type = static_cast<unsigned>(read_number(in, 1));
        if (type == 0) break;
        if (type == 6 && format == 5) {
            const auto count = read_number(in, 4);
            if (!count || count > solid_entries) invalid("Invalid solid entry count.");
            const auto first = entries.size();
            std::uint64_t size = 0;
            for (std::uint64_t i = 0; i < count; ++i) {
                Entry member;
                member.type = 2;
                member.name = read_name(in);
                names.add(member.name, false);
                if (member.name.size() > max_name_bytes - metadata_bytes) invalid("Too much archive metadata.");
                metadata_bytes += member.name.size();
                member.meta = read_metadata(in);
                member.size = read_number(in, 8);
                if (member.size > solid_limit - size) invalid("Solid group exceeds limit.");
                size += member.size;
                entries.push_back(std::move(member));
            }
            std::ostringstream decoded;
            const auto actual = verify
                ? decompress_stream(in, decoded, options.threads, false, size)
                : list_stream(in, false);
            if (actual != size) invalid("Solid size mismatch.");
            const auto data = std::move(decoded).str();
            std::size_t offset = 0;
            for (std::size_t i = first; i < entries.size(); ++i) {
                const auto& member = entries[i];
                if (root >= 0) {
                    auto parent = parent_at(root, member.name);
                    auto fd = open_at(parent.fd.get(), parent.leaf, O_WRONLY | O_CREAT | O_EXCL, 0600);
                    if (::fchmod(fd.get(), 0600)) system_error("Cannot prepare file");
                    FdBuffer output_buffer(fd.get());
                    std::ostream output(&output_buffer);
                    output.write(data.data() + offset, member.size);
                    if (!output) invalid("Cannot write solid file.");
                    fd.finish();
                }
                offset += member.size;
                if (visitor) visitor({member.name, false, member.size, EntryType::file, {}});
            }
            add_size(total, size);
            if (progress) progress->advance(size);
            continue;
        }
        if (type > (format >= 4 ? 5u : 2u)) invalid("Unsupported archive entry type.");
        Entry entry;
        entry.type = type;
        entry.name = read_name(in);
        names.add(entry.name, type == 1);
        if (entry.name.size() > max_name_bytes - metadata_bytes) invalid("Too much archive metadata.");
        metadata_bytes += entry.name.size();
        if (format >= 3 && type != 4) entry.meta = read_metadata(in, type == 3);
        if (type == 3) {
            entry.target = read_name(in);
            check_name(entry.target, true);
            if (entry.target.size() > max_name_bytes - metadata_bytes) invalid("Too much archive metadata.");
            metadata_bytes += entry.target.size();
        } else if (type == 4) {
            entry.reference = read_number(in, 8);
            if (entry.reference >= entries.size() || entries[entry.reference].type != 2) invalid("Invalid hard link reference.");
            entry.size = entries[entry.reference].size;
            entry.target = entries[entry.reference].name;
            add_size(total, entry.size);
            if (progress) progress->advance(entry.size);
        } else if (type == 5) {
            if (root >= 0 && !options.allow_fifo) invalid("FIFO requires --allow-fifo.");
        } else if (type == 1) {
            if (root >= 0) {
                auto parent = parent_at(root, entry.name);
                if (::mkdirat(parent.fd.get(), parent.leaf.c_str(), 0777)) system_error("Cannot create directory");
                if (format >= 3 && ::fchmodat(parent.fd.get(), parent.leaf.c_str(), 0700, AT_SYMLINK_NOFOLLOW)) system_error("Cannot prepare directory");
            }
        } else {
            entry.size = read_number(in, 8);
            Fd fd;
            if (root >= 0) {
                auto parent = parent_at(root, entry.name);
                fd = open_at(parent.fd.get(), parent.leaf, O_WRONLY | O_CREAT | O_EXCL, 0666);
                if (format >= 3 && ::fchmod(fd.get(), 0600)) system_error("Cannot prepare file");
            }
            FdBuffer output_buffer(fd.get());
            std::ostream output(&output_buffer);
            const auto actual = verify
                ? decompress_stream(in, root >= 0 ? output : discard, options.threads, false, entry.size, progress)
                : list_stream(in, false);
            if (actual != entry.size) invalid("Archive file size mismatch.");
            if (root >= 0) fd.finish();
            add_size(total, actual);
        }
        if (visitor) visitor({entry.name, type == 1, entry.size, static_cast<EntryType>(type), entry.target});
        entries.push_back(std::move(entry));
    }
    if (read_number(in, 8) != entries.size() || read_number(in, 8) != total) invalid("Archive totals mismatch.");
    end_of_input(in);
    if (format >= 4) validate_links(entries);
    if (root >= 0) {
        materialize_links(root, entries);
        if (format >= 3) {
            for (const auto& entry : std::views::reverse(entries))
                if (entry.type != 1 && entry.type != 4) apply_metadata(root, entry);
            for (const auto& entry : std::views::reverse(entries))
                if (entry.type == 1) apply_metadata(root, entry);
        }
    }
    return total;
}

std::uint64_t inspect(const fs::path& input, bool verify, Options options,
                      const EntryVisitor& visitor) {
    check_options(options);
    auto in = open_archive(input);
    const auto format = read_format(in);
    if (format != 1) return walk_records(in, -1, verify, options, visitor, format);
    DiscardBuffer buffer;
    std::ostream discard(&buffer);
    const auto total = verify ? decompress_stream(in, discard, options.threads, true)
                              : list_stream(in, true);
    if (visitor) visitor({"", false, total});
    return total;
}
} // namespace

// One archive-wide queue. Headers travel with the following block, so only
// the caller touches the output and completion order cannot change the format.
class ArchiveWriter {
    struct Pending {
        std::string prefix;
        std::future<concurrency::BlockPool::Bytes> result;
        std::size_t original;
    };
    std::ostream& destination_;
    detail::ProgressTracker& progress_;
    const std::size_t limit_;
    concurrency::BlockPool pool_;
    std::deque<Pending> pending_;
public:
    std::ostringstream headers;
    ArchiveWriter(std::ostream& out, unsigned threads, detail::ProgressTracker& progress)
        : destination_(out), progress_(progress), limit_(2 * threads), pool_(threads) {}
    void next() {
        auto& item = pending_.front();
        auto encoded = item.result.get();
        destination_.write(item.prefix.data(), item.prefix.size());
        detail::write_number(destination_, encoded.size(), 4);
        detail::write_bytes(destination_, encoded);
        const auto original = item.original;
        pending_.pop_front();
        progress_.advance(original);
    }
    void flush() {
        while (!pending_.empty()) next();
        const auto bytes = headers.str();
        destination_.write(bytes.data(), bytes.size());
        if (!destination_) throw std::runtime_error("Failed to write archive.");
        headers.str({});
    }
    void reserve_block() {
        if (pending_.size() == limit_) next();
    }
    void bound_headers() {
        // Metadata-only runs must also have bounded buffering.
        if (headers.tellp() >= 1024 * 1024) flush();
    }
    std::uint64_t compress(std::istream& in) {
        headers.write("BWA1", 4);
        std::uint64_t total = 0;
        for (;;) {
            // Reserve a slot before allocating/reading the next input block.
            reserve_block();
            concurrency::BlockPool::Bytes bytes(bwt::max_block_size);
            in.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
            const auto count = static_cast<std::size_t>(in.gcount());
            if (in.bad() || (in.fail() && !in.eof())) throw std::runtime_error("Failed to read input.");
            if (count) {
                bytes.resize(count);
                auto prefix = headers.str();
                headers.str({});
                pending_.push_back({std::move(prefix), pool_.submit(std::move(bytes), true), count});
                detail::add_size(total, count);
            }
            if (in.eof()) break;
        }
        detail::write_number(headers, 0, 4);
        detail::write_number(headers, total, 8);
        return total;
    }
};

std::uint64_t create(std::span<const fs::path> inputs, const fs::path& output, Options options) {
    ProgressTracker progress(options.progress);
    check_options(options);
    // Enumerate before creating any temporary output, including for an archive
    // located within an input directory. Only metadata is held in memory.
    const auto entries = collect(inputs, output, options);
    std::uint64_t input_size = 0;
    for (const auto& entry : entries)
        if (entry.type == 2 || entry.type == 4) add_size(input_size, entry.size);
    progress.start(input_size, input_size);
    Stage stage(output);
    std::ofstream destination(stage.payload(), std::ios::binary | std::ios::noreplace);
    if (!destination) invalid("Cannot create temporary archive.");
    ArchiveWriter writer(destination, options.threads, progress);
    auto& out = writer.headers;
    write_bytes(out, signature5);
    std::uint64_t total = 0;
    for (std::size_t index = 0; index < entries.size();) {
        writer.bound_headers();
        const auto& entry = entries[index];
        if (entry.type == 2 && entry.size <= solid_limit) {
            writer.reserve_block();
            const auto first = index;
            std::uint64_t size = 0;
            while (index < entries.size() && index - first < solid_entries &&
                   entries[index].type == 2 && entries[index].size <= solid_limit - size) {
                size += entries[index++].size;
            }
            write_number(out, 6, 1);
            write_number(out, index - first, 4);
            std::string data;
            data.reserve(size);
            for (auto i = first; i < index; ++i) {
                const auto& member = entries[i];
                auto parent = source_parent(member.source);
                auto fd = open_at(parent.fd.get(), parent.leaf, O_RDONLY | O_NONBLOCK);
                struct stat status{};
                if (::fstat(fd.get(), &status)) system_error("Cannot stat source file");
                check_source(member, status);
                write_number(out, member.name.size(), 4);
                write_bytes(out, std::as_bytes(std::span(member.name)));
                write_number(out, member.meta.mode, 4);
                write_number(out, std::bit_cast<std::uint64_t>(member.meta.seconds), 8);
                write_number(out, member.meta.nanos, 4);
                write_number(out, member.size, 8);
                FdBuffer input_buffer(fd.get());
                std::istream input(&input_buffer);
                const auto offset = data.size();
                data.resize(offset + member.size);
                input.read(data.data() + offset, member.size);
                if (input.gcount() != static_cast<std::streamsize>(member.size) ||
                    input.peek() != std::char_traits<char>::eof()) invalid("Input file size changed while archiving.");
                if (::fstat(fd.get(), &status)) system_error("Cannot stat source file");
                check_source(member, status);
            }
            std::istringstream input(std::move(data));
            if (writer.compress(input) != size) invalid("Solid size mismatch.");
            add_size(total, size);
            continue;
        }
        ++index;
        auto parent = source_parent(entry.source);
        struct stat status{};
        if (::fstatat(parent.fd.get(), parent.leaf.c_str(), &status, AT_SYMLINK_NOFOLLOW)) system_error("Cannot stat source");
        check_source(entry, status);
        write_number(out, entry.type, 1);
        write_number(out, entry.name.size(), 4);
        write_bytes(out, std::as_bytes(std::span(entry.name)));
        if (entry.type == 4) {
            write_number(out, entry.reference, 8);
            add_size(total, entry.size);
            writer.flush();
            progress.advance(entry.size);
            continue;
        }
        if (entry.type != 3) write_number(out, entry.meta.mode, 4);
        write_number(out, std::bit_cast<std::uint64_t>(entry.meta.seconds), 8);
        write_number(out, entry.meta.nanos, 4);
        if (entry.type == 3) {
            if (read_link(parent.fd.get(), parent.leaf) != entry.target) invalid("Symlink changed while archiving.");
            write_number(out, entry.target.size(), 4);
            write_bytes(out, std::as_bytes(std::span(entry.target)));
        }
        if (entry.type != 2) continue;
        auto fd = open_at(parent.fd.get(), parent.leaf, O_RDONLY | O_NONBLOCK);
        if (::fstat(fd.get(), &status)) system_error("Cannot stat source file");
        check_source(entry, status);
        FdBuffer input_buffer(fd.get());
        std::istream in(&input_buffer);
        write_number(out, entry.size, 8);
        const auto actual = writer.compress(in);
        if (actual != entry.size) invalid("Input file size changed while archiving.");
        if (::fstat(fd.get(), &status)) system_error("Cannot stat source file");
        check_source(entry, status);
        add_size(total, actual);
    }
    write_number(out, 0, 1);
    write_number(out, entries.size(), 8);
    write_number(out, total, 8);
    writer.flush();
    const auto result_size = destination.tellp();
    if (result_size < 0) invalid("Cannot determine archive size.");
    destination.close();
    if (!destination) invalid("Cannot finish writing archive.");
    stage.publish();
    progress.finish(static_cast<std::uint64_t>(result_size));
    return total;
}

std::uint64_t extract(const fs::path& input, const fs::path& output, Options options) {
    ProgressTracker progress(options.progress);
    check_options(options);
    auto in = open_archive(input);
    const auto format = read_format(in);
    progress.start(fs::file_size(input), std::nullopt);
    if (format == 1) {
        Stage stage(output);
        std::ofstream out(stage.payload(), std::ios::binary | std::ios::noreplace);
        if (!out) invalid("Cannot create temporary output.");
        const auto total = decompress_stream(in, out, options.threads, true,
            std::numeric_limits<std::uint64_t>::max(), &progress);
        out.close();
        if (!out) invalid("Cannot finish writing output.");
        stage.publish();
        progress.finish(total);
        return total;
    }
    Stage stage(output);
    fs::create_directory(stage.payload());
    if (format >= 3) fs::permissions(stage.payload(), fs::perms::owner_all, fs::perm_options::add);
    auto root = open_at(AT_FDCWD, stage.payload().string(), O_RDONLY | O_DIRECTORY);
    const auto total = walk_records(in, root.get(), true, options, {}, format, &progress);
    stage.publish();
    progress.finish(total);
    return total;
}
std::uint64_t list(const fs::path& input, const EntryVisitor& visitor) {
    return inspect(input, false, {}, visitor);
}

std::uint64_t test(const fs::path& input, Options options) {
    return inspect(input, true, options, {});
}
} // namespace archive
