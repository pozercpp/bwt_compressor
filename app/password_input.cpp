#include <cli/password_input.hpp>
#include <array>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <stdexcept>
#include <termios.h>
#include <unistd.h>

namespace cli {
namespace {
volatile std::sig_atomic_t interrupted = 0;
void interrupt(int value) { interrupted = value; }
class Terminal {
    int fd_ = -1;
    termios original_{};
    bool changed_ = false;
    static constexpr std::array signals{SIGINT, SIGTERM, SIGHUP, SIGQUIT};
    std::array<struct sigaction, signals.size()> previous_{};
    std::size_t installed_ = 0;
    void restore() noexcept {
        if (changed_) {
            while (::tcsetattr(fd_, TCSANOW, &original_) != 0 && errno == EINTR) {}
        }
        while (installed_) {
            --installed_;
            ::sigaction(signals[installed_], &previous_[installed_], nullptr);
        }
        if (fd_ >= 0) ::close(fd_);
    }
public:
    Terminal() {
        try {
            fd_ = ::open("/dev/tty", O_RDWR | O_CLOEXEC);
            if (fd_ < 0) throw std::runtime_error("No terminal; supply --password-fd N.");
            if (::tcgetattr(fd_, &original_)) throw std::runtime_error("Cannot read terminal settings.");
            interrupted = 0;
            struct sigaction action{};
            action.sa_handler = interrupt;
            sigemptyset(&action.sa_mask);
            for (int signal : signals) {
                if (::sigaction(signal, &action, &previous_[installed_])) throw std::runtime_error("Cannot protect terminal input.");
                ++installed_;
            }
            auto settings = original_;
            settings.c_lflag &= ~(ECHO | ECHONL);
            if (::tcsetattr(fd_, TCSANOW, &settings)) throw std::runtime_error("Cannot disable terminal echo.");
            changed_ = true;
        } catch (...) { restore(); throw; }
    }
    ~Terminal() { restore(); }
    int fd() const { return fd_; }
    void prompt(const char* text) {
        for (; *text; ++text) {
            if (interrupted) throw std::runtime_error("Password input cancelled.");
            if (::write(fd_, text, 1) != 1) throw std::runtime_error("Cannot write password prompt.");
        }
    }
};
void line(int fd, archive::Password& password, bool terminal) {
    bool cr = false;
    for (;;) {
        if (terminal && interrupted) throw std::runtime_error("Password input cancelled.");
        char byte = 0;
        const auto count = ::read(fd, &byte, 1);
        if (count < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("Cannot read password descriptor.");
        }
        if (count == 0) {
            if (cr) password.append('\r');
            break;
        }
        if (byte == '\n') break;
        if (cr) password.append('\r');
        cr = byte == '\r';
        if (!cr) password.append(byte);
    }
    if (password.bytes().empty()) throw std::runtime_error("Empty password is not allowed.");
}
}
void read_password(archive::Password& password, bool creating, std::optional<int> fd) {
    if (fd) { line(*fd, password, false); return; }
    Terminal terminal;
    terminal.prompt("Password: ");
    line(terminal.fd(), password, true);
    terminal.prompt("\n");
    if (creating) {
        archive::Password confirmation;
        terminal.prompt("Confirm password: ");
        line(terminal.fd(), confirmation, true);
        terminal.prompt("\n");
        if (!password.equals(confirmation)) throw std::runtime_error("Passwords do not match.");
    }
}
}
