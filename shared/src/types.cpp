#include "bitvm/proto/types.hpp"

#include <array>
#include <utility>

namespace bitvm::proto
{
namespace
{

template <typename E>
struct NameEntry {
    E                value;
    std::string_view name;
};

constexpr std::array kAddActions = {
    NameEntry<AddAction>{ AddAction::head, "head" },
    NameEntry<AddAction>{ AddAction::tail, "tail" },
    NameEntry<AddAction>{ AddAction::before, "before" },
    NameEntry<AddAction>{ AddAction::after, "after" },
    NameEntry<AddAction>{ AddAction::replace, "replace" },
};

constexpr std::array kBusKinds = {
    NameEntry<BusKind>{ BusKind::audio, "audio" },
    NameEntry<BusKind>{ BusKind::control, "control" },
};

constexpr std::array kCurves = {
    NameEntry<Curve>{ Curve::lin, "lin" },
};

constexpr std::array kRates = {
    NameEntry<Rate>{ Rate::ir, "ir" },
    NameEntry<Rate>{ Rate::kr, "kr" },
    NameEntry<Rate>{ Rate::ar, "ar" },
};

constexpr std::array kNodeKinds = {
    NameEntry<NodeKind>{ NodeKind::group, "group" },
    NameEntry<NodeKind>{ NodeKind::synth, "synth" },
};

constexpr std::array kErrorCodes = {
    NameEntry<ErrorCode>{ ErrorCode::ok, "ok" },
    NameEntry<ErrorCode>{ ErrorCode::malformed, "malformed" },
    NameEntry<ErrorCode>{ ErrorCode::unsupported, "unsupported" },
    NameEntry<ErrorCode>{ ErrorCode::version, "version" },
    NameEntry<ErrorCode>{ ErrorCode::context, "context" },
    NameEntry<ErrorCode>{ ErrorCode::invalid_arg, "invalid_arg" },
    NameEntry<ErrorCode>{ ErrorCode::not_found, "not_found" },
    NameEntry<ErrorCode>{ ErrorCode::exists, "exists" },
    NameEntry<ErrorCode>{ ErrorCode::id_range, "id_range" },
    NameEntry<ErrorCode>{ ErrorCode::limit, "limit" },
    NameEntry<ErrorCode>{ ErrorCode::io, "io" },
    NameEntry<ErrorCode>{ ErrorCode::format, "format" },
    NameEntry<ErrorCode>{ ErrorCode::graph, "graph" },
    NameEntry<ErrorCode>{ ErrorCode::internal, "internal" },
};

constexpr std::array kMessageKinds = {
    NameEntry<MessageKind>{ MessageKind::hello, "hello" },
    NameEntry<MessageKind>{ MessageKind::welcome, "welcome" },
    NameEntry<MessageKind>{ MessageKind::reject, "reject" },
    NameEntry<MessageKind>{ MessageKind::bundle, "bundle" },
    NameEntry<MessageKind>{ MessageKind::call, "call" },
    NameEntry<MessageKind>{ MessageKind::reply, "reply" },
    NameEntry<MessageKind>{ MessageKind::event, "event" },
};

constexpr std::array kOpcodes = {
    NameEntry<Opcode>{ Opcode::sync, "sync" },
    NameEntry<Opcode>{ Opcode::status, "status" },
    NameEntry<Opcode>{ Opcode::tree, "tree" },
    NameEntry<Opcode>{ Opcode::client_reset, "client.reset" },
    NameEntry<Opcode>{ Opcode::def_load, "def.load" },
    NameEntry<Opcode>{ Opcode::def_free, "def.free" },
    NameEntry<Opcode>{ Opcode::buf_alloc, "buf.alloc" },
    NameEntry<Opcode>{ Opcode::buf_load, "buf.load" },
    NameEntry<Opcode>{ Opcode::buf_free, "buf.free" },
    NameEntry<Opcode>{ Opcode::bus_alloc, "bus.alloc" },
    NameEntry<Opcode>{ Opcode::bus_free, "bus.free" },
    NameEntry<Opcode>{ Opcode::node_new, "node.new" },
    NameEntry<Opcode>{ Opcode::group_new, "group.new" },
    NameEntry<Opcode>{ Opcode::node_free, "node.free" },
    NameEntry<Opcode>{ Opcode::node_set, "node.set" },
    NameEntry<Opcode>{ Opcode::node_run, "node.run" },
    NameEntry<Opcode>{ Opcode::node_move, "node.move" },
    NameEntry<Opcode>{ Opcode::group_clear, "group.clear" },
    NameEntry<Opcode>{ Opcode::cbus_set, "cbus.set" },
    NameEntry<Opcode>{ Opcode::n_end, "n_end" },
    NameEntry<Opcode>{ Opcode::late, "late" },
    NameEntry<Opcode>{ Opcode::xrun, "xrun" },
    NameEntry<Opcode>{ Opcode::error, "error" },
};

template <typename E, std::size_t N>
std::string_view
lookup_name (const std::array<NameEntry<E>, N>& table, E value) noexcept {
    for (const auto& entry : table) {
        if (entry.value == value) {
            return entry.name;
        }
    }
    return {};
}

template <typename E, std::size_t N>
std::optional<E>
lookup_value (const std::array<NameEntry<E>, N>& table, std::string_view name) noexcept {
    for (const auto& entry : table) {
        if (entry.name == name) {
            return entry.value;
        }
    }
    return std::nullopt;
}

}  // namespace

std::optional<OpcodeClass>
classify (std::uint16_t opcode) noexcept {
    for (const auto& entry : kOpcodes) {
        if (static_cast<std::uint16_t> (entry.value) != opcode) {
            continue;
        }
        if (opcode >= 0x8000) {
            return OpcodeClass::event;
        }
        return opcode >= 0x0100 ? OpcodeClass::rt : OpcodeClass::call;
    }
    return std::nullopt;
}

std::string_view
name_of (AddAction value) noexcept {
    return lookup_name (kAddActions, value);
}

std::string_view
name_of (BusKind value) noexcept {
    return lookup_name (kBusKinds, value);
}

std::string_view
name_of (Curve value) noexcept {
    return lookup_name (kCurves, value);
}

std::string_view
name_of (Rate value) noexcept {
    return lookup_name (kRates, value);
}

std::string_view
name_of (NodeKind value) noexcept {
    return lookup_name (kNodeKinds, value);
}

std::string_view
name_of (ErrorCode value) noexcept {
    return lookup_name (kErrorCodes, value);
}

std::string_view
name_of (MessageKind value) noexcept {
    return lookup_name (kMessageKinds, value);
}

std::string_view
name_of (Opcode value) noexcept {
    return lookup_name (kOpcodes, value);
}

std::optional<AddAction>
add_action_from_name (std::string_view name) noexcept {
    return lookup_value (kAddActions, name);
}

std::optional<BusKind>
bus_kind_from_name (std::string_view name) noexcept {
    return lookup_value (kBusKinds, name);
}

std::optional<Curve>
curve_from_name (std::string_view name) noexcept {
    return lookup_value (kCurves, name);
}

std::optional<Rate>
rate_from_name (std::string_view name) noexcept {
    return lookup_value (kRates, name);
}

std::optional<NodeKind>
node_kind_from_name (std::string_view name) noexcept {
    return lookup_value (kNodeKinds, name);
}

std::optional<ErrorCode>
error_code_from_name (std::string_view name) noexcept {
    return lookup_value (kErrorCodes, name);
}

std::optional<Opcode>
opcode_from_name (std::string_view name) noexcept {
    return lookup_value (kOpcodes, name);
}

std::optional<AddAction>
add_action_from_wire (std::uint8_t value) noexcept {
    if (value > static_cast<std::uint8_t> (AddAction::replace)) {
        return std::nullopt;
    }
    return static_cast<AddAction> (value);
}

std::optional<BusKind>
bus_kind_from_wire (std::uint8_t value) noexcept {
    if (value > static_cast<std::uint8_t> (BusKind::control)) {
        return std::nullopt;
    }
    return static_cast<BusKind> (value);
}

std::optional<Rate>
rate_from_wire (std::uint8_t value) noexcept {
    if (value > static_cast<std::uint8_t> (Rate::ar)) {
        return std::nullopt;
    }
    return static_cast<Rate> (value);
}

std::optional<NodeKind>
node_kind_from_wire (std::uint8_t value) noexcept {
    if (value > static_cast<std::uint8_t> (NodeKind::synth)) {
        return std::nullopt;
    }
    return static_cast<NodeKind> (value);
}

std::optional<ErrorCode>
error_code_from_wire (std::uint16_t value) noexcept {
    if (value >= kErrorCodeCount) {
        return std::nullopt;
    }
    return static_cast<ErrorCode> (value);
}

bool
is_valid_name (std::string_view name) noexcept {
    if (name.empty() || name.size() > kMaxName) {
        return false;
    }
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c     = name[i];
        const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        const bool digit = c >= '0' && c <= '9';
        if (!alpha && !(digit && i > 0)) {
            return false;
        }
    }
    return true;
}

}  // namespace bitvm::proto
