// Test-only syscall failure injection after link creation / metadata application.
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <dlfcn.h>
#include <sys/stat.h>

extern "C" int utimensat(int fd, const char* path, const timespec times[2], int flags) noexcept {
    if (std::getenv("COMPRESSOR_TEST_FAULT") && std::strcmp(path, "fail-time") == 0) {
        errno = EIO;
        return -1;
    }
    if (const char* destination = std::getenv("COMPRESSOR_TEST_COLLISION")) {
        ::mkdir(destination, 0700);
    }
    static const auto real = reinterpret_cast<decltype(&utimensat)>(dlsym(RTLD_NEXT, "utimensat"));
    return real(fd, path, times, flags);
}
extern "C" int fchmod(int fd, mode_t mode) noexcept {
    if (std::getenv("COMPRESSOR_TEST_FAULT") && mode == 0401) {
        errno = EIO;
        return -1;
    }
    static const auto real = reinterpret_cast<decltype(&fchmod)>(dlsym(RTLD_NEXT, "fchmod"));
    return real(fd, mode);
}
