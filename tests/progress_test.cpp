#include <archive/archive.hpp>
#include <compression/compression.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
#include <cstdlib>

namespace fs = std::filesystem;
void require(bool condition) { if (!condition) throw std::runtime_error("Progress check failed"); }

int main() {
    char pattern[] = "/tmp/compressor-progress-XXXXXX";
    const auto directory = ::mkdtemp(pattern);
    if (!directory) return 1;
    const fs::path root(directory);
    try {
        const auto source = root / "source";
        const auto size = bwt::max_block_size + 13;
        { std::ofstream out(source); out << std::string(size, 'a'); }
        const auto caller = std::this_thread::get_id();
        std::vector<archive::Progress> events;
        fs::path destination;
        archive::Options options{.threads = 2};
        options.progress = [&](const archive::Progress& event) {
            require(std::this_thread::get_id() == caller);
            if (!events.empty()) {
                require(event.processed_bytes >= events.back().processed_bytes);
                require(event.elapsed >= events.back().elapsed);
                require(!events.back().completed);
            }
            if (event.completed) require(fs::exists(destination));
            events.push_back(event);
        };
        auto check = [&](bool compressing, std::uint64_t logical, std::uint64_t packed) {
            require(events.size() >= 2);
            require(events.front().processed_bytes == 0 && !events.front().completed);
            require(events.back().completed && events.back().processed_bytes == logical);
            require(events.back().total_bytes == logical);
            require(events.back().input_bytes == (compressing ? logical : packed));
            require(events.back().result_bytes == (compressing ? packed : logical));
            for (std::size_t i = 0; i + 1 < events.size(); ++i) {
                require(events[i].total_bytes.has_value() == compressing);
                if (compressing) require(events[i].total_bytes == logical);
                require(!events[i].result_bytes);
            }
            events.clear();
        };
        const auto legacy = root / "legacy";
        destination = legacy;
        require(archive::compress_file(source, legacy, options) == size);
        require(events.size() == 4); // initial, two committed blocks, completion
        check(true, size, fs::file_size(legacy));
        destination = root / "legacy-out";
        require(archive::decompress_file(legacy, destination, options) == size);
        check(false, size, fs::file_size(legacy));
        destination = root / "extract-legacy";
        require(archive::extract(legacy, destination, options) == size);
        check(false, size, fs::file_size(legacy));

        const auto link = root / "hard";
        fs::create_hard_link(source, link);
        const std::array inputs{source, link};
        const auto packed = root / "packed";
        destination = packed;
        require(archive::create(inputs, packed, options) == 2 * size);
        require(events.size() == 5); // two data blocks and one hard-link record
        check(true, 2 * size, fs::file_size(packed));
        destination = root / "unpacked";
        require(archive::extract(packed, destination, options) == 2 * size);
        check(false, 2 * size, fs::file_size(packed));
        require(archive::test(packed, options) == 2 * size);
        require(events.empty());

        // A callback failure while blocks are pending must clean up staging.
        options.progress = [](const archive::Progress& p) {
            if (p.processed_bytes) throw std::runtime_error("callback failure");
        };
        destination = root / "failed";
        bool failed = false;
        try { (void)archive::extract(packed, destination, options); }
        catch (const std::runtime_error&) { failed = true; }
        require(failed && !fs::exists(destination));
        for (const auto& entry : fs::directory_iterator(root))
            require(!entry.path().filename().string().starts_with(".compressor-"));

        // Publication precedes final notification, even if the observer throws.
        destination = root / "final-callback-failed";
        options.progress = [](const archive::Progress& p) {
            if (p.completed) throw std::runtime_error("final callback failure");
        };
        failed = false;
        try { (void)archive::extract(packed, destination, options); }
        catch (const std::runtime_error&) { failed = true; }
        require(failed && fs::file_size(destination / "source") == size);

        // Empty input still reports initial and successful final events.
        { std::ofstream out(root / "empty"); }
        events.clear();
        options.progress = [&](const archive::Progress& p) { events.push_back(p); };
        destination = root / "empty-packed";
        const std::array empty_inputs{root / "empty"};
        require(archive::create(empty_inputs, destination, options) == 0);
        require(events.size() == 2);
        check(true, 0, fs::file_size(destination));
        fs::remove_all(root);
        return 0;
    } catch (const std::exception& error) {
        fs::remove_all(root);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
