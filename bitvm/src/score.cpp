#include "bitvm/score.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <unordered_map>

#include "bitvm/proto/catalog.hpp"
#include "bitvm/proto/codec.hpp"
#include "bitvm/proto/text.hpp"
#include "bitvm/time_map.hpp"

namespace bitvm
{
namespace
{

std::unexpected<ScoreError>
error (std::string message, std::size_t index = 0) {
    return std::unexpected (ScoreError{ std::move (message), index });
}

std::expected<void, ScoreError>
validate_score (const Score& score) {
    std::int64_t previous = 0;
    for (std::size_t i = 0; i < score.messages.size(); ++i) {
        const auto& message = score.messages[i];
        if (const auto* bundle = std::get_if<proto::Bundle> (&message.body)) {
            if (bundle->time < 0 || (bundle->time != 0 && bundle->time < previous)) {
                return error ("score bundle times must be nonnegative and nondecreasing", i + 1);
            }
            if (bundle->time != 0) {
                previous = bundle->time;
            }
        } else if (!std::holds_alternative<proto::Call> (message.body)) {
            return error ("scores allow only call and bundle messages", i + 1);
        }
        if (const auto encoded = proto::encode (message); !encoded) {
            return error (std::string (encoded.error().detail), i + 1);
        }
    }
    return {};
}

std::expected<std::string, std::string>
read_text (const std::filesystem::path& path) {
    std::ifstream input (path, std::ios::binary);
    if (!input) {
        return std::unexpected ("cannot open " + path.string());
    }
    std::string result ((std::istreambuf_iterator<char> (input)), {});
    if (input.bad()) {
        return std::unexpected ("cannot read " + path.string());
    }
    return result;
}

std::uint64_t
little_endian (std::span<const std::byte> bytes) {
    std::uint64_t result = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        result |= std::to_integer<std::uint64_t> (bytes[i]) << (8 * i);
    }
    return result;
}

void
append_integer (std::vector<std::byte>& bytes, std::uint64_t value, unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
        bytes.push_back (static_cast<std::byte> ((value >> (8 * i)) & 0xffU));
    }
}

std::expected<void, ScoreError>
check_notifications (Engine& engine, std::size_t index) {
    engine.collect();
    if (engine.status().events_dropped != 0) {
        return error (
            "offline notification queue overflowed; bundle errors may have been lost",
            index
        );
    }
    Notification notification;
    while (engine.pop_notification (notification)) {
        if (notification.kind == Notification::Kind::error) {
            return error (
                "bundle " + std::to_string (notification.seq) + " command " +
                    std::to_string (notification.index) + " failed with code " +
                    std::string (proto::name_of (notification.code)),
                index
            );
        }
        if (notification.kind == Notification::Kind::late) {
            return error ("offline score bundle was scheduled late", index);
        }
    }
    return {};
}

}  // namespace

std::expected<Score, ScoreError>
parse_score (std::string_view text, const std::filesystem::path& source_path) {
    Score result;
    result.directory = source_path.parent_path();
    proto::TextContext                                          context;
    std::unordered_map<std::uint32_t, std::vector<std::string>> names;
    std::unordered_map<proto::NodeId, std::uint32_t>            nodes;
    std::uint32_t                                               next_def = 1;
    context.param_names = [&] (std::uint32_t def) -> const std::vector<std::string>* {
        const auto found = names.find (def);
        return found == names.end() ? nullptr : &found->second;
    };
    context.node_def = [&] (proto::NodeId node) -> std::optional<std::uint32_t> {
        const auto found = nodes.find (node);
        return found == nodes.end() ? std::nullopt : std::optional (found->second);
    };
    context.read_file = [&] (std::string_view path) {
        return read_text (result.directory / std::filesystem::path (path));
    };

    std::size_t start       = 0;
    std::size_t line        = 1;
    std::size_t start_line  = 1;
    unsigned    braces      = 0;
    bool        quoted      = false;
    bool        escaped     = false;
    bool        comment     = false;
    const auto  token_space = [] (char ch) {
        return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
    };
    for (std::size_t position = 0; position <= text.size(); ++position) {
        const char ch = position == text.size() ? '\n' : text[position];
        if (ch == '\n') {
            comment = false;
            if (braces == 0 || position == text.size()) {
                const auto length   = position - start + (position < text.size() ? 1 : 0);
                auto       messages = proto::assemble (text.substr (start, length), context);
                if (!messages) {
                    auto text_error = messages.error();
                    text_error.line += start_line - 1;
                    return error (proto::to_string (text_error), result.messages.size() + 1);
                }
                for (auto& message : *messages) {
                    if (const auto* call = std::get_if<proto::Call> (&message.body)) {
                        if (const auto* load = std::get_if<proto::DefLoad> (&call->command)) {
                            if (const auto graph = proto::decode_graph (load->graph)) {
                                auto& params = names[next_def];
                                for (const auto& param : graph->params) {
                                    params.push_back (param.name);
                                }
                            }
                            ++next_def;
                        } else if (std::holds_alternative<proto::ClientReset> (call->command)) {
                            nodes.clear();
                        }
                    } else if (const auto* bundle = std::get_if<proto::Bundle> (&message.body)) {
                        for (const auto& command : bundle->commands) {
                            if (const auto* node = std::get_if<proto::NodeNew> (&command)) {
                                nodes[node->id] = node->def;
                            } else if (const auto* freed =
                                           std::get_if<proto::NodeFree> (&command)) {
                                for (const auto id : freed->ids) {
                                    nodes.erase (id);
                                }
                            }
                        }
                    }
                    result.messages.push_back (std::move (message));
                }
                start      = position + 1;
                start_line = line + 1;
            }
            ++line;
            continue;
        }
        if (comment) {
            continue;
        }
        if (quoted) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                quoted = false;
            }
        } else if (ch == '"') {
            quoted = true;
        } else if (ch == '#') {
            comment = true;
        } else if ((ch == '{' || ch == '}') &&
                   (position == 0 || token_space (text[position - 1])) &&
                   (position + 1 == text.size() || token_space (text[position + 1]) ||
                    text[position + 1] == '#')) {
            if (ch == '{') {
                ++braces;
            } else if (braces != 0) {
                --braces;
            }
        }
    }
    if (auto valid = validate_score (result); !valid) {
        return std::unexpected (valid.error());
    }
    return result;
}

std::expected<Score, ScoreError>
decode_score (std::span<const std::byte> bytes) {
    if (bytes.size() < 16) {
        return error ("truncated score header");
    }
    if (little_endian (bytes.first (4)) != 0x43535642U) {
        return error ("invalid score magic");
    }
    if (little_endian (bytes.subspan (4, 2)) != proto::kVersionMajor ||
        little_endian (bytes.subspan (6, 2)) != proto::kVersionMinor) {
        return error ("incompatible score protocol version");
    }
    if (little_endian (bytes.subspan (8, 8)) != proto::catalog_hash()) {
        return error ("score unit catalog hash mismatch");
    }
    Score       score;
    std::size_t offset = 16;
    while (offset < bytes.size()) {
        if (bytes.size() - offset < 4) {
            return error ("truncated score message length", score.messages.size() + 1);
        }
        const auto length = little_endian (bytes.subspan (offset, 4));
        if (length < proto::kHeaderSize || length > proto::kMaxMessage ||
            length > bytes.size() - offset) {
            return error ("invalid or truncated score message length", score.messages.size() + 1);
        }
        auto message = proto::decode (bytes.subspan (offset, static_cast<std::size_t> (length)));
        if (!message) {
            return error (std::string (message.error().detail), score.messages.size() + 1);
        }
        score.messages.push_back (std::move (*message));
        offset += static_cast<std::size_t> (length);
    }
    if (auto valid = validate_score (score); !valid) {
        return std::unexpected (valid.error());
    }
    return score;
}

std::expected<std::vector<std::byte>, ScoreError>
encode_score (const Score& score) {
    if (auto valid = validate_score (score); !valid) {
        return std::unexpected (valid.error());
    }
    std::vector<std::byte> bytes;
    append_integer (bytes, 0x43535642U, 4);
    append_integer (bytes, proto::kVersionMajor, 2);
    append_integer (bytes, proto::kVersionMinor, 2);
    append_integer (bytes, proto::catalog_hash(), 8);
    for (const auto& message : score.messages) {
        const auto encoded = proto::encode (message);
        bytes.insert (bytes.end(), encoded->begin(), encoded->end());
    }
    return bytes;
}

std::expected<Score, ScoreError>
read_score (const std::filesystem::path& path) {
    const auto contents = read_text (path);
    if (!contents) {
        return error (contents.error());
    }
    if (path.extension() == ".bvsb") {
        auto score = decode_score (std::as_bytes (std::span (*contents)));
        if (score) {
            score->directory = path.parent_path();
        }
        return score;
    }
    if (path.extension() != ".bvs") {
        return error ("score extension must be .bvs or .bvsb");
    }
    return parse_score (*contents, path);
}

std::expected<RenderResult, ScoreError>
render_score (Engine& engine, const Score& score, AudioBackend& backend, RenderOptions options) {
    if (auto valid = validate_score (score); !valid) {
        return std::unexpected (valid.error());
    }
    const auto& config = engine.config();
    const auto& output = backend.output_config();
    if (engine.frame() != 0) {
        return error ("score rendering requires an engine at frame zero");
    }
    if (output.sample_rate != config.sample_rate || output.channels != config.out_channels ||
        output.block_size == 0) {
        return error ("backend format does not match engine output");
    }
    const auto valid_seconds = [] (double seconds) {
        return std::isfinite (seconds) && seconds >= 0 && seconds <= kMaxRenderSeconds;
    };
    if (!valid_seconds (options.tail_seconds) ||
        (options.duration_seconds && !valid_seconds (*options.duration_seconds))) {
        return error ("render duration must be finite and between 0 and 86400 seconds");
    }
    const TimeMap time (config.sample_rate);
    std::uint64_t last_frame = 0;
    for (const auto& message : score.messages) {
        if (const auto* bundle = std::get_if<proto::Bundle> (&message.body);
            bundle && bundle->time != 0) {
            if (static_cast<long double> (bundle->time) > kMaxRenderSeconds * 1'000'000'000.0L) {
                return error ("score time exceeds maximum render duration");
            }
            last_frame = time.frame_at (bundle->time);
        }
    }
    const auto duration_frames = [&] (double seconds) {
        return time.frame_at (static_cast<std::int64_t> (std::llround (seconds * 1.0e9)));
    };
    const auto end = options.duration_seconds ? duration_frames (*options.duration_seconds) :
                                                last_frame + duration_frames (options.tail_seconds);
    if (end < last_frame || end > duration_frames (kMaxRenderSeconds)) {
        return error (
            "render duration must include all score bundles and stay within 86400 seconds"
        );
    }
    std::vector<float> buffer (static_cast<std::size_t> (output.block_size) * output.channels);
    const auto         render_until =
        [&] (std::uint64_t target, std::size_t index) -> std::expected<void, ScoreError> {
        while (engine.frame() < target) {
            const auto count = static_cast<std::uint32_t> (
                std::min<std::uint64_t> (target - engine.frame(), output.block_size)
            );
            const auto block =
                std::span (buffer).first (static_cast<std::size_t> (count) * output.channels);
            engine.process (block, count);
            if (auto notifications = check_notifications (engine, index); !notifications) {
                return notifications;
            }
            if (auto written = backend.write (block); !written) {
                return error (written.error().message, index);
            }
        }
        return {};
    };
    for (std::size_t index = 0; index < score.messages.size(); ++index) {
        const auto& message = score.messages[index];
        if (const auto* call = std::get_if<proto::Call> (&message.body)) {
            auto command = call->command;
            if (auto* load = std::get_if<proto::BufLoad> (&command);
                load && !score.directory.empty() &&
                std::filesystem::path (load->path).is_relative()) {
                load->path = (score.directory / load->path).string();
            }
            if (auto result = engine.call (1, command); !result) {
                return error (result.error().message, index + 1);
            }
        } else {
            auto       bundle = std::get<proto::Bundle> (message.body);
            const auto target = bundle.time == 0 ? engine.frame() : time.frame_at (bundle.time);
            if (auto rendered = render_until (target, index + 1); !rendered) {
                return std::unexpected (rendered.error());
            }
            bundle.time = time.ns_at (engine.frame());
            if (auto submitted = engine.submit (1, message.seq, bundle); !submitted) {
                return error (submitted.error().message, index + 1);
            }
            engine.process ({}, 0);
        }
        if (auto notifications = check_notifications (engine, index + 1); !notifications) {
            return std::unexpected (notifications.error());
        }
    }
    if (auto rendered = render_until (end, score.messages.size()); !rendered) {
        return std::unexpected (rendered.error());
    }
    if (auto finished = backend.finish(); !finished) {
        return error (finished.error().message);
    }
    return RenderResult{ engine.frame() };
}

}  // namespace bitvm
