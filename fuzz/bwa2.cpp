#include "common.hpp"
#include <archive/archive.hpp>
#include <algorithm>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > fuzz::max_archive_input) return 0;
    // BWE1/KDF is deliberately out of scope. Keep malformed signatures on the
    // parser path, but never provide a password callback.
    fuzz::TemporaryDirectory directory;
    directory.write(data, size);
    const auto input = directory.path() / "input";
    archive::Options options;
    options.threads = 1;
    std::uint64_t total = 0;
    unsigned entries = 0;
    const bool bounded = fuzz::accepted([&] {
        (void)archive::list(input, [&](const archive::EntryInfo& entry) {
            if (++entries > 128 || entry.size > fuzz::max_output - total ||
                entry.path.size() > 1024 || std::count(entry.path.begin(), entry.path.end(), '/') > 16)
                throw std::runtime_error("Fuzz budget exhausted");
            total += entry.size;
        }, options);
    });
    if (!bounded) return 0;
    fuzz::accepted([&] { (void)archive::test(input, options); });
    // Only BWA2 is extracted: no restrictive metadata, links or special files.
    // Every iteration owns a private mkdtemp(0700); nothing is extracted into
    // the working directory or a caller-supplied destination.
    if (size >= 4 && std::string_view(reinterpret_cast<const char*>(data), 4) == "BWA2") {
        options.progress = [](const archive::Progress& progress) {
            if (progress.processed_bytes > fuzz::max_output)
                throw std::runtime_error("Fuzz output budget exhausted");
        };
        fuzz::accepted([&] { (void)archive::extract(input, directory.path() / "output", options); });
    }
    return 0;
}
