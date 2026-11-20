#pragma once
#include <archive/archive.hpp>
#include <memory>
#include <streambuf>
#include <istream>
#include <ostream>

namespace archive::detail {
class EncryptBuffer : public std::streambuf {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::streamsize xsputn(const char*, std::streamsize) override;
    int_type overflow(int_type) override;
public:
    EncryptBuffer(std::ostream&, const Options&);
    ~EncryptBuffer();
    void finish();
};
class DecryptBuffer : public std::streambuf {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    int_type underflow() override;
public:
    // The BWE1 signature has already been consumed.
    DecryptBuffer(std::istream&, const Options&);
    ~DecryptBuffer();
};
}
