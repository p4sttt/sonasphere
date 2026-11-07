#include "bitvm/proto/messages.hpp"

namespace bitvm::proto
{

MessageKind
kind_of (const MessageBody& body) noexcept {
    // alternative order matches MessageKind values 1..7
    return static_cast<MessageKind> (body.index() + 1);
}

Opcode
opcode_of (const RtCommand& command) noexcept {
    static constexpr Opcode kTable[] = {
        Opcode::node_new, Opcode::group_new, Opcode::node_free,   Opcode::node_set,
        Opcode::node_run, Opcode::node_move, Opcode::group_clear, Opcode::cbus_set,
    };
    return kTable[command.index()];
}

Opcode
opcode_of (const CallCommand& command) noexcept {
    static constexpr Opcode kTable[] = {
        Opcode::sync,     Opcode::status,    Opcode::tree,      Opcode::client_reset,
        Opcode::def_load, Opcode::def_free,  Opcode::buf_alloc, Opcode::buf_load,
        Opcode::buf_free, Opcode::bus_alloc, Opcode::bus_free,
    };
    return kTable[command.index()];
}

Opcode
opcode_of (const EventData& event) noexcept {
    static constexpr Opcode kTable[] = {
        Opcode::n_end,
        Opcode::late,
        Opcode::xrun,
        Opcode::error,
    };
    return kTable[event.index()];
}

bool
result_matches (std::uint16_t call_opcode, const CallResult& result) noexcept {
    switch (static_cast<Opcode> (call_opcode)) {
        case Opcode::sync:
        case Opcode::client_reset:
        case Opcode::def_free:
        case Opcode::buf_free:
        case Opcode::bus_free:
            return std::holds_alternative<Empty> (result);
        case Opcode::status:
            return std::holds_alternative<StatusInfo> (result);
        case Opcode::tree:
            return std::holds_alternative<TreeInfo> (result);
        case Opcode::def_load:
            return std::holds_alternative<DefLoaded> (result);
        case Opcode::buf_alloc:
            return std::holds_alternative<BufAllocated> (result);
        case Opcode::buf_load:
            return std::holds_alternative<BufLoaded> (result);
        case Opcode::bus_alloc:
            return std::holds_alternative<BusAllocated> (result);
        default:
            return false;
    }
}

bool
is_client_message (MessageKind kind) noexcept {
    return kind == MessageKind::hello || kind == MessageKind::bundle || kind == MessageKind::call;
}

}  // namespace bitvm::proto
