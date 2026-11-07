#pragma once

/// @file
/// @brief GraphDef: a dsp graph, its binary codec and its validator (docs/protocol.md section 13).
///
/// Responsibility: describe an instrument or effect as data and decide whether it is acceptable.
/// There is one validator, used by sona before sending and by bitvm on `def.load`, so both
/// sides agree on what a valid graph is.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "bitvm/proto/types.hpp"

namespace bitvm::proto
{

/// @brief what an input of a unit is connected to
enum class InputSource : std::uint8_t {
    constant = 0,  ///< index into GraphDef::consts
    param    = 1,  ///< index into GraphDef::params
    unit     = 2,  ///< index of an earlier unit, plus the output number
};

/// @brief a named parameter of the graph with its default value (`param`, section 13).
struct GraphParam {
    std::string name;
    float       default_value = 0.0F;

    bool operator== (const GraphParam&) const = default;
};

/// @brief one input of a unit (`input`, section 13).
struct GraphInput {
    InputSource   src    = InputSource::constant;
    std::uint16_t index  = 0;
    std::uint8_t  output = 0;  ///< output number, only for `src == unit`

    bool operator== (const GraphInput&) const = default;
};

/// @brief one unit of the graph: a catalog type, a rate, its outputs and inputs.
struct GraphUnit {
    std::uint16_t           type  = 0;  ///< index into GraphDef::types
    Rate                    rate  = Rate::kr;
    std::uint8_t            n_out = 1;
    std::vector<GraphInput> inputs;

    bool operator== (const GraphUnit&) const = default;
};

/// @brief a whole graph.
///
/// The in-memory form mirrors the wire form (shared tables of types and constants) and may be
/// non-canonical; encode_graph() always writes the canonical form.
struct GraphDef {
    std::string              name;
    std::vector<GraphParam>  params;
    std::vector<float>       consts;
    std::vector<std::string> types;
    std::vector<GraphUnit>   units;

    bool operator== (const GraphDef&) const = default;
};

/// @brief why a graph was refused; every failure maps to the `graph` error code
struct GraphError {
    std::string message;  ///< goes into the reply to `def.load`
};

/// @brief the canonical form: types and constants without repeats, in order of first use
///        (constants compared bit by bit), unused ones dropped; params keep their order.
/// @return the graph, or an error for a dangling type or constant reference
[[nodiscard]] std::expected<GraphDef, GraphError> canonicalize (const GraphDef& graph);

/// @brief canonicalizes, then writes the binary form. Does not run the validator.
[[nodiscard]] std::expected<std::vector<std::byte>, GraphError> encode_graph (
    const GraphDef& graph
);

/// @brief reads the binary form. Checks the structure only: magic, version, truncation, enums,
///        name syntax, trailing bytes. Non-canonical graphs and dangling indices are accepted,
///        the validator rejects the latter.
[[nodiscard]] std::expected<GraphDef, GraphError> decode_graph (std::span<const std::byte> bytes);

/// @brief the semantic checks of section 13, rules 2-6.
[[nodiscard]] std::expected<void, GraphError> validate (const GraphDef& graph);

/// @brief decode_graph() followed by validate(): what bitvm calls on `def.load`.
[[nodiscard]] std::expected<GraphDef, GraphError> load_graph (std::span<const std::byte> bytes);

}  // namespace bitvm::proto
