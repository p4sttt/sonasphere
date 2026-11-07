#pragma once

/// @file
/// @brief Plain data for every bitvm message: handshake, bundles, calls, replies and events.
///
/// Responsibility: describe what can be said, nothing more. The structures are aggregates
/// without behaviour; turning them into bytes is `codec.hpp`, into text `text.hpp`, checking
/// their meaning `validate.hpp`. That keeps each module to one reason to change and lets sona
/// and bitvm build or inspect messages without pulling in any of the machinery.
///
/// Fixed parts of the wire format (magic, length, flags, command opcode and size) are not
/// stored: the codec derives them. Section numbers refer to docs/protocol.md.

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "bitvm/proto/types.hpp"

namespace bitvm::proto
{

/// @brief value of one node parameter (`pair`, section 3).
///
/// A parameter is addressed by its index in the GraphDef, names exist only in the text form.
struct Pair {
    std::uint16_t index = 0;
    float         value = 0.0F;

    bool operator== (const Pair&) const = default;
};

// ---- bundle (rt) commands ----

/// @brief `node.new` (0x0100, section 8): create a synth from a GraphDef.
///
/// Use: start a note or an effect. `params` overrides the defaults of the GraphDef, anything
/// not listed keeps its default.
struct NodeNew {
    NodeId            id     = 0;
    std::uint32_t     def    = 0;
    AddAction         add    = AddAction::head;
    NodeId            target = kRootGroup;
    bool              paused = false;  ///< `flags` bit 0: create on pause
    std::vector<Pair> params;

    bool operator== (const NodeNew&) const = default;
};

/// @brief `group.new` (0x0101, section 8): create an empty group.
///
/// Use: groups fix the order of execution, effects go after the sources they read.
struct GroupNew {
    NodeId    id     = 0;
    AddAction add    = AddAction::head;
    NodeId    target = kRootGroup;

    bool operator== (const GroupNew&) const = default;
};

/// @brief `node.free` (0x0102, section 8): free nodes; a group goes with its subtree.
struct NodeFree {
    std::vector<NodeId> ids;

    bool operator== (const NodeFree&) const = default;
};

/// @brief `node.set` (0x0103, section 8): move parameters of a synth to new values.
///
/// With `ramp` > 0 the change takes that many seconds, a new set cancels an unfinished ramp of
/// the same parameter. When an index repeats in `params`, the last one wins.
struct NodeSet {
    NodeId            id    = 0;
    float             ramp  = 0.0F;
    Curve             curve = Curve::lin;
    std::vector<Pair> params;

    bool operator== (const NodeSet&) const = default;
};

/// @brief `node.run` (0x0104, section 8): pause or resume a node; a group, its whole subtree.
struct NodeRun {
    NodeId id  = 0;
    bool   run = true;

    bool operator== (const NodeRun&) const = default;
};

/// @brief `node.move` (0x0105, section 8): move a synth or a group; `replace` is not allowed.
struct NodeMove {
    NodeId    id     = 0;
    AddAction add    = AddAction::head;
    NodeId    target = kRootGroup;

    bool operator== (const NodeMove&) const = default;
};

/// @brief `group.clear` (0x0106, section 8): free everything in a group, the group stays.
///
/// `group.clear` of the root group (id 0) is the "hush" of a live session.
struct GroupClear {
    NodeId id = 0;

    bool operator== (const GroupClear&) const = default;
};

/// @brief `cbus.set` (0x0107, section 8): write values to consecutive control buses.
struct CbusSet {
    std::uint32_t      index = 0;
    std::vector<float> values;

    bool operator== (const CbusSet&) const = default;
};

/// @brief any command allowed in a bundle (section 8).
using RtCommand =
    std::variant<NodeNew, GroupNew, NodeFree, NodeSet, NodeRun, NodeMove, GroupClear, CbusSet>;

// ---- calls ----

/// @brief `sync` (0x0001, section 10): reply once all earlier work of this client is done.
struct Sync {
    bool operator== (const Sync&) const = default;
};

/// @brief `status` (0x0002, section 10): ask for the counters of the server, see StatusInfo.
struct Status {
    bool operator== (const Status&) const = default;
};

/// @brief `tree` (0x0003, section 10): list the node tree below `root`.
///
/// `depth` 0 means no limit. Answered with TreeInfo.
struct Tree {
    NodeId        root  = kRootGroup;
    std::uint16_t depth = 0;

    bool operator== (const Tree&) const = default;
};

/// @brief `client.reset` (0x0004, section 10): free every node and resource of this client.
struct ClientReset {
    bool operator== (const ClientReset&) const = default;
};

/// @brief `def.load` (0x0010, section 10): load a GraphDef.
///
/// `graph` is the binary form from `graphdef.hpp`. Answered with DefLoaded or the error
/// `graph` when the validator refuses it.
struct DefLoad {
    std::vector<std::byte> graph;

    bool operator== (const DefLoad&) const = default;
};

/// @brief `def.free` (0x0011, section 10): drop a GraphDef; running nodes finish normally.
struct DefFree {
    std::uint32_t def = 0;

    bool operator== (const DefFree&) const = default;
};

/// @brief `buf.alloc` (0x0020, section 10): allocate a zeroed buffer. Answered with BufAllocated.
struct BufAlloc {
    std::uint32_t frames   = 0;
    std::uint16_t channels = 0;

    bool operator== (const BufAlloc&) const = default;
};

/// @brief `buf.load` (0x0021, section 10): load an audio file into a buffer.
///
/// `frames` 0 reads to the end of the file. Asynchronous: the reply may arrive out of order.
/// Answered with BufLoaded.
struct BufLoad {
    std::string   path;
    std::uint32_t start  = 0;
    std::uint32_t frames = 0;

    bool operator== (const BufLoad&) const = default;
};

/// @brief `buf.free` (0x0022, section 10): free a buffer; readers get silence.
struct BufFree {
    std::uint32_t buf = 0;

    bool operator== (const BufFree&) const = default;
};

/// @brief `bus.alloc` (0x0030, section 10): allocate `count` consecutive buses.
///
/// Answered with BusAllocated.
struct BusAlloc {
    BusKind       kind  = BusKind::audio;
    std::uint16_t count = 0;

    bool operator== (const BusAlloc&) const = default;
};

/// @brief `bus.free` (0x0031, section 10): free the whole range that was allocated at `base`.
struct BusFree {
    BusKind       kind = BusKind::audio;
    std::uint32_t base = 0;

    bool operator== (const BusFree&) const = default;
};

/// @brief any call (section 10).
using CallCommand = std::variant<
    Sync,
    Status,
    Tree,
    ClientReset,
    DefLoad,
    DefFree,
    BufAlloc,
    BufLoad,
    BufFree,
    BusAlloc,
    BusFree>;

// ---- call results ----

// result of sync, client.reset, def.free, buf.free, bus.free
/// @brief result of `sync`, `client.reset`, `def.free`, `buf.free` and `bus.free`: nothing.
struct Empty {
    bool operator== (const Empty&) const = default;
};

/// @brief result of `status` (section 10): counters of the server.
///
/// `cpu_avg` is the mean load of the last second, `cpu_peak` the peak since the previous
/// status. All counters are server-wide except `events_dropped`, which is per client.
struct StatusInfo {
    std::int64_t  now_ns            = 0;
    std::uint32_t sample_rate       = 0;
    std::uint16_t block_size        = 0;
    float         cpu_avg           = 0.0F;
    float         cpu_peak          = 0.0F;
    std::uint32_t nodes             = 0;
    std::uint32_t groups            = 0;
    std::uint32_t defs              = 0;
    std::uint32_t buffers           = 0;
    std::uint32_t rt_queue_used     = 0;
    std::uint32_t rt_queue_capacity = 0;
    std::uint64_t late_played       = 0;
    std::uint64_t late_dropped      = 0;
    std::uint64_t skipped           = 0;
    std::uint64_t xruns             = 0;
    std::uint64_t events_dropped    = 0;

    bool operator== (const StatusInfo&) const = default;
};

/// @brief one node in the result of `tree`; the root of the listing has `kNoParent`.
struct TreeEntry {
    NodeId        id      = 0;
    std::uint32_t parent  = kNoParent;
    NodeKind      kind    = NodeKind::group;
    std::uint32_t def     = 0;
    bool          running = true;

    bool operator== (const TreeEntry&) const = default;
};

/// @brief result of `tree` (section 10), in execution order, the requested root first.
///
/// `truncated` is set when the tree did not fit into one message.
struct TreeInfo {
    bool                   truncated = false;
    std::vector<TreeEntry> entries;

    bool operator== (const TreeInfo&) const = default;
};

/// @brief result of `def.load`: the number of the new GraphDef (from 1, never reused).
struct DefLoaded {
    std::uint32_t def = 0;

    bool operator== (const DefLoaded&) const = default;
};

/// @brief result of `buf.alloc`: the buffer number.
struct BufAllocated {
    std::uint32_t buf = 0;

    bool operator== (const BufAllocated&) const = default;
};

/// @brief result of `buf.load`: the buffer and the format of the file (no resampling).
struct BufLoaded {
    std::uint32_t buf         = 0;
    std::uint32_t frames      = 0;
    std::uint16_t channels    = 0;
    std::uint32_t sample_rate = 0;

    bool operator== (const BufLoaded&) const = default;
};

/// @brief result of `bus.alloc`: the first bus of the range.
struct BusAllocated {
    std::uint32_t base = 0;

    bool operator== (const BusAllocated&) const = default;
};

/// @brief result of a successful call; which alternative depends on the call.
using CallResult =
    std::variant<Empty, StatusInfo, TreeInfo, DefLoaded, BufAllocated, BufLoaded, BusAllocated>;

/// @brief failed outcome of a call (section 10): an error code and an explanation.
struct CallError {
    ErrorCode   code = ErrorCode::internal;  ///< never `ok`
    std::string message;

    bool operator== (const CallError&) const = default;
};

// ---- events ----

/// @brief `n_end` (0x8001, section 11): a node left the tree, for whatever reason.
struct NEnd {
    NodeId id = 0;

    bool operator== (const NEnd&) const = default;
};

/// @brief `late` (0x8002, section 11): summary of bundles that came after their time.
///
/// Use: sona raises its lookahead when this arrives.
struct Late {
    std::uint32_t played      = 0;
    std::uint32_t dropped     = 0;
    std::int64_t  max_late_ns = 0;

    bool operator== (const Late&) const = default;
};

/// @brief `xrun` (0x8003, section 11): the audio device ran dry `count` times.
struct Xrun {
    std::uint32_t count = 0;

    bool operator== (const Xrun&) const = default;
};

/// @brief `error` (0x8004, section 11): a bundle was refused or aborted.
///
/// `level` is 1 for checks before execution and 2 for failures during it (section 9).
/// `index` is the command in the bundle, `kNoCommand` if the error is not about one command.
struct ErrorEvent {
    std::uint32_t bundle_seq = 0;
    std::uint16_t index      = kNoCommand;
    std::uint16_t opcode     = 0;
    ErrorCode     code       = ErrorCode::internal;
    std::uint8_t  level      = 1;
    std::string   message;

    bool operator== (const ErrorEvent&) const = default;
};

/// @brief any event (section 11).
using EventData = std::variant<NEnd, Late, Xrun, ErrorEvent>;

// ---- messages ----

/// @brief `HELLO` (kind 1, section 5): the first message of a client.
///
/// Use: states the protocol version and catalog hash the client was built with, which client
/// number it wants (0 = any) and which events it subscribes to. `magic` is not stored.
struct Hello {
    std::uint16_t major            = kVersionMajor;
    std::uint16_t minor            = kVersionMinor;
    std::uint64_t catalog_hash     = 0;
    std::uint8_t  requested_client = 0;
    std::uint32_t events           = 0;  ///< `kEvent*` bits
    std::string   name;

    bool operator== (const Hello&) const = default;
};

/// @brief `WELCOME` (kind 2, section 5): the server accepts the client.
///
/// Use: carries the id range of the client and the parameters of the audio setup that the
/// client needs for planning (latency, tolerance, bus counts, message limit).
struct Welcome {
    std::uint16_t major             = kVersionMajor;
    std::uint16_t minor             = kVersionMinor;
    std::uint64_t catalog_hash      = 0;
    std::uint8_t  client_id         = 0;
    std::uint32_t id_base           = 0;
    std::uint32_t id_count          = kClientIdSize;
    std::uint32_t sample_rate       = 0;
    std::uint16_t block_size        = 64;
    std::uint16_t out_channels      = 0;
    std::uint16_t in_channels       = 0;
    std::uint32_t audio_buses       = 0;
    std::uint32_t control_buses     = 0;
    std::uint32_t max_buffers       = 0;
    std::int64_t  output_latency_ns = 0;
    std::int64_t  late_tolerance_ns = 0;
    std::uint32_t max_message       = static_cast<std::uint32_t> (kMaxMessage);
    std::int64_t  server_now_ns     = 0;
    std::string   server;

    bool operator== (const Welcome&) const = default;
};

/// @brief `REJECT` (kind 3, section 5): the server refuses the client and closes.
struct Reject {
    ErrorCode   code = ErrorCode::internal;
    std::string message;

    bool operator== (const Reject&) const = default;
};

/// @brief `BUNDLE` (kind 4, section 4): real-time commands that run in one frame.
///
/// Use: the only way to make sound. `time` is an absolute CLOCK_MONOTONIC time in ns when the
/// first sample should be heard, 0 means as soon as possible (section 6).
struct Bundle {
    std::int64_t           time = 0;  ///< CLOCK_MONOTONIC ns, 0 = as soon as possible
    std::vector<RtCommand> commands;

    bool operator== (const Bundle&) const = default;
};

/// @brief `CALL` (kind 5, section 4): one non-real-time command that gets a Reply.
struct Call {
    CallCommand command;

    bool operator== (const Call&) const = default;
};

/// @brief `REPLY` (kind 6, section 4): the answer to a call.
///
/// `outcome` holds the result for a successful call (it must match `opcode`, see
/// result_matches) or a CallError.
struct Reply {
    std::uint16_t opcode = 0;  ///< opcode of the call; 0 if it could not be read
    std::variant<CallResult, CallError> outcome;

    bool operator== (const Reply&) const = default;
};

/// @brief `EVENT` (kind 7, section 4): the server tells the client something happened.
struct Event {
    EventData data;

    bool operator== (const Event&) const = default;
};

/// @brief the body of a message; the alternative order matches MessageKind 1..7.
using MessageBody = std::variant<Hello, Welcome, Reject, Bundle, Call, Reply, Event>;

/// @brief any message: the sequence number of the header and one body.
///
/// `seq` is chosen by the client for its messages; replies repeat it, events carry a per
/// connection counter (section 4).
struct Message {
    std::uint32_t seq = 0;
    MessageBody   body;

    bool operator== (const Message&) const = default;
};

[[nodiscard]] MessageKind kind_of (const MessageBody& body) noexcept;
[[nodiscard]] Opcode      opcode_of (const RtCommand& command) noexcept;
[[nodiscard]] Opcode      opcode_of (const CallCommand& command) noexcept;
[[nodiscard]] Opcode      opcode_of (const EventData& event) noexcept;

/// @brief kind of a message body.
[[nodiscard]] MessageKind kind_of (const MessageBody& body) noexcept;

/// @brief opcode of a command or an event.
/// @{
[[nodiscard]] Opcode opcode_of (const RtCommand& command) noexcept;
[[nodiscard]] Opcode opcode_of (const CallCommand& command) noexcept;
[[nodiscard]] Opcode opcode_of (const EventData& event) noexcept;
/// @}

/// @brief true if `result` is the result type of the call with this opcode.
/// @param call_opcode raw opcode of the call; unknown opcodes never match
[[nodiscard]] bool result_matches (std::uint16_t call_opcode, const CallResult& result) noexcept;

/// @brief true for the kinds a client may send to a server (hello, bundle, call).
[[nodiscard]] bool is_client_message (MessageKind kind) noexcept;

}  // namespace bitvm::proto
