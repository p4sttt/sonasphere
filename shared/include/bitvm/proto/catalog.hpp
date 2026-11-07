#pragma once

/// @file
/// @brief The static unit catalog (docs/protocol.md section 14), shared by sona and bitvm.
///
/// Responsibility: say which units exist and what their signatures are. sona builds graphs
/// against it without a running server; bitvm checks at start that it implements every entry.
/// A catalog change needs a new minor protocol version and a new pinned hash in the tests.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "bitvm/proto/types.hpp"

namespace bitvm::proto
{

/// @brief how a unit reads an input; `unit` means the rate of the unit itself.
enum class InputRead : std::uint8_t {
    ir   = 0,
    kr   = 1,
    ar   = 2,
    unit = 3,
};

/// @brief a named input of a catalog unit
struct InputSpec {
    std::string_view name;
    bool             required      = false;
    float            default_value = 0.0F;  ///< meaningless when required
    InputRead        read          = InputRead::unit;
};

/// @brief a trailing input that repeats between `min_count` and `max_count` times
struct VariadicSpec {
    std::string_view name;
    InputRead        read      = InputRead::unit;
    std::uint8_t     min_count = 1;
    std::uint8_t     max_count = 1;
};

/// @brief the signature of one unit type
struct UnitSpec {
    std::string_view            name;
    std::uint8_t                rates = 0;  ///< bit `1 << rate` per allowed rate
    std::span<const InputSpec>  inputs;
    std::optional<VariadicSpec> variadic;
    std::uint8_t                min_outputs = 1;
    std::uint8_t                max_outputs = 1;
    bool                        done        = false;  ///< can finish and free the node
    std::string_view            description;
};

/// @brief all units of the catalog
[[nodiscard]] std::span<const UnitSpec> catalog () noexcept;

/// @brief the unit with this name, or nullptr
[[nodiscard]] const UnitSpec* find_unit (std::string_view name) noexcept;

/// @brief true if the unit may run at `rate`
[[nodiscard]] bool allows_rate (const UnitSpec& unit, Rate rate) noexcept;

/// @brief fewest inputs a graph may give the unit (fixed inputs plus the variadic minimum)
[[nodiscard]] std::size_t min_inputs (const UnitSpec& unit) noexcept;

/// @brief most inputs a graph may give the unit
[[nodiscard]] std::size_t max_inputs (const UnitSpec& unit) noexcept;

/// @brief true if the number of outputs is chosen by the graph (`:n` in the text form)
[[nodiscard]] bool has_variable_outputs (const UnitSpec& unit) noexcept;

/// @brief the rate at which an input with mode `read` is read in a unit running at `unit_rate`
[[nodiscard]] Rate effective_read (InputRead read, Rate unit_rate) noexcept;

/// @brief canonical text of the whole catalog, one block per unit.
/// @details the exact text is internal; it is pinned by tests/data/catalog.golden
[[nodiscard]] std::string catalog_text ();

/// @brief FNV-1a 64 of catalog_text(); sent in the handshake to catch mismatched builds
[[nodiscard]] std::uint64_t catalog_hash ();

}  // namespace bitvm::proto
