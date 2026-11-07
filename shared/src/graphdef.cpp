#include "bitvm/proto/graphdef.hpp"

#include <algorithm>
#include <bit>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "bitvm/proto/catalog.hpp"
#include "wire.hpp"

namespace bitvm::proto
{
namespace
{

using wire::Reader;
using wire::Writer;

std::unexpected<GraphError>
fail (std::string message) {
    return std::unexpected (GraphError{ std::move (message) });
}

std::string
unit_prefix (std::size_t index) {
    return "unit " + std::to_string (index) + ": ";
}

}  // namespace

std::expected<GraphDef, GraphError>
canonicalize (const GraphDef& graph) {
    GraphDef out;
    out.name   = graph.name;
    out.params = graph.params;
    out.units.reserve (graph.units.size());

    std::unordered_map<std::string, std::uint16_t>   type_index;
    std::unordered_map<std::uint32_t, std::uint16_t> const_index;

    for (std::size_t i = 0; i < graph.units.size(); ++i) {
        const auto& unit = graph.units[i];
        if (unit.type >= graph.types.size()) {
            return fail (unit_prefix (i) + "type index out of range");
        }
        GraphUnit   copy      = unit;
        const auto& type_name = graph.types[unit.type];
        auto [tit, new_type]  = type_index.try_emplace (type_name, 0);
        if (new_type) {
            if (out.types.size() >= 0xFFFF) {
                return fail ("too many unit types");
            }
            tit->second = static_cast<std::uint16_t> (out.types.size());
            out.types.push_back (type_name);
        }
        copy.type = tit->second;
        for (std::size_t k = 0; k < copy.inputs.size(); ++k) {
            auto& input = copy.inputs[k];
            if (input.src != InputSource::constant) {
                continue;
            }
            if (input.index >= graph.consts.size()) {
                return fail (unit_prefix (i) + "constant index out of range");
            }
            const auto value      = graph.consts[input.index];
            const auto bits       = std::bit_cast<std::uint32_t> (value);
            auto [cit, new_const] = const_index.try_emplace (bits, 0);
            if (new_const) {
                if (out.consts.size() >= 0xFFFF) {
                    return fail ("too many constants");
                }
                cit->second = static_cast<std::uint16_t> (out.consts.size());
                out.consts.push_back (value);
            }
            input.index = cit->second;
        }
        out.units.push_back (std::move (copy));
    }
    return out;
}

std::expected<std::vector<std::byte>, GraphError>
encode_graph (const GraphDef& graph) {
    auto canon = canonicalize (graph);
    if (!canon) {
        return std::unexpected (canon.error());
    }
    std::vector<std::byte> out;
    Writer                 w (out);
    w.u32 (kGraphMagic);
    w.u16 (kGraphVersion);
    w.str (canon->name);
    w.count (canon->params.size());
    for (const auto& p : canon->params) {
        w.name (p.name);
        w.f32 (p.default_value);
    }
    w.count (canon->consts.size());
    for (const auto c : canon->consts) {
        w.f32 (c);
    }
    w.count (canon->types.size());
    for (const auto& t : canon->types) {
        w.name (t);
    }
    w.count (canon->units.size());
    for (const auto& u : canon->units) {
        w.u16 (u.type);
        w.u8 (static_cast<std::uint8_t> (u.rate));
        w.u8 (u.n_out);
        if (u.inputs.size() > 0xFF) {
            w.fail ("too many unit inputs");
        }
        w.u8 (static_cast<std::uint8_t> (u.inputs.size()));
        for (const auto& in : u.inputs) {
            w.u8 (static_cast<std::uint8_t> (in.src));
            w.u16 (in.index);
            w.u8 (in.output);
        }
    }
    if (!w.ok()) {
        return fail ("encode: " + std::string (w.detail()));
    }
    return out;
}

std::expected<GraphDef, GraphError>
decode_graph (std::span<const std::byte> bytes) {
    Reader   r (bytes, 0);
    GraphDef g;
    // never reserve more than the remaining bytes can hold
    const auto reserve = [&r] (auto& vec, std::size_t n, std::size_t min_size) {
        vec.reserve (std::min (n, r.remaining() / min_size));
    };

    if (r.u32() != kGraphMagic && r.ok()) {
        r.fail_back (4, ErrorCode::graph, "bad magic");
    }
    if (r.u16() != kGraphVersion && r.ok()) {
        r.fail_back (2, ErrorCode::graph, "unsupported graph version");
    }
    g.name = r.str();

    const auto n_params = r.u16();
    reserve (g.params, n_params, 7);
    for (std::size_t i = 0; i < n_params && r.ok(); ++i) {
        GraphParam p;
        p.name          = r.name();
        p.default_value = r.f32();
        g.params.push_back (std::move (p));
    }

    const auto n_consts = r.u16();
    reserve (g.consts, n_consts, 4);
    for (std::size_t i = 0; i < n_consts && r.ok(); ++i) {
        g.consts.push_back (r.f32());
    }

    const auto n_types = r.u16();
    reserve (g.types, n_types, 3);
    for (std::size_t i = 0; i < n_types && r.ok(); ++i) {
        g.types.push_back (r.name());
    }

    const auto n_units = r.u16();
    reserve (g.units, n_units, 5);
    for (std::size_t i = 0; i < n_units && r.ok(); ++i) {
        GraphUnit u;
        u.type          = r.u16();
        const auto rate = rate_from_wire (r.u8());
        if (r.ok() && !rate) {
            r.fail_back (1, ErrorCode::graph, "invalid rate");
        }
        u.rate          = rate.value_or (Rate::kr);
        u.n_out         = r.u8();
        const auto n_in = r.u8();
        reserve (u.inputs, n_in, 4);
        for (std::size_t k = 0; k < n_in && r.ok(); ++k) {
            GraphInput in;
            const auto src = r.u8();
            if (r.ok() && src > static_cast<std::uint8_t> (InputSource::unit)) {
                r.fail_back (1, ErrorCode::graph, "invalid input source");
            }
            in.src    = static_cast<InputSource> (src);
            in.index  = r.u16();
            in.output = r.u8();
            u.inputs.push_back (in);
        }
        g.units.push_back (std::move (u));
    }

    if (r.ok() && r.remaining() != 0) {
        r.fail (ErrorCode::graph, "bytes after units");
    }
    if (!r.ok()) {
        const auto& f = r.failure();
        return fail (
            "graphdef: " + std::string (f.detail) + " at byte " + std::to_string (f.offset)
        );
    }
    return g;
}

std::expected<void, GraphError>
validate (const GraphDef& graph) {
    // rule 2 and the name part of rule 3
    std::unordered_set<std::string_view> names;
    for (const auto& p : graph.params) {
        if (!is_valid_name (p.name)) {
            return fail ("param \"" + p.name + "\": invalid name");
        }
        if (!names.insert (p.name).second) {
            return fail ("param \"" + p.name + "\": duplicate name");
        }
        if (!std::isfinite (p.default_value)) {
            return fail ("param \"" + p.name + "\": default is not finite");
        }
    }
    // rule 3
    if (graph.params.size() > kMaxGraphParams) {
        return fail ("too many params (max 256)");
    }
    if (graph.consts.size() > kMaxGraphConsts) {
        return fail ("too many constants (max 4096)");
    }
    if (graph.units.size() > kMaxGraphUnits) {
        return fail ("too many units (max 4096)");
    }
    for (std::size_t i = 0; i < graph.consts.size(); ++i) {
        if (!std::isfinite (graph.consts[i])) {
            return fail ("constant " + std::to_string (i) + " is not finite");
        }
    }
    for (std::size_t i = 0; i < graph.types.size(); ++i) {
        if (!is_valid_name (graph.types[i])) {
            return fail ("type " + std::to_string (i) + ": invalid name");
        }
    }

    for (std::size_t i = 0; i < graph.units.size(); ++i) {
        const auto& u = graph.units[i];
        if (u.inputs.size() > kMaxUnitInputs) {
            return fail (unit_prefix (i) + "too many inputs (max 65)");
        }
        if (u.n_out > kMaxUnitOutputs) {
            return fail (unit_prefix (i) + "too many outputs (max 64)");
        }
        // rule 4
        if (u.type >= graph.types.size()) {
            return fail (unit_prefix (i) + "type index out of range");
        }
        const auto& type_name = graph.types[u.type];
        const auto* spec      = find_unit (type_name);
        if (spec == nullptr) {
            return fail (unit_prefix (i) + "unknown unit type \"" + type_name + "\"");
        }
        if (!allows_rate (*spec, u.rate)) {
            return fail (
                unit_prefix (i) + type_name + " does not support rate " +
                std::string (name_of (u.rate))
            );
        }
        if (u.n_out < spec->min_outputs || u.n_out > spec->max_outputs) {
            return fail (unit_prefix (i) + type_name + ": wrong number of outputs");
        }
        if (u.inputs.size() < min_inputs (*spec) || u.inputs.size() > max_inputs (*spec)) {
            return fail (unit_prefix (i) + type_name + ": wrong number of inputs");
        }
        // rules 5 and 6
        for (std::size_t k = 0; k < u.inputs.size(); ++k) {
            const auto&       in    = u.inputs[k];
            const std::string where = unit_prefix (i) + "input " + std::to_string (k) + ": ";
            switch (in.src) {
                case InputSource::constant: {
                    if (in.index >= graph.consts.size()) {
                        return fail (where + "constant index out of range");
                    }
                    if (in.output != 0) {
                        return fail (where + "output must be 0 for a constant");
                    }
                    break;
                }
                case InputSource::param: {
                    if (in.index >= graph.params.size()) {
                        return fail (where + "param index out of range");
                    }
                    if (in.output != 0) {
                        return fail (where + "output must be 0 for a param");
                    }
                    break;
                }
                case InputSource::unit: {
                    if (in.index >= i) {
                        return fail (where + "refers to a unit that is not earlier");
                    }
                    const auto& src = graph.units[in.index];
                    if (in.output >= src.n_out) {
                        return fail (where + "refers to a missing output");
                    }
                    if (u.rate == Rate::ir && src.rate != Rate::ir) {
                        return fail (where + "ir unit reads a kr or ar unit");
                    }
                    break;
                }
                default:
                    return fail (where + "invalid input source");
            }
        }
    }
    return {};
}

std::expected<GraphDef, GraphError>
load_graph (std::span<const std::byte> bytes) {
    auto graph = decode_graph (bytes);
    if (!graph) {
        return graph;
    }
    if (auto ok = validate (*graph); !ok) {
        return std::unexpected (ok.error());
    }
    return graph;
}

}  // namespace bitvm::proto
