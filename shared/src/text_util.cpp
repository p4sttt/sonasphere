#include "text_util.hpp"

#include <array>
#include <charconv>
#include <cmath>

namespace bitvm::proto::text
{
namespace
{

bool
is_space (char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

int
hex_digit (char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool
is_bare_char (char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '.' || c == '/' || c == ':' || c == '+' || c == '-' || c == '@';
}

// length of a structurally valid utf-8 sequence starting at s[i], or 0
std::size_t
utf8_sequence (std::string_view s, std::size_t i) {
    const auto  lead = static_cast<unsigned char> (s[i]);
    std::size_t len  = 0;
    if (lead >= 0xC2 && lead <= 0xDF) {
        len = 2;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        len = 3;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        len = 4;
    } else {
        return 0;
    }
    if (i + len > s.size()) {
        return 0;
    }
    for (std::size_t k = 1; k < len; ++k) {
        if ((static_cast<unsigned char> (s[i + k]) & 0xC0) != 0x80) {
            return 0;
        }
    }
    return len;
}

}  // namespace

std::expected<std::vector<Token>, TextError>
tokenize (std::string_view line, std::size_t line_no) {
    std::vector<Token> tokens;
    std::size_t        i = 0;
    while (i < line.size()) {
        if (is_space (line[i])) {
            ++i;
            continue;
        }
        if (line[i] == '#') {
            break;
        }
        Token token;
        token.col            = i + 1;
        bool        in_quote = false;
        std::size_t quote    = 0;
        while (i < line.size() && (in_quote || (!is_space (line[i]) && line[i] != '#'))) {
            const char c = line[i];
            if (in_quote && c == '\\' && i + 1 < line.size()) {
                token.raw += c;
                token.raw += line[i + 1];
                i += 2;
                continue;
            }
            if (c == '"') {
                in_quote = !in_quote;
                quote    = i;
            }
            token.raw += c;
            ++i;
        }
        if (in_quote) {
            return std::unexpected (TextError{ line_no, quote + 1, "unterminated string" });
        }
        tokens.push_back (std::move (token));
    }
    return tokens;
}

std::expected<std::string, UnquoteError>
unquote (std::string_view raw) {
    std::string out;
    if (raw.empty() || raw.front() != '"') {
        for (std::size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == '"') {
                return std::unexpected (UnquoteError{ "quote inside a bare word", i });
            }
        }
        return std::string (raw);
    }
    std::size_t i = 1;
    while (i < raw.size()) {
        const char c = raw[i];
        if (c == '"') {
            if (i + 1 != raw.size()) {
                return std::unexpected (UnquoteError{ "text after the closing quote", i + 1 });
            }
            return out;
        }
        if (c != '\\') {
            out += c;
            ++i;
            continue;
        }
        if (i + 1 >= raw.size()) {
            break;
        }
        const char e = raw[i + 1];
        switch (e) {
            case '"': {
                out += '"';
                i += 2;
                break;
            }
            case '\\': {
                out += '\\';
                i += 2;
                break;
            }
            case 'n': {
                out += '\n';
                i += 2;
                break;
            }
            case 't': {
                out += '\t';
                i += 2;
                break;
            }
            case 'x': {
                const int hi = i + 2 < raw.size() ? hex_digit (raw[i + 2]) : -1;
                const int lo = i + 3 < raw.size() ? hex_digit (raw[i + 3]) : -1;
                if (hi < 0 || lo < 0) {
                    return std::unexpected (UnquoteError{ "bad \\x escape", i });
                }
                out += static_cast<char> (hi * 16 + lo);
                i += 4;
                break;
            }
            default: {
                return std::unexpected (UnquoteError{ "unknown escape", i });
            }
        }
    }
    return std::unexpected (UnquoteError{ "unterminated string", raw.size() });
}

bool
parse_uint (std::string_view s, std::uint64_t max, std::uint64_t& out) {
    int base = 10;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s.remove_prefix (2);
    }
    if (s.empty()) {
        return false;
    }
    std::uint64_t v   = 0;
    const auto    res = std::from_chars (s.data(), s.data() + s.size(), v, base);
    if (res.ec != std::errc{} || res.ptr != s.data() + s.size() || v > max) {
        return false;
    }
    out = v;
    return true;
}

bool
parse_i64 (std::string_view s, std::int64_t& out) {
    bool negative = false;
    if (!s.empty() && s[0] == '-') {
        negative = true;
        s.remove_prefix (1);
    }
    std::uint64_t mag = 0;
    if (!parse_uint (s, negative ? (1ULL << 63) : INT64_MAX, mag)) {
        return false;
    }
    out = negative ? static_cast<std::int64_t> (0 - mag) : static_cast<std::int64_t> (mag);
    return true;
}

namespace
{

// decimal numbers only: a digit or a dot must follow the optional minus sign
bool
looks_numeric (std::string_view s) {
    if (!s.empty() && s[0] == '-') {
        s.remove_prefix (1);
    }
    return !s.empty() && ((s[0] >= '0' && s[0] <= '9') || s[0] == '.');
}

}  // namespace

bool
parse_f32 (std::string_view s, float& out) {
    if (!looks_numeric (s)) {
        return false;
    }
    float      v   = 0.0F;
    const auto res = std::from_chars (s.data(), s.data() + s.size(), v);
    if (res.ec != std::errc{} || res.ptr != s.data() + s.size() || !std::isfinite (v)) {
        return false;
    }
    out = v;
    return true;
}

bool
parse_f64 (std::string_view s, double& out) {
    if (!looks_numeric (s)) {
        return false;
    }
    double     v   = 0.0;
    const auto res = std::from_chars (s.data(), s.data() + s.size(), v);
    if (res.ec != std::errc{} || res.ptr != s.data() + s.size() || !std::isfinite (v)) {
        return false;
    }
    out = v;
    return true;
}

std::vector<std::string_view>
split (std::string_view s, char sep) {
    std::vector<std::string_view> parts;
    std::size_t                   start = 0;
    while (true) {
        const auto pos = s.find (sep, start);
        if (pos == std::string_view::npos) {
            parts.push_back (s.substr (start));
            return parts;
        }
        parts.push_back (s.substr (start, pos - start));
        start = pos + 1;
    }
}

std::string
format_f32 (float v) {
    std::array<char, 32> buf{};
    const auto           res = std::to_chars (buf.data(), buf.data() + buf.size(), v);
    return std::string (buf.data(), res.ptr);
}

std::string
format_hex32 (std::uint32_t v) {
    static constexpr std::string_view kDigits = "0123456789abcdef";
    std::string                       s       = "0x00000000";
    for (std::size_t i = 0; i < 8; ++i) {
        s[9 - i] = kDigits[(v >> (4 * i)) & 0xF];
    }
    return s;
}

std::string
format_hex64 (std::uint64_t v) {
    static constexpr std::string_view kDigits = "0123456789abcdef";
    std::string                       s       = "0x0000000000000000";
    for (std::size_t i = 0; i < 16; ++i) {
        s[17 - i] = kDigits[(v >> (4 * i)) & 0xF];
    }
    return s;
}

std::string
format_hex_opcode (std::uint16_t v) {
    static constexpr std::string_view kDigits = "0123456789abcdef";
    std::string                       s       = "0x0000";
    for (std::size_t i = 0; i < 4; ++i) {
        s[5 - i] = kDigits[(v >> (4 * i)) & 0xF];
    }
    return s;
}

std::string
format_str (std::string_view s) {
    bool bare = !s.empty();
    for (const char c : s) {
        if (!is_bare_char (c)) {
            bare = false;
            break;
        }
    }
    if (bare) {
        return std::string (s);
    }
    static constexpr std::string_view kDigits = "0123456789abcdef";
    std::string                       out     = "\"";
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        const auto u = static_cast<unsigned char> (c);
        if (c == '"') {
            out += "\\\"";
        } else if (c == '\\') {
            out += "\\\\";
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\t') {
            out += "\\t";
        } else if (u < 0x20 || u == 0x7F) {
            out += "\\x";
            out += kDigits[u >> 4];
            out += kDigits[u & 0xF];
        } else if (u >= 0x80) {
            const auto len = utf8_sequence (s, i);
            if (len == 0) {
                out += "\\x";
                out += kDigits[u >> 4];
                out += kDigits[u & 0xF];
            } else {
                out.append (s.substr (i, len));
                i += len - 1;
            }
        } else {
            out += c;
        }
    }
    out += '"';
    return out;
}

}  // namespace bitvm::proto::text
