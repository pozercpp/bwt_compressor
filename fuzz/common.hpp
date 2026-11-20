#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fuzz {
inline constexpr std::size_t max_archive_input = 64 * 1024;
inline constexpr std::uint64_t max_output = 2 * 1024 * 1024;

// Catch only expected validation/I/O failures. bad_alloc and unexpected logic
// errors must reach libFuzzer, just like sanitizer failures.
template<class Function> bool accepted(Function&& function) {
    try { function(); return true; }
    catch (const std::invalid_argument&) { return false; }
    catch (const std::runtime_error&) { return false; }
}

class TemporaryDirectory {
    std::filesystem::path path_;
public:
    TemporaryDirectory() {
        auto name = (std::filesystem::temp_directory_path() / "compressor-fuzz-XXXXXX").string();
        if (!::mkdtemp(name.data())) throw std::runtime_error("mkdtemp failed");
        path_ = name;
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        if (error) std::abort();
    }
    const std::filesystem::path& path() const { return path_; }
    void write(const std::uint8_t* data, std::size_t size) const {
        std::ofstream file(path_ / "input", std::ios::binary);
        file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        file.close();
        if (!file) throw std::runtime_error("Cannot write fuzz input");
    }
};
} // namespace fuzz

// Safe defaults even when launched directly. Explicit libFuzzer arguments may
// override them; the bounded runner adds process-level CPU/file/wall limits.
extern "C" int LLVMFuzzerInitialize(int* argc, char*** argv) {
    static std::vector<std::string> arguments;
    static std::vector<char*> pointers;
    arguments = {(*argv)[0], "-timeout=5", "-rss_limit_mb=512",
                 "-malloc_limit_mb=128", "-max_total_time=60", "-max_len=65536"};
    for (int i = 1; i < *argc; ++i) arguments.emplace_back((*argv)[i]);
    for (auto& argument : arguments) pointers.push_back(argument.data());
    *argc = static_cast<int>(pointers.size());
    pointers.push_back(nullptr);
    *argv = pointers.data();
    return 0;
}