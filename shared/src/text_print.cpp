#include <cmath>
#include <type_traits>
#include <unordered_map>
#include <variant>

#include "bitvm/proto/codec.hpp"
#include "bitvm/proto/text.hpp"
#include "text_util.hpp"

namespace bitvm::proto
{
namespace
{

using text::format_f32;
using text::format_hex32;
using text::format_str;

// node id -> def, learned from node.new inside the text being printed
using Overlay = std::unordered_map<NodeId, std::uint32_t>;

std::string
opcode_label (std::uint16_t opcode) {
    const auto name = name_of (static_cast<Opcode> (opcode));
    return name.empty() ? text::format_hex_opcode (opcode) : std::string (name);
}

class Printer {
public:
    Printer&
    kv (std::string_view key, std::string_view value) {
        s_ += ' ';
        s_ += key;
        s_ += '=';
        s_ += value;
        return *this;
    }

    Printer&
    num (std::string_view key, std::uint64_t value) {
        return kv (key, std::to_string (value));
    }

    Printer&
    num (std::string_view key, std::int64_t value) {
        return kv (key, std::to_string (value));
    }

    Printer&
    hex (std::string_view key, std::uint32_t value) {
        return kv (key, format_hex32 (value));
    }

    Printer&
    flt (std::string_view key, float value) {
        return kv (key, format_f32 (value));
    }

    Printer&
    boolean (std::string_view key, bool value) {
        return kv (key, value ? "1" : "0");
    }

    Printer&
    raw (std::string_view text) {
        s_ += text;
        return *this;
    }

    [[nodiscard]] std::string
    take () {
        return std::move (s_);
    }

private:
    std::string s_;
};

std::string
param_key (const TextContext& ctx, std::optional<std::uint32_t> def, std::uint16_t index) {
    if (def && ctx.param_names) {
        const auto* names = ctx.param_names (*def);
        if (names != nullptr && index < names->size() && is_valid_name ((*names)[index])) {
            return "$" + (*names)[index];
        }
    }
    return "$" + std::to_string (index);
}

void
print_params (
    Printer&                     p,
    const std::vector<Pair>&     pairs,
    std::optional<std::uint32_t> def,
    const TextContext&           ctx
) {
    for (const auto& pair : pairs) {
        p.kv (param_key (ctx, def, pair.index), format_f32 (pair.value));
    }
}

std::optional<std::uint32_t>
def_of_node (NodeId id, const TextContext& ctx, const Overlay& overlay) {
    if (const auto it = overlay.find (id); it != overlay.end()) {
        return it->second;
    }
    return ctx.node_def ? ctx.node_def (id) : std::nullopt;
}

std::string
join_hex (const std::vector<std::uint32_t>& values) {
    std::string out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        out += (i == 0 ? "" : ",") + format_hex32 (values[i]);
    }
    return out;
}

// ---- bundle commands ----

std::string
print_rt (const RtCommand& command, const TextContext& ctx, Overlay& overlay) {
    Printer p;
    std::visit (
        [&] (const auto& c) {
            using T = std::decay_t<decltype (c)>;
            if constexpr (std::is_same_v<T, NodeNew>) {
                p.raw ("node.new").hex ("id", c.id).num ("def", std::uint64_t{ c.def });
                p.kv ("add", name_of (c.add)).hex ("target", c.target);
                if (c.paused) {
                    p.boolean ("paused", true);
                }
                print_params (p, c.params, c.def, ctx);
                overlay[c.id] = c.def;
            } else if constexpr (std::is_same_v<T, GroupNew>) {
                p.raw ("group.new")
                    .hex ("id", c.id)
                    .kv ("add", name_of (c.add))
                    .hex ("target", c.target);
            } else if constexpr (std::is_same_v<T, NodeFree>) {
                p.raw ("node.free").kv ("ids", join_hex (c.ids));
            } else if constexpr (std::is_same_v<T, NodeSet>) {
                p.raw ("node.set").hex ("id", c.id);
                if (c.ramp != 0.0F || std::signbit (c.ramp)) {
                    p.flt ("ramp", c.ramp);
                }
                if (c.curve != Curve::lin) {
                    p.kv ("curve", name_of (c.curve));
                }
                print_params (p, c.params, def_of_node (c.id, ctx, overlay), ctx);
            } else if constexpr (std::is_same_v<T, NodeRun>) {
                p.raw ("node.run").hex ("id", c.id).boolean ("run", c.run);
            } else if constexpr (std::is_same_v<T, NodeMove>) {
                p.raw ("node.move")
                    .hex ("id", c.id)
                    .kv ("add", name_of (c.add))
                    .hex ("target", c.target);
            } else if constexpr (std::is_same_v<T, GroupClear>) {
                p.raw ("group.clear").hex ("id", c.id);
            } else {
                std::string values;
                for (std::size_t i = 0; i < c.values.size(); ++i) {
                    values += (i == 0 ? "" : ",") + format_f32 (c.values[i]);
                }
                p.raw ("cbus.set").num ("index", std::uint64_t{ c.index }).kv ("values", values);
            }
        },
        command
    );
    return p.take();
}

// ---- calls ----

std::string
print_call (const CallCommand& command) {
    Printer p;
    p.raw (name_of (opcode_of (command)));
    std::visit (
        [&] (const auto& c) {
            using T = std::decay_t<decltype (c)>;
            if constexpr (std::is_same_v<T, Tree>) {
                p.hex ("root", c.root);
                if (c.depth != 0) {
                    p.num ("depth", std::uint64_t{ c.depth });
                }
            } else if constexpr (std::is_same_v<T, DefLoad>) {
                p.kv ("graph", "base64:" + base64_encode (c.graph));
            } else if constexpr (std::is_same_v<T, DefFree>) {
                p.num ("def", std::uint64_t{ c.def });
            } else if constexpr (std::is_same_v<T, BufAlloc>) {
                p.num ("frames", std::uint64_t{ c.frames })
                    .num ("channels", std::uint64_t{ c.channels });
            } else if constexpr (std::is_same_v<T, BufLoad>) {
                p.kv ("path", format_str (c.path));
                if (c.start != 0) {
                    p.num ("start", std::uint64_t{ c.start });
                }
                if (c.frames != 0) {
                    p.num ("frames", std::uint64_t{ c.frames });
                }
            } else if constexpr (std::is_same_v<T, BufFree>) {
                p.num ("buf", std::uint64_t{ c.buf });
            } else if constexpr (std::is_same_v<T, BusAlloc>) {
                p.kv ("kind", name_of (c.kind)).num ("count", std::uint64_t{ c.count });
            } else if constexpr (std::is_same_v<T, BusFree>) {
                p.kv ("kind", name_of (c.kind)).num ("base", std::uint64_t{ c.base });
            }
        },
        command
    );
    return p.take();
}

// ---- results and events ----

void
print_result (Printer& p, const CallResult& result) {
    std::visit (
        [&] (const auto& r) {
            using T = std::decay_t<decltype (r)>;
            if constexpr (std::is_same_v<T, StatusInfo>) {
                p.num ("now_ns", r.now_ns)
                    .num ("sample_rate", std::uint64_t{ r.sample_rate })
                    .num ("block_size", std::uint64_t{ r.block_size })
                    .flt ("cpu_avg", r.cpu_avg)
                    .flt ("cpu_peak", r.cpu_peak)
                    .num ("nodes", std::uint64_t{ r.nodes })
                    .num ("groups", std::uint64_t{ r.groups })
                    .num ("defs", std::uint64_t{ r.defs })
                    .num ("buffers", std::uint64_t{ r.buffers })
                    .num ("rt_queue_used", std::uint64_t{ r.rt_queue_used })
                    .num ("rt_queue_capacity", std::uint64_t{ r.rt_queue_capacity })
                    .num ("late_played", r.late_played)
                    .num ("late_dropped", r.late_dropped)
                    .num ("skipped", r.skipped)
                    .num ("xruns", r.xruns)
                    .num ("events_dropped", r.events_dropped);
            } else if constexpr (std::is_same_v<T, TreeInfo>) {
                std::string entries;
                for (std::size_t i = 0; i < r.entries.size(); ++i) {
                    const auto& e = r.entries[i];
                    entries += (i == 0 ? "" : ",") + format_hex32 (e.id) + ":" +
                               format_hex32 (e.parent) + ":" + std::string (name_of (e.kind)) +
                               ":" + std::to_string (e.def) + ":" + (e.running ? "1" : "0");
                }
                p.boolean ("truncated", r.truncated).kv ("entries", entries);
            } else if constexpr (std::is_same_v<T, DefLoaded>) {
                p.num ("def", std::uint64_t{ r.def });
            } else if constexpr (std::is_same_v<T, BufAllocated>) {
                p.num ("buf", std::uint64_t{ r.buf });
            } else if constexpr (std::is_same_v<T, BufLoaded>) {
                p.num ("buf", std::uint64_t{ r.buf })
                    .num ("frames", std::uint64_t{ r.frames })
                    .num ("channels", std::uint64_t{ r.channels })
                    .num ("sample_rate", std::uint64_t{ r.sample_rate });
            } else if constexpr (std::is_same_v<T, BusAllocated>) {
                p.num ("base", std::uint64_t{ r.base });
            }
        },
        result
    );
}

std::string
print_event (const EventData& event) {
    Printer p;
    p.raw (name_of (opcode_of (event)));
    std::visit (
        [&] (const auto& e) {
            using T = std::decay_t<decltype (e)>;
            if constexpr (std::is_same_v<T, NEnd>) {
                p.hex ("id", e.id);
            } else if constexpr (std::is_same_v<T, Late>) {
                p.num ("played", std::uint64_t{ e.played })
                    .num ("dropped", std::uint64_t{ e.dropped })
                    .num ("max_late_ns", e.max_late_ns);
            } else if constexpr (std::is_same_v<T, Xrun>) {
                p.num ("count", std::uint64_t{ e.count });
            } else {
                p.num ("bundle_seq", std::uint64_t{ e.bundle_seq })
                    .num ("index", std::uint64_t{ e.index })
                    .kv ("opcode", opcode_label (e.opcode))
                    .kv ("code", name_of (e.code))
                    .num ("level", std::uint64_t{ e.level })
                    .kv ("msg", format_str (e.message));
            }
        },
        event
    );
    return p.take();
}

std::string
format_events (std::uint32_t bits) {
    if (bits == 0) {
        return "none";
    }
    std::string out;
    const auto  add = [&] (std::uint32_t bit, std::string_view name) {
        if ((bits & bit) != 0) {
            out += (out.empty() ? "" : ",");
            out += name;
        }
    };
    add (kEventNEnd, "n_end");
    add (kEventLate, "late");
    add (kEventXrun, "xrun");
    return out;
}

// ---- messages ----

std::string
seq_suffix (std::uint32_t seq) {
    return "/" + std::to_string (seq);
}

std::string
print_body (std::uint32_t seq, const Hello& m, const TextContext&) {
    Printer p;
    p.raw ("hello" + seq_suffix (seq))
        .num ("major", std::uint64_t{ m.major })
        .num ("minor", std::uint64_t{ m.minor })
        .kv ("catalog_hash", text::format_hex64 (m.catalog_hash))
        .num ("requested_client", std::uint64_t{ m.requested_client })
        .kv ("events", format_events (m.events))
        .kv ("name", format_str (m.name));
    return p.take();
}

std::string
print_body (std::uint32_t seq, const Welcome& m, const TextContext&) {
    Printer p;
    p.raw ("welcome" + seq_suffix (seq))
        .num ("major", std::uint64_t{ m.major })
        .num ("minor", std::uint64_t{ m.minor })
        .kv ("catalog_hash", text::format_hex64 (m.catalog_hash))
        .num ("client_id", std::uint64_t{ m.client_id })
        .hex ("id_base", m.id_base)
        .num ("id_count", std::uint64_t{ m.id_count })
        .num ("sample_rate", std::uint64_t{ m.sample_rate })
        .num ("block_size", std::uint64_t{ m.block_size })
        .num ("out_channels", std::uint64_t{ m.out_channels })
        .num ("in_channels", std::uint64_t{ m.in_channels })
        .num ("audio_buses", std::uint64_t{ m.audio_buses })
        .num ("control_buses", std::uint64_t{ m.control_buses })
        .num ("max_buffers", std::uint64_t{ m.max_buffers })
        .num ("output_latency_ns", m.output_latency_ns)
        .num ("late_tolerance_ns", m.late_tolerance_ns)
        .num ("max_message", std::uint64_t{ m.max_message })
        .num ("server_now_ns", m.server_now_ns)
        .kv ("server", format_str (m.server));
    return p.take();
}

std::string
print_body (std::uint32_t seq, const Reject& m, const TextContext&) {
    Printer p;
    p.raw ("reject" + seq_suffix (seq))
        .kv ("code", name_of (m.code))
        .kv ("msg", format_str (m.message));
    return p.take();
}

std::string
print_body (std::uint32_t seq, const Bundle& m, const TextContext& ctx) {
    Overlay     overlay;
    std::string out =
        "@" + (m.time == 0 ? std::string ("now") : std::to_string (m.time)) + seq_suffix (seq);
    if (m.commands.size() == 1) {
        return out + " " + print_rt (m.commands[0], ctx, overlay);
    }
    out += " {\n";
    for (const auto& c : m.commands) {
        out += "  " + print_rt (c, ctx, overlay) + "\n";
    }
    out += "}";
    return out;
}

std::string
print_body (std::uint32_t seq, const Call& m, const TextContext&) {
    return "call" + seq_suffix (seq) + " " + print_call (m.command);
}

std::string
print_body (std::uint32_t seq, const Reply& m, const TextContext&) {
    Printer p;
    p.raw ("reply" + seq_suffix (seq) + " " + opcode_label (m.opcode));
    if (const auto* result = std::get_if<CallResult> (&m.outcome)) {
        p.raw (" ok");
        print_result (p, *result);
    } else {
        const auto& e = std::get<CallError> (m.outcome);
        p.kv ("err", name_of (e.code)).kv ("msg", format_str (e.message));
        // `err=` follows the call name directly
    }
    return p.take();
}

std::string
print_body (std::uint32_t seq, const Event& m, const TextContext&) {
    return "event" + seq_suffix (seq) + " " + print_event (m.data);
}

std::string
hex_bytes (std::span<const std::byte> bytes) {
    static constexpr std::string_view kDigits = "0123456789abcdef";
    std::string                       out;
    out.reserve (bytes.size() * 2);
    for (const auto b : bytes) {
        out += kDigits[(static_cast<unsigned> (b) >> 4) & 0xF];
        out += kDigits[static_cast<unsigned> (b) & 0xF];
    }
    return out;
}

std::string
describe (const DecodeError& e) {
    std::string out = "# error: " + std::string (name_of (e.code)) + ": " + std::string (e.detail) +
                      " at byte " + std::to_string (e.offset);
    if (e.command_index != kNoCommand) {
        out += ", command " + std::to_string (e.command_index);
    }
    return out + "\n";
}

std::string
unreadable (const CommandFrame& frame) {
    return "? opcode=" + text::format_hex_opcode (frame.opcode) +
           " data=" + hex_bytes (frame.fields);
}

// walks command frames like split_frames but keeps what it has read before a bad frame
std::vector<CommandFrame>
walk_frames (std::span<const std::byte> payload, std::size_t base, std::string& problem) {
    std::vector<CommandFrame> frames;
    std::size_t               pos = 0;
    while (pos < payload.size()) {
        if (payload.size() - pos < 4) {
            problem =
                "# stopped: truncated command header at byte " + std::to_string (base + pos) + "\n";
            break;
        }
        const auto opcode = static_cast<std::uint16_t> (
            static_cast<unsigned> (payload[pos]) | (static_cast<unsigned> (payload[pos + 1]) << 8)
        );
        const auto size = static_cast<std::uint16_t> (
            static_cast<unsigned> (payload[pos + 2]) |
            (static_cast<unsigned> (payload[pos + 3]) << 8)
        );
        if (size < 4 || size > payload.size() - pos) {
            problem =
                "# stopped: bad command size at byte " + std::to_string (base + pos + 2) + "\n";
            break;
        }
        frames.push_back (CommandFrame{ opcode, base + pos, payload.subspan (pos + 4, size - 4U) });
        pos += size;
    }
    return frames;
}

}  // namespace

std::string
disassemble (const Message& message, const TextContext& ctx) {
    return std::visit (
        [&] (const auto& body) {
            return print_body (message.seq, body, ctx) + "\n";
        },
        message.body
    );
}

std::string
disassemble (std::span<const std::byte> bytes, const TextContext& ctx) {
    auto message = decode (bytes);
    if (message) {
        return disassemble (*message, ctx);
    }
    std::string out    = describe (message.error());
    const auto  header = decode_header (bytes);
    if (!header) {
        return out + "# bytes=" + hex_bytes (bytes) + "\n";
    }
    const auto  seq = seq_suffix (header->seq);
    std::string problem;
    switch (header->kind) {
        case MessageKind::bundle: {
            if (bytes.size() < kHeaderSize + 8) {
                break;
            }
            std::uint64_t time = 0;
            for (std::size_t i = 0; i < 8; ++i) {
                time |= static_cast<std::uint64_t> (bytes[kHeaderSize + i]) << (8 * i);
            }
            const auto frames =
                walk_frames (bytes.subspan (kHeaderSize + 8), kHeaderSize + 8, problem);
            out += "@" + std::to_string (static_cast<std::int64_t> (time)) + seq + " {\n";
            Overlay overlay;
            for (const auto& frame : frames) {
                const auto command = decode_rt_command (frame);
                out += "  " + (command ? print_rt (*command, ctx, overlay) : unreadable (frame)) +
                       "\n";
            }
            out += "}\n" + problem;
            return out;
        }
        case MessageKind::call:
        case MessageKind::event: {
            const auto frames = walk_frames (bytes.subspan (kHeaderSize), kHeaderSize, problem);
            for (const auto& frame : frames) {
                if (header->kind == MessageKind::call) {
                    const auto command = decode_call_command (frame);
                    out +=
                        command ? "call" + seq + " " + print_call (*command) : unreadable (frame);
                } else {
                    const auto event = decode_event (frame);
                    out += event ? "event" + seq + " " + print_event (*event) : unreadable (frame);
                }
                out += "\n";
            }
            return out + problem;
        }
        default: {
            break;
        }
    }
    return out + "# bytes=" + hex_bytes (bytes) + "\n";
}

}  // namespace bitvm::proto
