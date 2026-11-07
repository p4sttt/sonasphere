#pragma once

/// @file
/// @brief Binary codec: `Message` <-> bytes of one packet (docs/protocol.md sections 3-5, 8, 10,
/// 11).
///
/// Responsibility: the wire format and nothing else. `decode` never reads outside the buffer it
/// is given and never throws; `encode` refuses everything `decode` would refuse, so
/// `decode(encode(m)) == m` and `encode(decode(b)) == b` hold.
///
/// @par Design notes
/// - A codec is a pair of free functions over the shared `Message` model. The model is the common
///   representation: the text form (`text.hpp`) is a second codec over the same structures, so
///   formats are already interchangeable without a codec class or an interface. A decoder of
///   one protocol version combined with an encoder of another is not wanted: until 1.0 client
///   and server must have the same version (docs/protocol.md, top). If compatibility rules for
///   several versions ever appear, the versioned entry points can be added next to these.
/// - Bytes are `std::vector<std::byte>` for output and `std::span<const std::byte>` for input.
///   A packet is a sequence of whole bytes with a length that is known only at run time, and the
///   socket calls take exactly that. `std::bitset` has a compile-time size and no byte access,
///   a dynamic bit vector would be bit packed, and the protocol has no field below a byte.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

#include "bitvm/proto/messages.hpp"
#include "bitvm/proto/types.hpp"

namespace bitvm::proto
{

/// @brief why a packet did not decode, and where.
struct DecodeError {
    ErrorCode        code          = ErrorCode::malformed;
    std::size_t      offset        = 0;           ///< start of the offending field or command
    std::uint16_t    command_index = kNoCommand;  ///< command in the bundle, `kNoCommand` if n/a
    std::string_view detail;                      ///< static text, never owned
};

/// @brief why a message did not encode (it would not decode back, or it is too large).
struct EncodeError {
    std::string_view detail;  ///< static text, never owned
};

/// @brief the 12 byte header of a packet after the checks of decode_header().
struct Header {
    MessageKind   kind = MessageKind::hello;
    std::uint32_t seq  = 0;
};

/// @brief one command of a bundle, call or event: opcode and the bytes of its fields.
struct CommandFrame {
    std::uint16_t              opcode = 0;
    std::size_t                offset = 0;  ///< where the frame starts in the message
    std::span<const std::byte> fields;      ///< without the 4 byte opcode and size prefix
};

/// @brief encodes a message into the bytes of one packet.
/// @return the packet, or the reason the message cannot be sent (a field out of range, a string
///         too long, a result that does not fit the call, more than 65536 bytes)
[[nodiscard]] std::expected<std::vector<std::byte>, EncodeError> encode (const Message& message);

/// @brief decodes one packet; `bytes` must be exactly one message.
/// @return the message, or the first problem found (code, offset and bundle command index)
[[nodiscard]] std::expected<Message, DecodeError> decode (std::span<const std::byte> bytes);

/// @brief checks only the header: length 12..65536 and equal to `bytes.size()`, known kind,
///        flags 0.
[[nodiscard]] std::expected<Header, DecodeError> decode_header (std::span<const std::byte> bytes);

/// @brief splits the payload of a message into command frames.
/// @param payload the commands: after `time` in a bundle, the whole payload in a call or event
/// @param base offset of `payload[0]` in the message, used for error offsets
/// @return the frames, or the first frame that is cut short or has a size below 4
[[nodiscard]] std::expected<std::vector<CommandFrame>, DecodeError>
split_frames (std::span<const std::byte> payload, std::size_t base);

/// @brief decodes one frame; its opcode must belong to the class of the function (else
///        `context`, or `unsupported` for an unknown opcode).
/// @param index stored in DecodeError::command_index
/// @{
[[nodiscard]] std::expected<RtCommand, DecodeError>
decode_rt_command (const CommandFrame& frame, std::uint16_t index = kNoCommand);

[[nodiscard]] std::expected<CallCommand, DecodeError>
decode_call_command (const CommandFrame& frame, std::uint16_t index = kNoCommand);

[[nodiscard]] std::expected<EventData, DecodeError>
decode_event (const CommandFrame& frame, std::uint16_t index = kNoCommand);
/// @}

/// @brief appends one command, opcode and size prefix included, to `out`.
/// @details on failure `out` is left as it was.
/// @{
[[nodiscard]] std::expected<void, EncodeError>
encode_command (const RtCommand& command, std::vector<std::byte>& out);

[[nodiscard]] std::expected<void, EncodeError>
encode_command (const CallCommand& command, std::vector<std::byte>& out);

[[nodiscard]] std::expected<void, EncodeError>
encode_command (const EventData& event, std::vector<std::byte>& out);
/// @}

}  // namespace bitvm::proto
