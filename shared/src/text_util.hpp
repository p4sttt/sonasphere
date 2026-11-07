#pragma once

// internal helpers of the text form: tokenizer, number parsing and formatting.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "bitvm/proto/text.hpp"

namespace bitvm::proto::text
{

struct Token {
    std::string raw;  // quotes and escapes kept
    std::size_t col = 1;
};

// splits a line into whitespace separated tokens; quoted parts may hold spaces; `#` outside
// quotes starts a comment
[[nodiscard]] std::expected<std::vector<Token>, TextError>
tokenize (std::string_view line, std::size_t line_no);

// value of a str field: a bare word or a quoted string with escapes. on failure returns
// the message and the byte offset (inside `raw`) of the problem.
struct UnquoteError {
    std::string message;
    std::size_t offset = 0;
};

[[nodiscard]] std::expected<std::string, UnquoteError> unquote (std::string_view raw);

// decimal or 0x-prefixed unsigned integer, no sign
[[nodiscard]] bool parse_uint (std::string_view s, std::uint64_t max, std::uint64_t& out);
// decimal (optionally negative) or 0x-prefixed integer
[[nodiscard]] bool parse_i64 (std::string_view s, std::int64_t& out);
// finite decimal number: 440, -1.5, 1e-3. nan, inf and out of range are refused.
[[nodiscard]] bool parse_f32 (std::string_view s, float& out);
[[nodiscard]] bool parse_f64 (std::string_view s, double& out);

[[nodiscard]] std::vector<std::string_view> split (std::string_view s, char sep);

[[nodiscard]] std::string format_f32 (float v);
[[nodiscard]] std::string format_hex32 (std::uint32_t v);
[[nodiscard]] std::string format_hex64 (std::uint64_t v);
[[nodiscard]] std::string format_hex_opcode (std::uint16_t v);
// bare word when possible, else quoted with escapes
[[nodiscard]] std::string format_str (std::string_view s);

}  // namespace bitvm::proto::text
