#include <cmath>
#include <limits>

#include "test_util.hpp"

namespace bitvm::proto::testing
{
namespace
{

constexpr auto kU32 = std::numeric_limits<std::uint32_t>::max();
constexpr auto kU64 = std::numeric_limits<std::uint64_t>::max();
constexpr auto kI64 = std::numeric_limits<std::int64_t>::max();
constexpr auto kF32 = std::numeric_limits<float>::max();

std::vector<Pair>
many_pairs (std::size_t n) {
    std::vector<Pair> out;
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back (Pair{ static_cast<std::uint16_t> (i), static_cast<float> (i) * 0.5F });
    }
    return out;
}

}  // namespace

std::vector<Message>
edge_samples () {
    std::vector<Message> out;
    std::uint32_t        seq = 1;
    const auto           add = [&] (auto body) {
        out.push_back (Message{ seq++, std::move (body) });
    };

    // handshake
    add (Hello{ 0, 1, kU64, 255, kEventMask, std::string (1024, 'x') });
    add (Hello{ 0, 1, 0, 0, 0, "" });
    add (
        Welcome{ 0,
                 1,
                 0x0123456789abcdefULL,
                 255,
                 255U << 24,
                 kClientIdSize,
                 96000,
                 64,
                 8,
                 8,
                 4096,
                 1024,
                 65535,
                 kU32,
                 -1,
                 65536,
                 kI64,
                 std::string (1024, 's') }
    );
    add (Welcome{});
    add (Reject{ ErrorCode::version, "versions differ" });
    add (Reject{ ErrorCode::limit, "" });

    // bundles: every command with edge values
    Bundle all;
    all.time     = kI64;
    all.commands = {
        NodeNew{ kU32, kU32, AddAction::replace, kU32 - 1, true, many_pairs (256) },
        NodeNew{ 1, 1, AddAction::head, 0, false, {} },
        GroupNew{ 0x01000001, AddAction::tail, 0 },
        NodeFree{ { 0x01000001, kU32 } },
        NodeFree{ {} },
        NodeSet{ 0x01000002, 3600.0F, Curve::lin, { { 0, -0.0F }, { 65535, kF32 } } },
        NodeSet{ 0, 0.0F, Curve::lin, {} },
        NodeRun{ 0x01000002, false },
        NodeMove{ 0x01000002, AddAction::before, 0x01000003 },
        GroupClear{ 0 },
        CbusSet{ 0, { 0.0F, -1.5F, std::numeric_limits<float>::denorm_min() } },
        CbusSet{ kU32, {} },
    };
    add (all);
    add (Bundle{ 0, { GroupClear{ 0 } } });
    add (Bundle{ 1, { NodeRun{ 1, true } } });

    // calls
    add (Call{ Sync{} });
    add (Call{ Status{} });
    add (Call{ Tree{ 0, 0 } });
    add (Call{ Tree{ kU32, 65535 } });
    add (Call{ ClientReset{} });
    add (Call{ DefLoad{ {} } });
    add (Call{ DefLoad{ bytes_of ({ 0, 1, 2, 255 }) } });
    add (Call{ DefFree{ kU32 } });
    add (Call{ BufAlloc{ kU32, 16 } });
    add (Call{ BufLoad{ "a", 0, 0 } });
    add (Call{ BufLoad{ std::string (4095, 'p'), kU32, kU32 } });
    add (Call{ BufFree{ 0 } });
    add (Call{ BusAlloc{ BusKind::control, 65535 } });
    add (Call{ BusFree{ BusKind::audio, kU32 } });

    // replies
    add (Reply{ static_cast<std::uint16_t> (Opcode::sync), CallResult{ Empty{} } });
    add (Reply{ static_cast<std::uint16_t> (Opcode::client_reset), CallResult{ Empty{} } });
    add (Reply{ static_cast<std::uint16_t> (Opcode::def_free), CallResult{ Empty{} } });
    add (Reply{ static_cast<std::uint16_t> (Opcode::buf_free), CallResult{ Empty{} } });
    add (Reply{ static_cast<std::uint16_t> (Opcode::bus_free), CallResult{ Empty{} } });
    add (
        Reply{ static_cast<std::uint16_t> (Opcode::status),
               CallResult{ StatusInfo{ -1,
                                       kU32,
                                       65535,
                                       0.5F,
                                       kF32,
                                       kU32,
                                       kU32,
                                       kU32,
                                       kU32,
                                       kU32,
                                       kU32,
                                       kU64,
                                       kU64,
                                       kU64,
                                       kU64,
                                       kU64 } } }
    );
    add (Reply{ static_cast<std::uint16_t> (Opcode::tree), CallResult{ TreeInfo{ false, {} } } });
    add (
        Reply{ static_cast<std::uint16_t> (Opcode::tree),
               CallResult{ TreeInfo{ true,
                                     { { 0, kNoParent, NodeKind::group, 0, true },
                                       { 0x01000001, 0, NodeKind::synth, kU32, false } } } } }
    );
    add (Reply{ static_cast<std::uint16_t> (Opcode::def_load), CallResult{ DefLoaded{ kU32 } } });
    add (Reply{ static_cast<std::uint16_t> (Opcode::buf_alloc), CallResult{ BufAllocated{ 0 } } });
    add (
        Reply{ static_cast<std::uint16_t> (Opcode::buf_load),
               CallResult{ BufLoaded{ 3, kU32, 16, 192000 } } }
    );
    add (Reply{ static_cast<std::uint16_t> (Opcode::bus_alloc), CallResult{ BusAllocated{ 2 } } });
    add (
        Reply{ static_cast<std::uint16_t> (Opcode::buf_load),
               CallError{ ErrorCode::io, "no such file" } }
    );
    add (Reply{ 0, CallError{ ErrorCode::malformed, std::string (1024, 'm') } });
    add (Reply{ 0x7777, CallError{ ErrorCode::unsupported, "" } });

    // events
    add (Event{ NEnd{ 0x01000001 } });
    add (Event{ Late{ 1, 2, kI64 } });
    add (Event{ Xrun{ kU32 } });
    add (
        Event{ ErrorEvent{ kU32,
                           kNoCommand,
                           static_cast<std::uint16_t> (Opcode::node_new),
                           ErrorCode::exists,
                           2,
                           "id in use" } }
    );
    add (Event{ ErrorEvent{ 0, 0, 0, ErrorCode::graph, 1, "" } });
    return out;
}

std::vector<Message>
canonical_samples () {
    std::vector<Message> out;
    std::uint32_t        seq = 1;
    const auto           add = [&] (auto body) {
        out.push_back (Message{ seq++, std::move (body) });
    };

    add (Hello{ 0, 1, 0x1122334455667788ULL, 0, kEventNEnd | kEventLate, "sona" });
    add (
        Welcome{ 0,
                 1,
                 0x1122334455667788ULL,
                 1,
                 0x01000000,
                 kClientIdSize,
                 48000,
                 64,
                 2,
                 0,
                 16,
                 16,
                 1024,
                 8000000,
                 50000000,
                 65536,
                 1234567890,
                 "bitvmd" }
    );
    add (Reject{ ErrorCode::version, "catalog mismatch" });
    add (Call{ Status{} });
    add (Reply{ static_cast<std::uint16_t> (Opcode::def_load), CallResult{ DefLoaded{ 1 } } });
    add (Bundle{ 0, { GroupNew{ 0x01000001, AddAction::head, 0 } } });
    add (
        Bundle{ 1500000000,
                { NodeNew{ 0x01000002,
                           1,
                           AddAction::tail,
                           0x01000001,
                           false,
                           { { 0, 440.0F }, { 1, 0.2F } } },
                  NodeSet{ 0x01000002, 0.05F, Curve::lin, { { 1, 0.0F } } },
                  NodeFree{ { 0x01000003 } } } }
    );
    add (Event{ NEnd{ 0x01000002 } });
    add (Reply{ 0, CallError{ ErrorCode::not_found, "no such def" } });
    return out;
}

}  // namespace bitvm::proto::testing
