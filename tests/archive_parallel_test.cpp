#include <archive/archive.hpp>
#include <filesystem>
#include <fstream>
#include <array>
#include <thread>
#include <stdexcept>
#include <cstdlib>
#include <iterator>
namespace fs = std::filesystem;
void require(bool value) { if (!value) throw std::runtime_error("parallel archive check failed"); }
std::string read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
int main() {
    char pattern[] = "/tmp/archive-parallel-XXXXXX";
    auto dir = mkdtemp(pattern);
    require(dir);
    fs::path root(dir);
    try {
        fs::create_directory(root / "source");
        for (int i = 0; i < 12; ++i) {
            std::ofstream out(root / "source" / ("file" + std::to_string(100+i)));
            // Each file is a separate solid group. Different data give workers
            // different workloads while input metadata remains identical.
            for (int j = 0; j < 600000; ++j) out.put(i % 2 ? char(j % 251) : 'a');
        }
        std::array inputs{root / "source"};
        for (unsigned threads : {1u, 2u, 4u}) {
            auto packed = root / ("archive" + std::to_string(threads));
            require(archive::create(inputs, packed, {.threads=threads}) == 7200000);
            require(archive::test(packed, {.threads=threads}) == 7200000);
            if (threads != 1) require(read(packed) == read(root / "archive1"));
        }
        auto caller = std::this_thread::get_id();
        // At first completion the bounded producer has not read arbitrarily
        // far ahead. Removing a future source must fail and publish nothing.
        bool removed = false;
        bool failed = false;
        try {
            archive::Options options{.threads=2};
            options.progress = [&](const archive::Progress& p) {
                require(std::this_thread::get_id() == caller);
                if (p.processed_bytes && !removed) {
                    removed = true;
                    fs::remove(root / "source/file104");
                }
            };
            (void)archive::create(inputs, root / "failed", options);
        } catch (const std::exception&) { failed = true; }
        require(removed && failed && !fs::exists(root / "failed"));
        // Throw with work pending: workers must join and staging must disappear.
        failed = false;
        try {
            archive::Options options{.threads=4};
            options.progress = [](const archive::Progress& p) {
                if (p.processed_bytes) throw std::runtime_error("injected writer failure");
            };
            (void)archive::create(inputs, root / "callback-failed", options);
        } catch (const std::exception&) { failed = true; }
        require(failed && !fs::exists(root / "callback-failed"));
        for (const auto& item : fs::directory_iterator(root))
            require(!item.path().filename().string().starts_with(".compressor-"));
        fs::remove_all(root);
    } catch (...) { fs::remove_all(root); throw; }
}
