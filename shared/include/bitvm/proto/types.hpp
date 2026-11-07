#pragma once

/// @file
/// @brief bitvm protocol v0.1: ids, enumerations and constants.
///
/// The values mirror docs/protocol.md, section numbers in the comments refer to it.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace bitvm::proto
{

/// id of a node (synth or group); a client owns the range `client << 24` (section 7)
using NodeId = std::uint32_t;

inline constexpr std::uint32_t kMagic        = 0x504D5642;  ///< bytes b v m p
inline constexpr std::uint16_t kVersionMajor = 0;
inline constexpr std::uint16_t kVersionMinor = 1;

inline constexpr std::size_t   kHeaderSize    = 12;
inline constexpr std::size_t   kMaxMessage    = 65536;
inline constexpr std::size_t   kMaxString     = 1024;
inline constexpr std::size_t   kMaxName       = 63;
inline constexpr std::size_t   kMaxPath       = 4095;
inline constexpr std::size_t   kMaxParams     = 256;  ///< pairs per node.new / node.set
inline constexpr std::uint16_t kNoCommand     = 0xFFFF;
inline constexpr std::uint32_t kNoParent      = 0xFFFFFFFF;
inline constexpr std::size_t   kMinSendBuffer = 256 * 1024;

inline constexpr NodeId        kRootGroup     = 0;
inline constexpr unsigned      kClientIdShift = 24;
inline constexpr std::uint32_t kClientIdSize  = 1U << kClientIdShift;

// graphdef (section 13)
inline constexpr std::uint32_t kGraphMagic     = 0x44475642;  ///< bytes b v g d
inline constexpr std::uint16_t kGraphVersion   = 1;
inline constexpr std::size_t   kMaxGraphParams = 256;
inline constexpr std::size_t   kMaxGraphConsts = 4096;
inline constexpr std::size_t   kMaxGraphUnits  = 4096;
inline constexpr std::size_t   kMaxUnitInputs  = 65;  ///< bus + 64 channels of `out`
inline constexpr std::size_t   kMaxUnitOutputs = 64;

// event subscription bits in `Hello::events`
inline constexpr std::uint32_t kEventNEnd = 1U << 0;
inline constexpr std::uint32_t kEventLate = 1U << 1;
inline constexpr std::uint32_t kEventXrun = 1U << 2;
inline constexpr std::uint32_t kEventMask = kEventNEnd | kEventLate | kEventXrun;

/// where a new or moved node goes relative to `target` (section 3)
enum class AddAction : std::uint8_t {
    head    = 0,  ///< first in the group `target`
    tail    = 1,  ///< last in the group `target`
    before  = 2,  ///< just before the node `target`
    after   = 3,  ///< just after the node `target`
    replace = 4,  ///< in place of the node `target`, which is freed
};

enum class BusKind : std::uint8_t {
    audio   = 0,
    control = 1,
};

/// shape of a parameter ramp; only `lin` exists in v0, other values are reserved
enum class Curve : std::uint8_t {
    lin = 0,
};

/// how often a unit is evaluated
enum class Rate : std::uint8_t {
    ir = 0,  ///< once, when the node is created
    kr = 1,  ///< once per block
    ar = 2,  ///< every frame
};

enum class NodeKind : std::uint8_t {
    group = 0,
    synth = 1,
};

/// error codes of replies, rejects and error events (section 12)
enum class ErrorCode : std::uint16_t {
    ok          = 0,
    malformed   = 1,
    unsupported = 2,
    version     = 3,
    context     = 4,
    invalid_arg = 5,
    not_found   = 6,
    exists      = 7,
    id_range    = 8,
    limit       = 9,
    io          = 10,
    format      = 11,
    graph       = 12,
    internal    = 13,
};

/// number of defined error codes; a wire value at or above it is unknown
inline constexpr std::uint16_t kErrorCodeCount = 14;

enum class MessageKind : std::uint16_t {
    hello   = 1,
    welcome = 2,
    reject  = 3,
    bundle  = 4,
    call    = 5,
    reply   = 6,
    event   = 7,
};

/// opcodes of calls (0x00xx), bundle commands (0x01xx) and events (0x80xx)
enum class Opcode : std::uint16_t {
    // calls
    sync         = 0x0001,
    status       = 0x0002,
    tree         = 0x0003,
    client_reset = 0x0004,
    def_load     = 0x0010,
    def_free     = 0x0011,
    buf_alloc    = 0x0020,
    buf_load     = 0x0021,
    buf_free     = 0x0022,
    bus_alloc    = 0x0030,
    bus_free     = 0x0031,
    // bundle (rt) commands
    node_new    = 0x0100,
    group_new   = 0x0101,
    node_free   = 0x0102,
    node_set    = 0x0103,
    node_run    = 0x0104,
    node_move   = 0x0105,
    group_clear = 0x0106,
    cbus_set    = 0x0107,
    // events
    n_end = 0x8001,
    late  = 0x8002,
    xrun  = 0x8003,
    error = 0x8004,
};

/// which kind of message may carry an opcode
enum class OpcodeClass : std::uint8_t {
    call,
    rt,
    event
};

/// @brief class of a raw opcode.
/// @return nullopt if the opcode is not in the tables
[[nodiscard]] std::optional<OpcodeClass> classify (std::uint16_t opcode) noexcept;

// Names of enumeration values.
//
// @note These names are the protocol's own: the text form (section 15) and the error code
// table (section 12) fix them, and the text round trip and the golden files rely on every
// build printing the same word for the same value. They are therefore plain functions over
// one table per type and not a replaceable formatter. How much information a printed
// message carries is decided one level up, in the text module.

/// @brief name of an enumeration value as written in the text form.
/// @return the name, or an empty view if the value is not in the table
/// @{
[[nodiscard]] std::string_view name_of (AddAction value) noexcept;
[[nodiscard]] std::string_view name_of (BusKind value) noexcept;
[[nodiscard]] std::string_view name_of (Curve value) noexcept;
[[nodiscard]] std::string_view name_of (Rate value) noexcept;
[[nodiscard]] std::string_view name_of (NodeKind value) noexcept;
[[nodiscard]] std::string_view name_of (ErrorCode value) noexcept;
[[nodiscard]] std::string_view name_of (MessageKind value) noexcept;
[[nodiscard]] std::string_view name_of (Opcode value) noexcept;
/// @}

/// @brief value of the enumeration with the given text name.
/// @return nullopt if there is no such name
/// @{
[[nodiscard]] std::optional<AddAction> add_action_from_name (std::string_view name) noexcept;
[[nodiscard]] std::optional<BusKind>   bus_kind_from_name (std::string_view name) noexcept;
[[nodiscard]] std::optional<Curve>     curve_from_name (std::string_view name) noexcept;
[[nodiscard]] std::optional<Rate>      rate_from_name (std::string_view name) noexcept;
[[nodiscard]] std::optional<NodeKind>  node_kind_from_name (std::string_view name) noexcept;
[[nodiscard]] std::optional<ErrorCode> error_code_from_name (std::string_view name) noexcept;
[[nodiscard]] std::optional<Opcode>    opcode_from_name (std::string_view name) noexcept;
/// @}

/// @brief range check of a raw wire value.
/// @return the enumeration value, or nullopt if the number is outside the table
/// @{
[[nodiscard]] std::optional<AddAction> add_action_from_wire (std::uint8_t value) noexcept;
[[nodiscard]] std::optional<BusKind>   bus_kind_from_wire (std::uint8_t value) noexcept;
[[nodiscard]] std::optional<Rate>      rate_from_wire (std::uint8_t value) noexcept;
[[nodiscard]] std::optional<NodeKind>  node_kind_from_wire (std::uint8_t value) noexcept;
[[nodiscard]] std::optional<ErrorCode> error_code_from_wire (std::uint16_t value) noexcept;
/// @}

/// @brief true if `name` matches `[A-Za-z_][A-Za-z0-9_]*` and has 1..kMaxName bytes
[[nodiscard]] bool is_valid_name (std::string_view name) noexcept;

}  // namespace bitvm::proto
