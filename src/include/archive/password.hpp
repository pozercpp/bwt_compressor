#pragma once
#include <cstddef>
#include <span>

namespace archive {
// Noncopyable, bounded secret; callbacks fill it without intermediate strings.
class Password {
    char* data_ = nullptr;
    bool locked_ = false;
    std::size_t size_ = 0;
public:
    Password();
    Password(const Password&) = delete;
    Password& operator=(const Password&) = delete;
    ~Password();
    void append(char byte);
    [[nodiscard]] std::span<const char> bytes() const { return {data_, size_}; }
    [[nodiscard]] bool equals(const Password& other) const;
};
}
