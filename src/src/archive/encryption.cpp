#include <archive/encryption.hpp>
#include <sodium.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace archive {
Password::Password() {
    if (sodium_init() < 0) throw std::runtime_error("Cannot initialize libsodium.");
    data_ = static_cast<char*>(sodium_malloc(1024));
    if (!data_) throw std::bad_alloc();
    locked_ = sodium_mlock(data_, 1024) == 0;
}
Password::~Password() {
    if (locked_) sodium_munlock(data_, 1024);
    else sodium_memzero(data_, 1024);
    sodium_free(data_);
}
void Password::append(char byte) {
    if (size_ == 1024) throw std::runtime_error("Password exceeds 1024 bytes.");
    data_[size_++] = byte;
}
bool Password::equals(const Password& other) const {
    return size_ == other.size_ && sodium_memcmp(data_, other.data_, size_) == 0;
}
namespace detail {
namespace {
constexpr std::size_t chunk = 1024 * 1024, overhead = 17, header_size = 64;
using Header = std::array<unsigned char, header_size>;
static_assert(crypto_secretstream_xchacha20poly1305_ABYTES == overhead);
static_assert(crypto_secretstream_xchacha20poly1305_HEADERBYTES == 24);
static_assert(crypto_pwhash_SALTBYTES == 16);
[[noreturn]] void auth_error() { throw std::runtime_error("Wrong password or damaged archive."); }
void init() { if (sodium_init() < 0) throw std::runtime_error("Cannot initialize libsodium."); }
void put(unsigned char* out, std::uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i) out[i] = static_cast<unsigned char>(value >> (8 * i));
}
std::uint64_t get(const unsigned char* in, unsigned width) {
    std::uint64_t result = 0;
    for (unsigned i = 0; i < width; ++i) result |= std::uint64_t(in[i]) << (8 * i);
    return result;
}
void read(std::istream& in, unsigned char* out, std::size_t size) {
    in.read(reinterpret_cast<char*>(out), size);
    if (in.bad()) throw std::runtime_error("Cannot read encrypted archive.");
    if (!in) auth_error();
}
void write(std::ostream& out, const unsigned char* data, std::size_t size) {
    out.write(reinterpret_cast<const char*>(data), size);
    if (!out) throw std::runtime_error("Cannot write encrypted archive.");
}
struct State {
    crypto_secretstream_xchacha20poly1305_state value{};
    State() { init(); }
    State(const State&) = delete;
    State& operator=(const State&) = delete;
    ~State() { sodium_memzero(&value, sizeof value); }
};
struct Key {
    std::array<unsigned char, 32> value{};
    bool locked;
    Key(const Header& header, const Options& options, bool creating) {
        locked = sodium_mlock(value.data(), value.size()) == 0;
        // Constructor failures must wipe the key too.
        try {
            if (!options.password) throw std::runtime_error("A password provider is required for BWE1.");
            Password password;
            options.password(password, creating);
            auto bytes = password.bytes();
            if (bytes.empty()) throw std::runtime_error("Empty password is not allowed.");
            if (crypto_pwhash(value.data(), value.size(), bytes.data(), bytes.size(), header.data() + 24,
                             get(header.data() + 8, 8), get(header.data() + 16, 8),
                             crypto_pwhash_ALG_ARGON2ID13) != 0)
                throw std::runtime_error("Cannot allocate memory for Argon2id.");
        } catch (...) { clear(); throw; }
    }
    Key(const Key&) = delete;
    Key& operator=(const Key&) = delete;
    void clear() { if (locked) sodium_munlock(value.data(), value.size()); else sodium_memzero(value.data(), value.size()); }
    ~Key() { clear(); }
};
std::array<unsigned char, 76> aad(const Header& header, std::uint64_t index, std::uint32_t size) {
    std::array<unsigned char, 76> result{};
    std::copy(header.begin(), header.end(), result.begin());
    put(result.data() + 64, index, 8);
    put(result.data() + 72, size, 4);
    return result;
}
}
struct EncryptBuffer::Impl {
    State state;
    Header header{};
    std::ostream& out;
    std::vector<unsigned char> plain = std::vector<unsigned char>(chunk);
    std::vector<unsigned char> cipher = std::vector<unsigned char>(chunk + overhead);
    std::size_t used = 0;
    std::uint64_t index = 0;
    bool finished = false;
    Impl(std::ostream& destination, const Options& options) : out(destination) {
        std::memcpy(header.data(), "BWE1", 4);
        header[4] = header[5] = 1;
        put(header.data() + 8, 3, 8);
        put(header.data() + 16, 256 * 1024 * 1024, 8);
        randombytes_buf(header.data() + 24, 16);
        Key key(header, options, true);
        crypto_secretstream_xchacha20poly1305_init_push(&state.value, header.data() + 40, key.value.data());
        write(out, header.data(), header.size());
        frame(0, crypto_secretstream_xchacha20poly1305_TAG_MESSAGE);
    }
    ~Impl() { sodium_memzero(plain.data(), plain.size()); }
    void frame(std::size_t count, unsigned char tag) {
        if (index == std::numeric_limits<std::uint64_t>::max()) throw std::runtime_error("Too many encrypted frames.");
        const auto size = static_cast<std::uint32_t>(count + overhead);
        const auto ad = aad(header, index++, size);
        if (crypto_secretstream_xchacha20poly1305_push(&state.value, cipher.data(), nullptr,
            plain.data(), count, ad.data(), ad.size(), tag) != 0) throw std::runtime_error("Encryption failed.");
        write(out, ad.data() + 72, 4);
        write(out, cipher.data(), size);
    }
};
EncryptBuffer::EncryptBuffer(std::ostream& out, const Options& options) : impl_(std::make_unique<Impl>(out, options)) {}
EncryptBuffer::~EncryptBuffer() = default;
std::streamsize EncryptBuffer::xsputn(const char* data, std::streamsize size) {
    auto& s = *impl_;
    if (s.finished) throw std::runtime_error("Encrypted stream is finished.");
    std::streamsize done = 0;
    while (done < size) {
        const auto count = std::min<std::size_t>(size - done, chunk - s.used);
        std::memcpy(s.plain.data() + s.used, data + done, count);
        done += count; s.used += count;
        if (s.used == chunk) { s.frame(s.used, 0); s.used = 0; }
    }
    return done;
}
EncryptBuffer::int_type EncryptBuffer::overflow(int_type value) {
    if (traits_type::eq_int_type(value, traits_type::eof())) return traits_type::not_eof(value);
    const auto byte = traits_type::to_char_type(value);
    xsputn(&byte, 1);
    return value;
}
void EncryptBuffer::finish() {
    auto& s = *impl_;
    if (s.finished) throw std::runtime_error("Encrypted stream is already finished.");
    if (s.used) s.frame(s.used, 0);
    s.frame(0, crypto_secretstream_xchacha20poly1305_TAG_FINAL);
    s.finished = true;
}
struct DecryptBuffer::Impl {
    State state;
    Header header{};
    std::istream& in;
    std::vector<unsigned char> plain = std::vector<unsigned char>(chunk);
    std::vector<unsigned char> cipher = std::vector<unsigned char>(chunk + overhead);
    std::uint64_t index = 0;
    bool finished = false;
    Impl(std::istream& source, const Options& options) : in(source) {
        std::memcpy(header.data(), "BWE1", 4);
        read(in, header.data() + 4, header.size() - 4);
        if (header[4] != 1 || header[5] != 1 || header[6] || header[7])
            throw std::runtime_error("Unsupported BWE1 algorithms or flags.");
        const auto ops = get(header.data() + 8, 8), memory = get(header.data() + 16, 8);
        if (ops < 3 || ops > 10 || memory < 64 * 1024 * 1024 || memory > 1024ull * 1024 * 1024)
            throw std::runtime_error("BWE1 KDF parameters exceed allowed limits.");
        Key key(header, options, false);
        if (crypto_secretstream_xchacha20poly1305_init_pull(&state.value, header.data() + 40, key.value.data()) != 0) auth_error();
        unsigned char tag;
        if (frame(tag) != 0 || tag != 0) auth_error();
    }
    ~Impl() { sodium_memzero(plain.data(), plain.size()); }
    std::size_t frame(unsigned char& tag) {
        std::array<unsigned char, 4> length{};
        read(in, length.data(), length.size());
        const auto size = get(length.data(), 4);
        if ((index == 0 && size != overhead) || size < overhead || size > chunk + overhead || index == std::numeric_limits<std::uint64_t>::max()) auth_error();
        read(in, cipher.data(), size);
        const auto ad = aad(header, index++, size);
        unsigned long long count = 0;
        if (crypto_secretstream_xchacha20poly1305_pull(&state.value, plain.data(), &count, &tag,
            cipher.data(), size, ad.data(), ad.size()) != 0) auth_error();
        return count;
    }
};
DecryptBuffer::DecryptBuffer(std::istream& in, const Options& options) : impl_(std::make_unique<Impl>(in, options)) {}
DecryptBuffer::~DecryptBuffer() = default;
DecryptBuffer::int_type DecryptBuffer::underflow() {
    auto& s = *impl_;
    if (s.finished) return traits_type::eof();
    unsigned char tag;
    const auto count = s.frame(tag);
    if (tag == crypto_secretstream_xchacha20poly1305_TAG_FINAL) {
        if (count) auth_error();
        if (s.in.peek() != std::char_traits<char>::eof()) auth_error();
        if (s.in.bad() || !s.in.eof()) throw std::runtime_error("Cannot read encrypted archive ending.");
        s.finished = true;
        return traits_type::eof();
    }
    if (tag != 0 || !count) auth_error();
    auto* begin = reinterpret_cast<char*>(s.plain.data());
    setg(begin, begin, begin + count);
    return traits_type::to_int_type(*gptr());
}
}
}
