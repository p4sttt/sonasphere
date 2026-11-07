#include <algorithm>
#include <array>
#include <bit>
#include <map>

#include "bitvm/proto/catalog.hpp"
#include "bitvm/proto/text.hpp"
#include "text_util.hpp"

namespace bitvm::proto
{
namespace
{

using text::Token;

constexpr std::string_view kBase64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::unexpected<TextError>
error_at (std::size_t line, std::size_t col, std::string message) {
    return std::unexpected (TextError{ line, col, std::move (message) });
}

bool
is_label_char (char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

struct UnitInfo {
    std::uint8_t n_out = 0;
};

class GraphAssembler {
public:
    std::expected<GraphDef, TextError>
    run (std::string_view source) {
        std::size_t line_no = 0;
        std::size_t pos     = 0;
        while (pos <= source.size()) {
            const auto end  = source.find ('\n', pos);
            const auto line = source.substr (pos, end == std::string_view::npos ? end : end - pos);
            ++line_no;
            pos = end == std::string_view::npos ? source.size() + 1 : end + 1;

            auto tokens = text::tokenize (line, line_no);
            if (!tokens) {
                return std::unexpected (tokens.error());
            }
            if (tokens->empty()) {
                continue;
            }
            if (auto ok = handle_line (*tokens, line_no); !ok) {
                return std::unexpected (ok.error());
            }
        }
        if (!seen_header_) {
            return error_at (line_no, 1, "missing `graph <name>` line");
        }
        auto canon = canonicalize (graph_);
        if (!canon) {
            return error_at (line_no, 1, canon.error().message);
        }
        return std::move (*canon);
    }

private:
    std::expected<void, TextError>
    handle_line (const std::vector<Token>& tokens, std::size_t line) {
        const auto& head = tokens[0];
        if (!seen_header_) {
            if (head.raw != "graph") {
                return fail (line, head.col, "expected `graph <name>`");
            }
            if (tokens.size() != 2) {
                return fail (line, head.col, "expected `graph <name>`");
            }
            auto name = text::unquote (tokens[1].raw);
            if (!name) {
                return fail (line, tokens[1].col + name.error().offset, name.error().message);
            }
            if (name->size() > kMaxString) {
                return fail (line, tokens[1].col, "graph name too long");
            }
            graph_.name  = std::move (*name);
            seen_header_ = true;
            return {};
        }
        if (head.raw == "param") {
            return handle_param (tokens, line);
        }
        return handle_unit (tokens, line);
    }

    std::expected<void, TextError>
    handle_param (const std::vector<Token>& tokens, std::size_t line) {
        if (!graph_.units.empty()) {
            return fail (line, tokens[0].col, "param after units");
        }
        if (tokens.size() != 3) {
            return fail (line, tokens[0].col, "expected `param <name> <default>`");
        }
        if (!is_valid_name (tokens[1].raw)) {
            return fail (line, tokens[1].col, "invalid parameter name");
        }
        for (const auto& p : graph_.params) {
            if (p.name == tokens[1].raw) {
                return fail (line, tokens[1].col, "duplicate parameter name");
            }
        }
        GraphParam param;
        param.name = tokens[1].raw;
        if (!text::parse_f32 (tokens[2].raw, param.default_value)) {
            return fail (line, tokens[2].col, "invalid number");
        }
        graph_.params.push_back (std::move (param));
        return {};
    }

    std::expected<void, TextError>
    handle_unit (const std::vector<Token>& tokens, std::size_t line) {
        std::size_t                  next = 0;
        std::string                  label;
        std::optional<std::uint64_t> explicit_out;
        std::size_t                  label_col = 0;
        std::size_t                  count_col = 0;

        if (tokens[0].raw.starts_with ('%')) {
            std::string spec = tokens[0].raw.substr (1);
            label_col        = tokens[0].col;
            bool has_equals  = false;
            if (spec.ends_with ('=')) {
                spec.pop_back();
                has_equals = true;
            }
            next = 1;
            if (!has_equals) {
                if (tokens.size() < 2 || tokens[1].raw != "=") {
                    return fail (line, tokens[0].col, "expected `=` after the label");
                }
                next = 2;
            }
            const auto colon = spec.find (':');
            label            = spec.substr (0, colon);
            if (label.empty() || !std::ranges::all_of (label, is_label_char)) {
                return fail (line, label_col + 1, "invalid label");
            }
            if (colon != std::string::npos) {
                std::uint64_t n = 0;
                count_col       = label_col + 1 + colon + 1;
                if (!text::parse_uint (spec.substr (colon + 1), kMaxUnitOutputs, n) || n == 0) {
                    return fail (line, count_col, "output count must be 1..64");
                }
                explicit_out = n;
            }
            if (labels_.contains (label)) {
                return fail (line, label_col, "duplicate label");
            }
        }
        if (next >= tokens.size()) {
            return fail (line, tokens.back().col, "expected `<type>.<rate>`");
        }

        // type.rate
        const auto& type_token = tokens[next];
        const auto  dot        = type_token.raw.rfind ('.');
        if (dot == std::string::npos) {
            return fail (line, type_token.col, "expected `<type>.<rate>`");
        }
        const auto  type_name = type_token.raw.substr (0, dot);
        const auto* spec      = find_unit (type_name);
        if (spec == nullptr) {
            return fail (line, type_token.col, "unknown unit type");
        }
        const auto rate = rate_from_name (std::string_view (type_token.raw).substr (dot + 1));
        if (!rate) {
            return fail (line, type_token.col + dot + 1, "rate must be ir, kr or ar");
        }

        GraphUnit unit;
        unit.rate = *rate;
        if (has_variable_outputs (*spec)) {
            unit.n_out =
                explicit_out ? static_cast<std::uint8_t> (*explicit_out) : spec->min_outputs;
            if (unit.n_out < spec->min_outputs || unit.n_out > spec->max_outputs) {
                return fail (line, count_col, "output count outside the range of the unit");
            }
        } else {
            if (explicit_out) {
                return fail (line, count_col, "this unit has a fixed number of outputs");
            }
            unit.n_out = spec->min_outputs;
        }

        const std::span<const Token> args (tokens.data() + next + 1, tokens.size() - next - 1);
        const auto                   n_fixed = spec->inputs.size();
        if (args.size() > max_inputs (*spec)) {
            return fail (line, args[max_inputs (*spec)].col, "too many arguments");
        }
        const auto end_col = tokens.back().col + tokens.back().raw.size();
        for (std::size_t i = 0; i < n_fixed; ++i) {
            if (i < args.size()) {
                auto input = make_input (args[i], line);
                if (!input) {
                    return std::unexpected (input.error());
                }
                unit.inputs.push_back (*input);
            } else if (spec->inputs[i].required) {
                return fail (
                    line,
                    end_col,
                    "missing required argument `" + std::string (spec->inputs[i].name) + "`"
                );
            } else {
                auto input = constant_input (spec->inputs[i].default_value, line, end_col);
                if (!input) {
                    return std::unexpected (input.error());
                }
                unit.inputs.push_back (*input);
            }
        }
        if (spec->variadic) {
            const std::size_t rest = args.size() > n_fixed ? args.size() - n_fixed : 0;
            if (rest < spec->variadic->min_count) {
                return fail (
                    line,
                    end_col,
                    "missing argument `" + std::string (spec->variadic->name) + "`"
                );
            }
            for (std::size_t i = n_fixed; i < args.size(); ++i) {
                auto input = make_input (args[i], line);
                if (!input) {
                    return std::unexpected (input.error());
                }
                unit.inputs.push_back (*input);
            }
        }

        unit.type = type_index (std::string (spec->name));
        if (!label.empty()) {
            labels_[label] = graph_.units.size();
        }
        units_.push_back (UnitInfo{ unit.n_out });
        graph_.units.push_back (std::move (unit));
        return {};
    }

    std::expected<GraphInput, TextError>
    constant_input (float value, std::size_t line, std::size_t col) {
        const auto bits = std::bit_cast<std::uint32_t> (value);
        if (const auto it = constants_.find (bits); it != constants_.end()) {
            return GraphInput{ InputSource::constant, it->second, 0 };
        }
        if (graph_.consts.size() >= 0xFFFF) {
            return error_at (line, col, "too many constants");
        }
        const auto index = static_cast<std::uint16_t> (graph_.consts.size());
        graph_.consts.push_back (value);
        constants_.emplace (bits, index);
        return GraphInput{ InputSource::constant, index, 0 };
    }

    std::uint16_t
    type_index (const std::string& name) {
        const auto it = std::ranges::find (graph_.types, name);
        if (it != graph_.types.end()) {
            return static_cast<std::uint16_t> (it - graph_.types.begin());
        }
        graph_.types.push_back (name);
        return static_cast<std::uint16_t> (graph_.types.size() - 1);
    }

    std::expected<GraphInput, TextError>
    make_input (const Token& arg, std::size_t line) {
        const std::string_view raw = arg.raw;
        GraphInput             in;
        if (raw.starts_with ('$')) {
            const auto    key   = raw.substr (1);
            std::uint64_t index = 0;
            if (!key.empty() && key[0] >= '0' && key[0] <= '9') {
                if (!text::parse_uint (key, 0xFFFF, index) || index >= graph_.params.size()) {
                    return error_at (line, arg.col, "parameter index out of range");
                }
            } else {
                const auto it = std::ranges::find (graph_.params, key, &GraphParam::name);
                if (it == graph_.params.end()) {
                    return error_at (line, arg.col, "unknown parameter");
                }
                index = static_cast<std::uint64_t> (it - graph_.params.begin());
            }
            in.src   = InputSource::param;
            in.index = static_cast<std::uint16_t> (index);
            return in;
        }
        if (raw.starts_with ('%')) {
            auto          ref = raw.substr (1);
            std::uint64_t k   = 0;
            const auto    dot = ref.find ('.');
            if (dot != std::string_view::npos) {
                if (!text::parse_uint (ref.substr (dot + 1), 0xFF, k)) {
                    return error_at (line, arg.col + 1 + dot + 1, "invalid output number");
                }
                ref = ref.substr (0, dot);
            }
            const auto it = labels_.find (std::string (ref));
            if (it == labels_.end()) {
                return error_at (line, arg.col, "unknown label");
            }
            if (k >= units_[it->second].n_out) {
                return error_at (line, arg.col, "the unit has no such output");
            }
            in.src    = InputSource::unit;
            in.index  = static_cast<std::uint16_t> (it->second);
            in.output = static_cast<std::uint8_t> (k);
            return in;
        }
        float value = 0.0F;
        if (!text::parse_f32 (raw, value)) {
            return error_at (line, arg.col, "expected a number, $param or %label");
        }
        return constant_input (value, line, arg.col);
    }

    static std::unexpected<TextError>
    fail (std::size_t line, std::size_t col, std::string msg) {
        return error_at (line, col, std::move (msg));
    }

    GraphDef                               graph_;
    bool                                   seen_header_ = false;
    std::map<std::string, std::size_t>     labels_;
    std::vector<UnitInfo>                  units_;
    std::map<std::uint32_t, std::uint16_t> constants_;
};

std::string
format_ref (const GraphInput& in, const GraphDef& graph) {
    switch (in.src) {
        case InputSource::constant: {
            return in.index < graph.consts.size() ? text::format_f32 (graph.consts[in.index]) : "?";
        }
        case InputSource::param: {
            return in.index < graph.params.size() ? "$" + graph.params[in.index].name : "$?";
        }
        case InputSource::unit: {
            std::string s = "%" + std::to_string (in.index);
            if (in.output != 0) {
                s += "." + std::to_string (in.output);
            }
            return s;
        }
    }
    return "?";
}

}  // namespace

std::expected<GraphDef, TextError>
assemble_graph (std::string_view text) {
    GraphAssembler assembler;
    return assembler.run (text);
}

std::string
disassemble_graph (const GraphDef& graph) {
    std::string out = "graph " + text::format_str (graph.name) + "\n";
    for (const auto& p : graph.params) {
        out += "param " + p.name + " " + text::format_f32 (p.default_value) + "\n";
    }
    out += "\n";
    for (std::size_t i = 0; i < graph.units.size(); ++i) {
        const auto&       u    = graph.units[i];
        const std::string type = u.type < graph.types.size() ? graph.types[u.type] : "?";
        const auto*       spec = find_unit (type);
        if (u.n_out > 0) {
            out += "%" + std::to_string (i);
            if (spec != nullptr && has_variable_outputs (*spec)) {
                out += ":" + std::to_string (u.n_out);
            }
            out += " = ";
        }
        out += type + "." + std::string (name_of (u.rate));
        for (const auto& in : u.inputs) {
            out += " " + format_ref (in, graph);
        }
        out += "\n";
    }
    return out;
}

std::string
base64_encode (std::span<const std::byte> bytes) {
    std::string out;
    out.reserve ((bytes.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const auto b0 = static_cast<std::uint32_t> (bytes[i]);
        const auto b1 = i + 1 < bytes.size() ? static_cast<std::uint32_t> (bytes[i + 1]) : 0U;
        const auto b2 = i + 2 < bytes.size() ? static_cast<std::uint32_t> (bytes[i + 2]) : 0U;
        const auto v  = (b0 << 16) | (b1 << 8) | b2;
        out += kBase64Alphabet[(v >> 18) & 63];
        out += kBase64Alphabet[(v >> 12) & 63];
        out += i + 1 < bytes.size() ? kBase64Alphabet[(v >> 6) & 63] : '=';
        out += i + 2 < bytes.size() ? kBase64Alphabet[v & 63] : '=';
    }
    return out;
}

std::optional<std::vector<std::byte>>
base64_decode (std::string_view text) {
    if (text.size() % 4 != 0) {
        return std::nullopt;
    }
    std::vector<std::byte> out;
    out.reserve (text.size() / 4 * 3);
    for (std::size_t i = 0; i < text.size(); i += 4) {
        std::uint32_t v   = 0;
        std::size_t   pad = 0;
        for (std::size_t k = 0; k < 4; ++k) {
            const char c = text[i + k];
            if (c == '=') {
                // padding only in the last two positions of the last group
                if (i + 4 != text.size() || k < 2) {
                    return std::nullopt;
                }
                ++pad;
                v <<= 6;
                continue;
            }
            if (pad != 0) {
                return std::nullopt;
            }
            const auto pos = kBase64Alphabet.find (c);
            if (pos == std::string_view::npos) {
                return std::nullopt;
            }
            v = (v << 6) | static_cast<std::uint32_t> (pos);
        }
        // bits that do not belong to an output byte must be zero, so that the text of
        // a byte string is unique
        if ((pad == 2 && (v & 0xFFFF) != 0) || (pad == 1 && (v & 0xFF) != 0)) {
            return std::nullopt;
        }
        out.push_back (static_cast<std::byte> ((v >> 16) & 0xFF));
        if (pad < 2) {
            out.push_back (static_cast<std::byte> ((v >> 8) & 0xFF));
        }
        if (pad < 1) {
            out.push_back (static_cast<std::byte> (v & 0xFF));
        }
    }
    return out;
}

}  // namespace bitvm::proto
