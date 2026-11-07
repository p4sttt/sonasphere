#pragma once

/// @file
/// @brief Text form of messages and graphs (docs/protocol.md section 15).
///
/// Responsibility: turn messages into readable, canonical text and back (assembler and
/// disassembler), for logs, `bitvmctl`, golden tests and text scores. It does no i/o: files are
/// read through TextContext::read_file. The text round trip is exact: for a canonical message
/// `binary -> text -> binary` gives the same bytes.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "bitvm/proto/graphdef.hpp"
#include "bitvm/proto/messages.hpp"

namespace bitvm::proto
{

/// @brief a text that did not parse, with the place of the problem
struct TextError {
    std::size_t line   = 0;  ///< 1-based
    std::size_t column = 0;  ///< 1-based, in bytes
    std::string message;
};

/// @brief `line:column: message`
[[nodiscard]] std::string to_string (const TextError& error);

/// @brief what the text layer needs to know that the text itself does not say.
///
/// All members are optional; without a callback the matching feature is unavailable (parameter
/// names print as indices, `graph=@file` is refused).
struct TextContext {
    /// base of `+<seconds>` times, in ns
    std::int64_t time_base_ns = 0;

    /// numbering of messages written without `/seq`; the assembler advances it
    std::uint32_t next_seq = 1;

    /// parameter names of a def, by index; nullptr when unknown
    std::function<const std::vector<std::string>*(std::uint32_t def)> param_names;

    /// def of a live node, for `node.set`; the assembler and the disassembler also learn the
    /// defs of nodes created by `node.new` in the text they process
    std::function<std::optional<std::uint32_t> (NodeId node)> node_def;

    /// reads a file for `graph=@path`; returns the contents or an error text
    std::function<std::expected<std::string, std::string> (std::string_view path)> read_file;
};

/// @brief assembles any number of messages.
/// @param text the source; comments and blank lines are skipped
/// @param ctx time base, name lookups and file loader; `next_seq` is advanced for every
///        message written without `/seq`
/// @return the messages, each of which encodes, or the first syntax error
[[nodiscard]] std::expected<std::vector<Message>, TextError>
assemble (std::string_view text, TextContext& ctx);

/// @brief canonical text of one message, ending with a newline
[[nodiscard]] std::string disassemble (const Message& message, const TextContext& ctx);

/// @brief canonical text of an encoded message.
/// @details never fails: a message that does not decode gets a `# error` comment and its
///          commands are listed as far as they can be read; a command that cannot be decoded
///          (an unknown opcode, say) is printed as `? opcode=0x... data=<hex>`.
[[nodiscard]] std::string disassemble (std::span<const std::byte> bytes, const TextContext& ctx);

/// @brief parses the `.bvg` text form of a graph (section 15); the result is canonical.
/// @details does not run the validator, rates and the rules of section 13 are checked by it
[[nodiscard]] std::expected<GraphDef, TextError> assemble_graph (std::string_view text);

/// @brief the `.bvg` text of a graph; every argument explicit, labels `%0`, `%1`, ...
[[nodiscard]] std::string disassemble_graph (const GraphDef& graph);

/// @brief standard base64 with padding, used by `graph=base64:...`
/// @{
[[nodiscard]] std::string base64_encode (std::span<const std::byte> bytes);
/// @return the bytes, or nullopt for bad characters, bad padding or non-zero padding bits
///         (so every byte string has exactly one text)
[[nodiscard]] std::optional<std::vector<std::byte>> base64_decode (std::string_view text);
/// @}

}  // namespace bitvm::proto
