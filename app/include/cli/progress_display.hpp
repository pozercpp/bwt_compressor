#pragma once

#include <archive/archive.hpp>

#include <chrono>
#include <cstddef>

namespace cli {

class ProgressDisplay {
    bool dynamic_;
    bool visible_ = false;
    std::size_t width_ = 0;
    std::chrono::duration<double> last_{};

public:
    ProgressDisplay();
    void clear();
    void operator()(const archive::Progress& progress);
};

} // namespace cli
