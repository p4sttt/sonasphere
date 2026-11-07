#pragma once

// internal byte-level reader and writer shared by the codec and the graphdef codec.
// both keep the first failure and turn every later call into a no-op, so callers can
// decode a whole structure and check ok() once.

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "bitvm/proto/types.hpp"

namespace bitvm::proto::wire
{

struct Failure {
    ErrorCode        code   = ErrorCode::ok;
    std::size_t      offset = 0;
    std::string_view detail;
};

class Reader {
public:
    // `base` is the absolute offset of data[0] in the whole message
    Reader (std::span<const std::byte> data, std::size_t base) noexcept :
        data_ (data), base_ (base) {}

    [[nodiscard]] bool
    ok () const noexcept {
        return failure_.code == ErrorCode::ok;
    }

    [[nodiscard]] const Failure&
    failure () const noexcept {
        return failure_;
    }

    [[nodiscard]] std::size_t
    remaining () const noexcept {
        return data_.size() - pos_;
    }

    [[nodiscard]] std::size_t
    offset () const noexcept {
        return base_ + pos_;
    }

    // records a failure at the current position
    void
    fail (ErrorCode code, std::string_view detail) noexcept {
        fail_at (offset(), code, detail);
    }

    // records a failure at the start of the `n` bytes that were just read
    void
    fail_back (std::size_t n, ErrorCode code, std::string_view detail) noexcept {
        fail_at (offset() - n, code, detail);
    }

    void
    fail_at (std::size_t at, ErrorCode code, std::string_view detail) noexcept {
        if (ok()) {
            failure_ = Failure{ code, at, detail };
        }
    }

    std::uint8_t
    u8 () noexcept {
        const auto* p = take (1);
        return p == nullptr ? 0 : static_cast<std::uint8_t> (p[0]);
    }

    std::uint16_t
    u16 () noexcept {
        return static_cast<std::uint16_t> (unsigned_int (2));
    }

    std::uint32_t
    u32 () noexcept {
        return static_cast<std::uint32_t> (unsigned_int (4));
    }

    std::uint64_t
    u64 () noexcept {
        return unsigned_int (8);
    }

    std::int64_t
    i64 () noexcept {
        return static_cast<std::int64_t> (unsigned_int (8));
    }

    float
    f32 () noexcept {
        const auto bits = u32();
        const auto v    = std::bit_cast<float> (bits);
        if (ok() && !std::isfinite (v)) {
            fail_back (4, ErrorCode::invalid_arg, "nan or inf");
            return 0.0F;
        }
        return v;
    }

    bool
    boolean () noexcept {
        const auto v = u8();
        if (ok() && v > 1) {
            fail_back (1, ErrorCode::malformed, "bool is not 0 or 1");
            return false;
        }
        return v == 1;
    }

    // u16 length + bytes
    std::string
    str (std::size_t max_len = kMaxString) {
        const auto start = offset();
        const auto len   = u16();
        if (ok() && len > max_len) {
            fail_at (start, ErrorCode::malformed, "string too long");
            return {};
        }
        const auto* p = take (len);
        return p == nullptr ? std::string{} : std::string (reinterpret_cast<const char*> (p), len);
    }

    std::string
    name () {
        const auto start = offset();
        auto       s     = str (kMaxName);
        if (ok() && !is_valid_name (s)) {
            fail_at (start, ErrorCode::malformed, "invalid name");
            return {};
        }
        return s;
    }

    std::string
    path () {
        const auto start = offset();
        auto       s     = str (kMaxPath);
        if (ok() && (s.empty() || s.find ('\0') != std::string::npos)) {
            fail_at (start, ErrorCode::malformed, "invalid path");
            return {};
        }
        return s;
    }

    // u32 length + bytes
    std::vector<std::byte>
    blob () {
        const auto start = offset();
        const auto len   = u32();
        if (ok() && len > remaining()) {
            fail_at (start, ErrorCode::malformed, "truncated blob");
            return {};
        }
        const auto* p = take (len);
        if (p == nullptr) {
            return {};
        }
        return { p, p + len };
    }

private:
    const std::byte*
    take (std::size_t n) noexcept {
        if (!ok()) {
            return nullptr;
        }
        if (n > remaining()) {
            fail (ErrorCode::malformed, "truncated");
            return nullptr;
        }
        const auto* p = data_.data() + pos_;
        pos_ += n;
        return p;
    }

    std::uint64_t
    unsigned_int (std::size_t n) noexcept {
        const auto* p = take (n);
        if (p == nullptr) {
            return 0;
        }
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < n; ++i) {
            v |= static_cast<std::uint64_t> (p[i]) << (8 * i);
        }
        return v;
    }

    std::span<const std::byte> data_;
    std::size_t                base_;
    std::size_t                pos_ = 0;
    Failure                    failure_;
};

class Writer {
public:
    explicit Writer (std::vector<std::byte>& out) noexcept : out_ (out) {}

    [[nodiscard]] bool
    ok () const noexcept {
        return detail_.empty();
    }

    [[nodiscard]] std::string_view
    detail () const noexcept {
        return detail_;
    }

    [[nodiscard]] std::size_t
    size () const noexcept {
        return out_.size();
    }

    void
    fail (std::string_view detail) noexcept {
        if (ok()) {
            detail_ = detail;
        }
    }

    void
    u8 (std::uint8_t v) {
        out_.push_back (static_cast<std::byte> (v));
    }

    void
    u16 (std::uint16_t v) {
        unsigned_int (v, 2);
    }

    void
    u32 (std::uint32_t v) {
        unsigned_int (v, 4);
    }

    void
    u64 (std::uint64_t v) {
        unsigned_int (v, 8);
    }

    void
    i64 (std::int64_t v) {
        unsigned_int (static_cast<std::uint64_t> (v), 8);
    }

    void
    f32 (float v) {
        if (!std::isfinite (v)) {
            fail ("nan or inf");
            v = 0.0F;
        }
        u32 (std::bit_cast<std::uint32_t> (v));
    }

    void
    boolean (bool v) {
        u8 (v ? 1 : 0);
    }

    void
    str (std::string_view s, std::size_t max_len = kMaxString) {
        if (s.size() > max_len) {
            fail ("string too long");
            u16 (0);
            return;
        }
        u16 (static_cast<std::uint16_t> (s.size()));
        raw (s.data(), s.size());
    }

    void
    name (std::string_view s) {
        if (!is_valid_name (s)) {
            fail ("invalid name");
        }
        str (s, kMaxName);
    }

    void
    path (std::string_view s) {
        if (s.empty() || s.find ('\0') != std::string_view::npos) {
            fail ("invalid path");
        }
        str (s, kMaxPath);
    }

    void
    blob (std::span<const std::byte> b) {
        if (b.size() > kMaxMessage) {
            fail ("blob too long");
            u32 (0);
            return;
        }
        u32 (static_cast<std::uint32_t> (b.size()));
        out_.insert (out_.end(), b.begin(), b.end());
    }

    // list count (u16); records a failure when the list is too long
    void
    count (std::size_t n) {
        if (n > 0xFFFF) {
            fail ("list too long");
            n = 0;
        }
        u16 (static_cast<std::uint16_t> (n));
    }

    void
    patch_u16 (std::size_t pos, std::uint16_t v) noexcept {
        out_[pos]     = static_cast<std::byte> (v & 0xFF);
        out_[pos + 1] = static_cast<std::byte> (v >> 8);
    }

    void
    patch_u32 (std::size_t pos, std::uint32_t v) noexcept {
        for (std::size_t i = 0; i < 4; ++i) {
            out_[pos + i] = static_cast<std::byte> ((v >> (8 * i)) & 0xFF);
        }
    }

private:
    void
    raw (const char* p, std::size_t n) {
        const auto* b = reinterpret_cast<const std::byte*> (p);
        out_.insert (out_.end(), b, b + n);
    }

    void
    unsigned_int (std::uint64_t v, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            out_.push_back (static_cast<std::byte> ((v >> (8 * i)) & 0xFF));
        }
    }

    std::vector<std::byte>& out_;
    std::string_view        detail_;
};

}  // namespace bitvm::proto::wire
