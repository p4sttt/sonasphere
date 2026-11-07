#include "bitvm/proto/codec.hpp"

#include <algorithm>
#include <optional>
#include <string_view>
#include <type_traits>
#include <variant>

#include "wire.hpp"

namespace bitvm::proto
{
namespace
{

using wire::Reader;
using wire::Writer;

constexpr std::size_t kPairSize      = 6;
constexpr std::size_t kTreeEntrySize = 14;

DecodeError
to_error (const wire::Failure& failure, std::uint16_t index) {
    return DecodeError{ failure.code, failure.offset, index, failure.detail };
}

std::unexpected<EncodeError>
encode_failure (const Writer& writer) {
    return std::unexpected (EncodeError{ writer.detail() });
}

// fails early when `n` elements of `size` bytes cannot fit in what is left
void
check_list (Reader& reader, std::size_t n, std::size_t size) {
    if (reader.ok() && n * size > reader.remaining()) {
        reader.fail (ErrorCode::malformed, "truncated list");
    }
}

// ---- how enumerations travel ----

// for every wire enumeration: its width, the range check and the error a bad value gives
template <typename E>
struct EnumWire;

template <>
struct EnumWire<AddAction> {
    using Wire                                 = std::uint8_t;
    static constexpr ErrorCode        kBadCode = ErrorCode::malformed;
    static constexpr std::string_view kDetail  = "invalid add action";

    static std::optional<AddAction>
    from_wire (Wire value) {
        return add_action_from_wire (value);
    }
};

template <>
struct EnumWire<BusKind> {
    using Wire                                 = std::uint8_t;
    static constexpr ErrorCode        kBadCode = ErrorCode::malformed;
    static constexpr std::string_view kDetail  = "invalid bus kind";

    static std::optional<BusKind>
    from_wire (Wire value) {
        return bus_kind_from_wire (value);
    }
};

template <>
struct EnumWire<NodeKind> {
    using Wire                                 = std::uint8_t;
    static constexpr ErrorCode        kBadCode = ErrorCode::malformed;
    static constexpr std::string_view kDetail  = "invalid node kind";

    static std::optional<NodeKind>
    from_wire (Wire value) {
        return node_kind_from_wire (value);
    }
};

// only `lin` exists in v0, the other values are reserved
template <>
struct EnumWire<Curve> {
    using Wire                                 = std::uint8_t;
    static constexpr ErrorCode        kBadCode = ErrorCode::unsupported;
    static constexpr std::string_view kDetail  = "reserved curve";

    static std::optional<Curve>
    from_wire (Wire value) {
        if (value == static_cast<Wire> (Curve::lin)) {
            return Curve::lin;
        }
        return std::nullopt;
    }
};

template <>
struct EnumWire<ErrorCode> {
    using Wire                                 = std::uint16_t;
    static constexpr ErrorCode        kBadCode = ErrorCode::unsupported;
    static constexpr std::string_view kDetail  = "unknown error code";

    static std::optional<ErrorCode>
    from_wire (Wire value) {
        return error_code_from_wire (value);
    }
};

// ---- one description per structure, two directions ----
//
// serialize(archive, value) lists the fields of a structure once, in wire order. the same
// function reads a structure (Decoder) and writes it (Encoder), so the two directions cannot
// drift apart and the checks of a field are written once. the archives share these calls:
//   u8 u16 u32 u64 i64 f32 boolean   scalars
//   str path blob                    strings and bytes
//   enumeration                      a wire enum, range checked through EnumWire
//   flag_bit0 magic event_bits       fields with a rule of their own
//   list                             u16 count followed by the elements

class Decoder {
public:
    explicit Decoder (Reader& reader) : reader_ (reader) {}

    void
    u8 (std::uint8_t& value) {
        value = reader_.u8();
    }

    void
    u16 (std::uint16_t& value) {
        value = reader_.u16();
    }

    void
    u32 (std::uint32_t& value) {
        value = reader_.u32();
    }

    void
    u64 (std::uint64_t& value) {
        value = reader_.u64();
    }

    void
    i64 (std::int64_t& value) {
        value = reader_.i64();
    }

    void
    f32 (float& value) {
        value = reader_.f32();
    }

    void
    boolean (bool& value) {
        value = reader_.boolean();
    }

    void
    str (std::string& value) {
        value = reader_.str();
    }

    void
    path (std::string& value) {
        value = reader_.path();
    }

    void
    blob (std::vector<std::byte>& value) {
        value = reader_.blob();
    }

    template <typename E>
    void
    enumeration (E& value) {
        using Traits = EnumWire<E>;
        typename Traits::Wire raw{};
        read_wire (raw);
        const auto parsed = Traits::from_wire (raw);
        if (reader_.ok() && !parsed) {
            reader_.fail_back (sizeof (raw), Traits::kBadCode, Traits::kDetail);
        }
        value = parsed.value_or (E{});
    }

    // node.new flags: bit 0 is `paused`, the others are reserved
    void
    flag_bit0 (bool& value) {
        const auto flags = reader_.u8();
        if (reader_.ok() && (flags & ~1U) != 0) {
            reader_.fail_back (1, ErrorCode::unsupported, "reserved node.new flags");
        }
        value = (flags & 1U) != 0;
    }

    void
    magic () {
        if (reader_.u32() != kMagic && reader_.ok()) {
            reader_.fail_back (4, ErrorCode::malformed, "bad magic");
        }
    }

    void
    event_bits (std::uint32_t& value) {
        value = reader_.u32();
        if (reader_.ok() && (value & ~kEventMask) != 0) {
            reader_.fail_back (4, ErrorCode::unsupported, "reserved event bits");
        }
    }

    template <typename T>
    void
    list (std::vector<T>& values, std::size_t element_size) {
        values.clear();
        const auto count = reader_.u16();
        check_list (reader_, count, element_size);
        if (!reader_.ok()) {
            return;
        }
        values.resize (count);
        for (auto& element : values) {
            serialize (*this, element);
        }
    }

private:
    void
    read_wire (std::uint8_t& value) {
        value = reader_.u8();
    }

    void
    read_wire (std::uint16_t& value) {
        value = reader_.u16();
    }

    Reader& reader_;
};

class Encoder {
public:
    explicit Encoder (Writer& writer) : writer_ (writer) {}

    void
    u8 (std::uint8_t value) {
        writer_.u8 (value);
    }

    void
    u16 (std::uint16_t value) {
        writer_.u16 (value);
    }

    void
    u32 (std::uint32_t value) {
        writer_.u32 (value);
    }

    void
    u64 (std::uint64_t value) {
        writer_.u64 (value);
    }

    void
    i64 (std::int64_t value) {
        writer_.i64 (value);
    }

    void
    f32 (float value) {
        writer_.f32 (value);
    }

    void
    boolean (bool value) {
        writer_.boolean (value);
    }

    void
    str (const std::string& value) {
        writer_.str (value);
    }

    void
    path (const std::string& value) {
        writer_.path (value);
    }

    void
    blob (const std::vector<std::byte>& value) {
        writer_.blob (value);
    }

    template <typename E>
    void
    enumeration (const E& value) {
        using Traits   = EnumWire<E>;
        const auto raw = static_cast<typename Traits::Wire> (value);
        if (!Traits::from_wire (raw)) {
            writer_.fail (Traits::kDetail);
        }
        write_wire (raw);
    }

    void
    flag_bit0 (bool value) {
        writer_.u8 (value ? 1 : 0);
    }

    void
    magic () {
        writer_.u32 (kMagic);
    }

    void
    event_bits (std::uint32_t value) {
        if ((value & ~kEventMask) != 0) {
            writer_.fail ("reserved event bits");
        }
        writer_.u32 (value);
    }

    template <typename T>
    void
    list (std::vector<T>& values, std::size_t /*element_size*/) {
        writer_.count (values.size());
        for (auto& element : values) {
            serialize (*this, element);
        }
    }

private:
    void
    write_wire (std::uint8_t value) {
        writer_.u8 (value);
    }

    void
    write_wire (std::uint16_t value) {
        writer_.u16 (value);
    }

    Writer& writer_;
};

// scalars as list elements
template <typename Ar>
void
serialize (Ar& ar, std::uint32_t& value) {
    ar.u32 (value);
}

template <typename Ar>
void
serialize (Ar& ar, float& value) {
    ar.f32 (value);
}

template <typename Ar>
void
serialize (Ar& ar, Pair& pair) {
    ar.u16 (pair.index);
    ar.f32 (pair.value);
}

// ---- handshake ----

template <typename Ar>
void
serialize (Ar& ar, Hello& m) {
    ar.magic();
    ar.u16 (m.major);
    ar.u16 (m.minor);
    ar.u64 (m.catalog_hash);
    ar.u8 (m.requested_client);
    ar.event_bits (m.events);
    ar.str (m.name);
}

template <typename Ar>
void
serialize (Ar& ar, Welcome& m) {
    ar.magic();
    ar.u16 (m.major);
    ar.u16 (m.minor);
    ar.u64 (m.catalog_hash);
    ar.u8 (m.client_id);
    ar.u32 (m.id_base);
    ar.u32 (m.id_count);
    ar.u32 (m.sample_rate);
    ar.u16 (m.block_size);
    ar.u16 (m.out_channels);
    ar.u16 (m.in_channels);
    ar.u32 (m.audio_buses);
    ar.u32 (m.control_buses);
    ar.u32 (m.max_buffers);
    ar.i64 (m.output_latency_ns);
    ar.i64 (m.late_tolerance_ns);
    ar.u32 (m.max_message);
    ar.i64 (m.server_now_ns);
    ar.str (m.server);
}

template <typename Ar>
void
serialize (Ar& ar, Reject& m) {
    ar.enumeration (m.code);
    ar.str (m.message);
}

// ---- rt commands ----

template <typename Ar>
void
serialize (Ar& ar, NodeNew& c) {
    ar.u32 (c.id);
    ar.u32 (c.def);
    ar.enumeration (c.add);
    ar.u32 (c.target);
    ar.flag_bit0 (c.paused);
    ar.list (c.params, kPairSize);
}

template <typename Ar>
void
serialize (Ar& ar, GroupNew& c) {
    ar.u32 (c.id);
    ar.enumeration (c.add);
    ar.u32 (c.target);
}

template <typename Ar>
void
serialize (Ar& ar, NodeFree& c) {
    ar.list (c.ids, sizeof (std::uint32_t));
}

template <typename Ar>
void
serialize (Ar& ar, NodeSet& c) {
    ar.u32 (c.id);
    ar.f32 (c.ramp);
    ar.enumeration (c.curve);
    ar.list (c.params, kPairSize);
}

template <typename Ar>
void
serialize (Ar& ar, NodeRun& c) {
    ar.u32 (c.id);
    ar.boolean (c.run);
}

template <typename Ar>
void
serialize (Ar& ar, NodeMove& c) {
    ar.u32 (c.id);
    ar.enumeration (c.add);
    ar.u32 (c.target);
}

template <typename Ar>
void
serialize (Ar& ar, GroupClear& c) {
    ar.u32 (c.id);
}

template <typename Ar>
void
serialize (Ar& ar, CbusSet& c) {
    ar.u32 (c.index);
    ar.list (c.values, sizeof (float));
}

// ---- calls ----

template <typename Ar>
void
serialize (Ar&, Sync&) {}

template <typename Ar>
void
serialize (Ar&, Status&) {}

template <typename Ar>
void
serialize (Ar& ar, Tree& c) {
    ar.u32 (c.root);
    ar.u16 (c.depth);
}

template <typename Ar>
void
serialize (Ar&, ClientReset&) {}

template <typename Ar>
void
serialize (Ar& ar, DefLoad& c) {
    ar.blob (c.graph);
}

template <typename Ar>
void
serialize (Ar& ar, DefFree& c) {
    ar.u32 (c.def);
}

template <typename Ar>
void
serialize (Ar& ar, BufAlloc& c) {
    ar.u32 (c.frames);
    ar.u16 (c.channels);
}

template <typename Ar>
void
serialize (Ar& ar, BufLoad& c) {
    ar.path (c.path);
    ar.u32 (c.start);
    ar.u32 (c.frames);
}

template <typename Ar>
void
serialize (Ar& ar, BufFree& c) {
    ar.u32 (c.buf);
}

template <typename Ar>
void
serialize (Ar& ar, BusAlloc& c) {
    ar.enumeration (c.kind);
    ar.u16 (c.count);
}

template <typename Ar>
void
serialize (Ar& ar, BusFree& c) {
    ar.enumeration (c.kind);
    ar.u32 (c.base);
}

// ---- call results ----

template <typename Ar>
void
serialize (Ar&, Empty&) {}

template <typename Ar>
void
serialize (Ar& ar, StatusInfo& s) {
    ar.i64 (s.now_ns);
    ar.u32 (s.sample_rate);
    ar.u16 (s.block_size);
    ar.f32 (s.cpu_avg);
    ar.f32 (s.cpu_peak);
    ar.u32 (s.nodes);
    ar.u32 (s.groups);
    ar.u32 (s.defs);
    ar.u32 (s.buffers);
    ar.u32 (s.rt_queue_used);
    ar.u32 (s.rt_queue_capacity);
    ar.u64 (s.late_played);
    ar.u64 (s.late_dropped);
    ar.u64 (s.skipped);
    ar.u64 (s.xruns);
    ar.u64 (s.events_dropped);
}

template <typename Ar>
void
serialize (Ar& ar, TreeEntry& e) {
    ar.u32 (e.id);
    ar.u32 (e.parent);
    ar.enumeration (e.kind);
    ar.u32 (e.def);
    ar.boolean (e.running);
}

template <typename Ar>
void
serialize (Ar& ar, TreeInfo& t) {
    ar.boolean (t.truncated);
    ar.list (t.entries, kTreeEntrySize);
}

template <typename Ar>
void
serialize (Ar& ar, DefLoaded& r) {
    ar.u32 (r.def);
}

template <typename Ar>
void
serialize (Ar& ar, BufAllocated& r) {
    ar.u32 (r.buf);
}

template <typename Ar>
void
serialize (Ar& ar, BufLoaded& r) {
    ar.u32 (r.buf);
    ar.u32 (r.frames);
    ar.u16 (r.channels);
    ar.u32 (r.sample_rate);
}

template <typename Ar>
void
serialize (Ar& ar, BusAllocated& r) {
    ar.u32 (r.base);
}

// ---- events ----

template <typename Ar>
void
serialize (Ar& ar, NEnd& e) {
    ar.u32 (e.id);
}

template <typename Ar>
void
serialize (Ar& ar, Late& e) {
    ar.u32 (e.played);
    ar.u32 (e.dropped);
    ar.i64 (e.max_late_ns);
}

template <typename Ar>
void
serialize (Ar& ar, Xrun& e) {
    ar.u32 (e.count);
}

template <typename Ar>
void
serialize (Ar& ar, ErrorEvent& e) {
    ar.u32 (e.bundle_seq);
    ar.u16 (e.index);
    ar.u16 (e.opcode);
    ar.enumeration (e.code);
    ar.u8 (e.level);
    ar.str (e.message);
}

// ---- reading and writing through the descriptions ----

template <typename T>
T
decode_as (Reader& reader) {
    T       value{};
    Decoder decoder (reader);
    serialize (decoder, value);
    return value;
}

// the description takes a mutable value so that one function serves both directions. the
// Encoder only ever reads from it, which makes the cast safe.
template <typename T>
void
encode_as (Writer& writer, const T& value) {
    Encoder encoder (writer);
    serialize (encoder, const_cast<T&> (value));
}

RtCommand
read_rt (Opcode op, Reader& reader) {
    switch (op) {
        case Opcode::node_new: {
            return decode_as<NodeNew> (reader);
        }
        case Opcode::group_new: {
            return decode_as<GroupNew> (reader);
        }
        case Opcode::node_free: {
            return decode_as<NodeFree> (reader);
        }
        case Opcode::node_set: {
            return decode_as<NodeSet> (reader);
        }
        case Opcode::node_run: {
            return decode_as<NodeRun> (reader);
        }
        case Opcode::node_move: {
            return decode_as<NodeMove> (reader);
        }
        case Opcode::group_clear: {
            return decode_as<GroupClear> (reader);
        }
        default: {
            return decode_as<CbusSet> (reader);
        }
    }
}

CallCommand
read_call (Opcode op, Reader& reader) {
    switch (op) {
        case Opcode::sync: {
            return decode_as<Sync> (reader);
        }
        case Opcode::status: {
            return decode_as<Status> (reader);
        }
        case Opcode::tree: {
            return decode_as<Tree> (reader);
        }
        case Opcode::client_reset: {
            return decode_as<ClientReset> (reader);
        }
        case Opcode::def_load: {
            return decode_as<DefLoad> (reader);
        }
        case Opcode::def_free: {
            return decode_as<DefFree> (reader);
        }
        case Opcode::buf_alloc: {
            return decode_as<BufAlloc> (reader);
        }
        case Opcode::buf_load: {
            return decode_as<BufLoad> (reader);
        }
        case Opcode::buf_free: {
            return decode_as<BufFree> (reader);
        }
        case Opcode::bus_alloc: {
            return decode_as<BusAlloc> (reader);
        }
        default: {
            return decode_as<BusFree> (reader);
        }
    }
}

CallResult
read_result (Opcode op, Reader& reader) {
    switch (op) {
        case Opcode::status: {
            return decode_as<StatusInfo> (reader);
        }
        case Opcode::tree: {
            return decode_as<TreeInfo> (reader);
        }
        case Opcode::def_load: {
            return decode_as<DefLoaded> (reader);
        }
        case Opcode::buf_alloc: {
            return decode_as<BufAllocated> (reader);
        }
        case Opcode::buf_load: {
            return decode_as<BufLoaded> (reader);
        }
        case Opcode::bus_alloc: {
            return decode_as<BusAllocated> (reader);
        }
        default: {
            return decode_as<Empty> (reader);
        }
    }
}

EventData
read_event (Opcode op, Reader& reader) {
    switch (op) {
        case Opcode::n_end: {
            return decode_as<NEnd> (reader);
        }
        case Opcode::late: {
            return decode_as<Late> (reader);
        }
        case Opcode::xrun: {
            return decode_as<Xrun> (reader);
        }
        default: {
            return decode_as<ErrorEvent> (reader);
        }
    }
}

// frame = opcode + size + fields. a failed writer rolls `out` back to its start.
template <typename Variant>
std::expected<void, EncodeError>
write_frame (const Variant& command, std::vector<std::byte>& out) {
    const auto start = out.size();
    Writer     writer (out);
    std::visit (
        [&] (const auto& c) {
            writer.u16 (static_cast<std::uint16_t> (opcode_of (command)));
            writer.u16 (0);
            encode_as (writer, c);
        },
        command
    );
    const auto size = out.size() - start;
    if (writer.ok() && size > 0xFFFF) {
        writer.fail ("command too large");
    }
    if (!writer.ok()) {
        out.resize (start);
        return encode_failure (writer);
    }
    writer.patch_u16 (start + 2, static_cast<std::uint16_t> (size));
    return {};
}

// ---- command class checks ----

std::expected<void, DecodeError>
check_class (const CommandFrame& frame, OpcodeClass expected, std::uint16_t index) {
    const auto cls = classify (frame.opcode);
    if (!cls) {
        return std::unexpected (
            DecodeError{ ErrorCode::unsupported, frame.offset, index, "unknown opcode" }
        );
    }
    if (*cls != expected) {
        return std::unexpected (
            DecodeError{ ErrorCode::context, frame.offset, index, "opcode not allowed here" }
        );
    }
    return {};
}

// reads the fields of a frame with `read`; requires the frame to be consumed exactly
template <typename Read>
auto
read_frame (const CommandFrame& frame, std::uint16_t index, Read&& read)
    -> std::expected<std::invoke_result_t<Read, Reader&>, DecodeError> {
    Reader reader (frame.fields, frame.offset + 4);
    auto   value = read (reader);
    if (reader.ok() && reader.remaining() != 0) {
        reader.fail (ErrorCode::malformed, "trailing bytes in command");
    }
    if (!reader.ok()) {
        return std::unexpected (to_error (reader.failure(), index));
    }
    return value;
}

std::expected<void, DecodeError>
finish (const Reader& reader, std::uint16_t index) {
    if (!reader.ok()) {
        return std::unexpected (to_error (reader.failure(), index));
    }
    if (reader.remaining() != 0) {
        return std::unexpected (
            DecodeError{ ErrorCode::malformed, reader.offset(), index, "trailing bytes" }
        );
    }
    return {};
}

std::span<const std::byte>
payload_of (std::span<const std::byte> bytes) {
    return bytes.subspan (kHeaderSize);
}

// ---- message bodies ----

// hello, welcome and reject: the payload is just the described structure
template <typename Body>
std::expected<Message, DecodeError>
decode_plain (const Header& header, std::span<const std::byte> bytes) {
    Reader reader (payload_of (bytes), kHeaderSize);
    auto   body = decode_as<Body> (reader);
    if (auto done = finish (reader, kNoCommand); !done) {
        return std::unexpected (done.error());
    }
    return Message{ header.seq, std::move (body) };
}

std::expected<Message, DecodeError>
decode_bundle (const Header& header, std::span<const std::byte> bytes) {
    Reader reader (payload_of (bytes), kHeaderSize);
    Bundle bundle;
    bundle.time = reader.i64();
    if (reader.ok() && bundle.time < 0) {
        reader.fail_back (8, ErrorCode::invalid_arg, "negative time");
    }
    if (!reader.ok()) {
        return std::unexpected (to_error (reader.failure(), kNoCommand));
    }
    const auto frames_start = kHeaderSize + 8;
    auto       frames       = split_frames (bytes.subspan (frames_start), frames_start);
    if (!frames) {
        return std::unexpected (frames.error());
    }
    if (frames->empty()) {
        return std::unexpected (
            DecodeError{ ErrorCode::malformed, frames_start, kNoCommand, "empty bundle" }
        );
    }
    bundle.commands.reserve (frames->size());
    for (std::size_t i = 0; i < frames->size(); ++i) {
        auto command = decode_rt_command ((*frames)[i], static_cast<std::uint16_t> (i));
        if (!command) {
            return std::unexpected (command.error());
        }
        bundle.commands.push_back (std::move (*command));
    }
    return Message{ header.seq, std::move (bundle) };
}

// the payload of call and event holds exactly one command frame
std::expected<CommandFrame, DecodeError>
single_frame (std::span<const std::byte> bytes) {
    auto frames = split_frames (payload_of (bytes), kHeaderSize);
    if (!frames) {
        return std::unexpected (frames.error());
    }
    if (frames->size() != 1) {
        return std::unexpected (
            DecodeError{ ErrorCode::malformed,
                         kHeaderSize,
                         kNoCommand,
                         "expected exactly one command" }
        );
    }
    return frames->front();
}

std::expected<Message, DecodeError>
decode_call (const Header& header, std::span<const std::byte> bytes) {
    auto frame = single_frame (bytes);
    if (!frame) {
        return std::unexpected (frame.error());
    }
    auto command = decode_call_command (*frame);
    if (!command) {
        return std::unexpected (command.error());
    }
    return Message{ header.seq, Call{ std::move (*command) } };
}

std::expected<Message, DecodeError>
decode_event_message (const Header& header, std::span<const std::byte> bytes) {
    auto frame = single_frame (bytes);
    if (!frame) {
        return std::unexpected (frame.error());
    }
    auto event = decode_event (*frame);
    if (!event) {
        return std::unexpected (event.error());
    }
    return Message{ header.seq, Event{ std::move (*event) } };
}

std::expected<Message, DecodeError>
decode_reply (const Header& header, std::span<const std::byte> bytes) {
    Reader reader (payload_of (bytes), kHeaderSize);
    Reply  reply;
    reply.opcode      = reader.u16();
    const auto status = reader.u16();
    if (!reader.ok()) {
        return std::unexpected (to_error (reader.failure(), kNoCommand));
    }
    if (status == 0) {
        if (classify (reply.opcode) != OpcodeClass::call) {
            return std::unexpected (
                DecodeError{ ErrorCode::unsupported,
                             kHeaderSize,
                             kNoCommand,
                             "reply to an unknown call" }
            );
        }
        reply.outcome = read_result (static_cast<Opcode> (reply.opcode), reader);
    } else {
        const auto code = error_code_from_wire (status);
        if (!code) {
            return std::unexpected (
                DecodeError{ ErrorCode::unsupported,
                             kHeaderSize + 2,
                             kNoCommand,
                             "unknown error code" }
            );
        }
        CallError error;
        error.code    = *code;
        error.message = reader.str();
        reply.outcome = std::move (error);
    }
    if (auto done = finish (reader, kNoCommand); !done) {
        return std::unexpected (done.error());
    }
    return Message{ header.seq, std::move (reply) };
}

// ---- message bodies, encoding side ----

template <typename Body>
void
write_plain (Writer& writer, const Body& body) {
    encode_as (writer, body);
}

void
write_body (Writer& writer, const Hello& m) {
    write_plain (writer, m);
}

void
write_body (Writer& writer, const Welcome& m) {
    write_plain (writer, m);
}

void
write_body (Writer& writer, const Reject& m) {
    write_plain (writer, m);
}

void
write_body (Writer& writer, const Bundle& m) {
    if (m.time < 0) {
        writer.fail ("negative time");
    }
    if (m.commands.empty()) {
        writer.fail ("empty bundle");
    }
    writer.i64 (m.time);
}

// the command frame of a call or an event follows the header, see encode()
void
write_body (Writer&, const Call&) {}

void
write_body (Writer&, const Event&) {}

void
write_body (Writer& writer, const Reply& m) {
    writer.u16 (m.opcode);
    if (const auto* result = std::get_if<CallResult> (&m.outcome)) {
        if (!result_matches (m.opcode, *result)) {
            writer.fail ("result does not match the call opcode");
        }
        writer.u16 (0);
        std::visit (
            [&] (const auto& value) {
                encode_as (writer, value);
            },
            *result
        );
        return;
    }
    const auto& error = std::get<CallError> (m.outcome);
    if (error.code == ErrorCode::ok) {
        writer.fail ("error reply with code ok");
    }
    Encoder encoder (writer);
    encoder.enumeration (error.code);
    encoder.str (error.message);
}

}  // namespace

// ---- public api ----

std::expected<Header, DecodeError>
decode_header (std::span<const std::byte> bytes) {
    if (bytes.size() < kHeaderSize) {
        return std::unexpected (
            DecodeError{ ErrorCode::malformed, 0, kNoCommand, "truncated header" }
        );
    }
    Reader     reader (bytes.first (kHeaderSize), 0);
    const auto length = reader.u32();
    const auto kind   = reader.u16();
    const auto flags  = reader.u16();
    const auto seq    = reader.u32();
    if (length < kHeaderSize || length > kMaxMessage) {
        return std::unexpected (
            DecodeError{ ErrorCode::malformed, 0, kNoCommand, "length out of range" }
        );
    }
    if (length != bytes.size()) {
        return std::unexpected (
            DecodeError{ ErrorCode::malformed, 0, kNoCommand, "length does not match packet" }
        );
    }
    if (kind < static_cast<std::uint16_t> (MessageKind::hello) ||
        kind > static_cast<std::uint16_t> (MessageKind::event)) {
        return std::unexpected (
            DecodeError{ ErrorCode::unsupported, 4, kNoCommand, "unknown message kind" }
        );
    }
    if (flags != 0) {
        return std::unexpected (
            DecodeError{ ErrorCode::unsupported, 6, kNoCommand, "non-zero flags" }
        );
    }
    return Header{ static_cast<MessageKind> (kind), seq };
}

std::expected<Message, DecodeError>
decode (std::span<const std::byte> bytes) {
    const auto header = decode_header (bytes);
    if (!header) {
        return std::unexpected (header.error());
    }
    switch (header->kind) {
        case MessageKind::hello: {
            return decode_plain<Hello> (*header, bytes);
        }
        case MessageKind::welcome: {
            return decode_plain<Welcome> (*header, bytes);
        }
        case MessageKind::reject: {
            return decode_plain<Reject> (*header, bytes);
        }
        case MessageKind::bundle: {
            return decode_bundle (*header, bytes);
        }
        case MessageKind::call: {
            return decode_call (*header, bytes);
        }
        case MessageKind::reply: {
            return decode_reply (*header, bytes);
        }
        case MessageKind::event: {
            return decode_event_message (*header, bytes);
        }
    }
    return std::unexpected (
        DecodeError{ ErrorCode::unsupported, 4, kNoCommand, "unknown message kind" }
    );
}

std::expected<std::vector<CommandFrame>, DecodeError>
split_frames (std::span<const std::byte> payload, std::size_t base) {
    std::vector<CommandFrame> frames;
    std::size_t               pos = 0;
    while (pos < payload.size()) {
        const auto index =
            static_cast<std::uint16_t> (std::min<std::size_t> (frames.size(), 0xFFFE));
        if (payload.size() - pos < 4) {
            return std::unexpected (
                DecodeError{ ErrorCode::malformed, base + pos, index, "truncated command header" }
            );
        }
        Reader     reader (payload.subspan (pos, 4), base + pos);
        const auto opcode = reader.u16();
        const auto size   = reader.u16();
        if (size < 4) {
            return std::unexpected (
                DecodeError{ ErrorCode::malformed, base + pos + 2, index, "bad command size" }
            );
        }
        if (size > payload.size() - pos) {
            return std::unexpected (
                DecodeError{ ErrorCode::malformed,
                             base + pos + 2,
                             index,
                             "command exceeds message" }
            );
        }
        frames.push_back (CommandFrame{ opcode, base + pos, payload.subspan (pos + 4, size - 4U) });
        pos += size;
    }
    return frames;
}

std::expected<RtCommand, DecodeError>
decode_rt_command (const CommandFrame& frame, std::uint16_t index) {
    if (auto ok = check_class (frame, OpcodeClass::rt, index); !ok) {
        return std::unexpected (ok.error());
    }
    return read_frame (frame, index, [&] (Reader& reader) {
        return read_rt (static_cast<Opcode> (frame.opcode), reader);
    });
}

std::expected<CallCommand, DecodeError>
decode_call_command (const CommandFrame& frame, std::uint16_t index) {
    if (auto ok = check_class (frame, OpcodeClass::call, index); !ok) {
        return std::unexpected (ok.error());
    }
    return read_frame (frame, index, [&] (Reader& reader) {
        return read_call (static_cast<Opcode> (frame.opcode), reader);
    });
}

std::expected<EventData, DecodeError>
decode_event (const CommandFrame& frame, std::uint16_t index) {
    if (auto ok = check_class (frame, OpcodeClass::event, index); !ok) {
        return std::unexpected (ok.error());
    }
    return read_frame (frame, index, [&] (Reader& reader) {
        return read_event (static_cast<Opcode> (frame.opcode), reader);
    });
}

std::expected<void, EncodeError>
encode_command (const RtCommand& command, std::vector<std::byte>& out) {
    return write_frame (command, out);
}

std::expected<void, EncodeError>
encode_command (const CallCommand& command, std::vector<std::byte>& out) {
    return write_frame (command, out);
}

std::expected<void, EncodeError>
encode_command (const EventData& event, std::vector<std::byte>& out) {
    return write_frame (event, out);
}

std::expected<std::vector<std::byte>, EncodeError>
encode (const Message& message) {
    std::vector<std::byte> out;
    out.reserve (64);
    Writer writer (out);
    writer.u32 (0);  // length, patched below
    writer.u16 (static_cast<std::uint16_t> (kind_of (message.body)));
    writer.u16 (0);
    writer.u32 (message.seq);

    std::expected<void, EncodeError> framed;
    std::visit (
        [&] (const auto& body) {
            write_body (writer, body);
            using T = std::decay_t<decltype (body)>;
            if constexpr (std::is_same_v<T, Bundle>) {
                for (const auto& command : body.commands) {
                    if (!framed) {
                        break;
                    }
                    framed = encode_command (command, out);
                }
            } else if constexpr (std::is_same_v<T, Call>) {
                framed = encode_command (body.command, out);
            } else if constexpr (std::is_same_v<T, Event>) {
                framed = encode_command (body.data, out);
            }
        },
        message.body
    );

    if (!writer.ok()) {
        return encode_failure (writer);
    }
    if (!framed) {
        return std::unexpected (framed.error());
    }
    if (out.size() > kMaxMessage) {
        return std::unexpected (EncodeError{ "message exceeds 65536 bytes" });
    }
    writer.patch_u32 (0, static_cast<std::uint32_t> (out.size()));
    return out;
}

}  // namespace bitvm::proto
