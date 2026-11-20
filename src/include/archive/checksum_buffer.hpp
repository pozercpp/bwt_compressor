#pragma once

#include <cstdint>
#include <streambuf>

namespace archive::detail {

// CRC-32/ISO-HDLC: reflected polynomial 0xedb88320, init/xorout ffffffff.
// No get/put area: only bytes actually consumed or successfully written count.
class ChecksumBuffer : public std::streambuf {
    std::streambuf* source_;
    std::uint32_t crc_ = 0xffffffffu;
    void update(const char* data, std::streamsize size) {
        for (std::streamsize i = 0; i < size; ++i) {
            crc_ ^= static_cast<unsigned char>(data[i]);
            for (unsigned bit = 0; bit < 8; ++bit)
                crc_ = (crc_ >> 1) ^ ((crc_ & 1) ? 0xedb88320u : 0);
        }
    }
    std::streamsize xsgetn(char* data, std::streamsize size) override {
        const auto count = source_->sgetn(data, size);
        update(data, count);
        return count;
    }
    int_type underflow() override { return source_->sgetc(); }
    int_type uflow() override {
        const auto value = source_->sbumpc();
        if (!traits_type::eq_int_type(value, traits_type::eof())) {
            const char byte = traits_type::to_char_type(value);
            update(&byte, 1);
        }
        return value;
    }
    std::streamsize xsputn(const char* data, std::streamsize size) override {
        const auto count = source_->sputn(data, size);
        update(data, count);
        return count;
    }
    int_type overflow(int_type value) override {
        if (traits_type::eq_int_type(value, traits_type::eof())) return traits_type::not_eof(value);
        const char byte = traits_type::to_char_type(value);
        return xsputn(&byte, 1) == 1 ? value : traits_type::eof();
    }
    int sync() override { return source_->pubsync(); }
public:
    explicit ChecksumBuffer(std::streambuf* source, bool read_signature = false) : source_(source) {
        // Format detection has already consumed these four bytes on input.
        if (read_signature) update("BWA6", 4);
    }
    [[nodiscard]] std::uint32_t checksum() const { return ~crc_; }
};
} // namespace archive::detail
