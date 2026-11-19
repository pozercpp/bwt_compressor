#include <cli/progress_display.hpp>

#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>

namespace cli {

ProgressDisplay::ProgressDisplay() : dynamic_(::isatty(STDERR_FILENO) && ::isatty(STDOUT_FILENO)) {}

void ProgressDisplay::clear() {
    if (visible_) {
        std::cerr << '\r' << std::string(width_, ' ') << '\r';
        visible_ = false;
    }
}

void ProgressDisplay::operator()(const archive::Progress& progress) {
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
    line << "; time " << std::fixed << std::setprecision(3) << seconds
         << " s; average " << std::setprecision(2) << speed << " MiB/s"
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

} // namespace cli
