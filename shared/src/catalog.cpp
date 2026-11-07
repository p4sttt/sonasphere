#include "bitvm/proto/catalog.hpp"

#include <array>
#include <charconv>

namespace bitvm::proto
{
namespace
{

constexpr std::uint8_t kIr = 1U << static_cast<unsigned> (Rate::ir);
constexpr std::uint8_t kKr = 1U << static_cast<unsigned> (Rate::kr);
constexpr std::uint8_t kAr = 1U << static_cast<unsigned> (Rate::ar);

// the tables below are laid out by hand: one argument per line, labelled, so that a reader
// sees what each value means. clang-format would pack them.
// clang-format off

constexpr InputSpec kBinaryInputs[] = {
    {
        /* name: */          "a",
        /* required: */      true,
        /* default_value: */ 0.0F,
        /* read: */          InputRead::unit,
    },
    {
        /* name: */          "b",
        /* required: */      true,
        /* default_value: */ 0.0F,
        /* read: */          InputRead::unit,
    },
};

constexpr InputSpec kSinOscInputs[] = {
    {
        /* name: */          "freq",
        /* required: */      false,
        /* default_value: */ 440.0F,
        /* read: */          InputRead::unit,
    },
    {
        /* name: */          "phase",
        /* required: */      false,
        /* default_value: */ 0.0F,
        /* read: */          InputRead::ir,
    },
};

constexpr InputSpec kEnvPercInputs[] = {
    {
        /* name: */          "attack",
        /* required: */      false,
        /* default_value: */ 0.01F,
        /* read: */          InputRead::ir,
    },
    {
        /* name: */          "release",
        /* required: */      false,
        /* default_value: */ 1.0F,
        /* read: */          InputRead::ir,
    },
    {
        /* name: */          "level",
        /* required: */      false,
        /* default_value: */ 1.0F,
        /* read: */          InputRead::ir,
    },
    {
        /* name: */          "curve",
        /* required: */      false,
        /* default_value: */ -4.0F,
        /* read: */          InputRead::ir,
    },
    {
        /* name: */          "done",
        /* required: */      false,
        /* default_value: */ 0.0F,
        /* read: */          InputRead::ir,
    },
};

constexpr InputSpec kEnvAsrInputs[] = {
    {
        /* name: */          "gate",
        /* required: */      false,
        /* default_value: */ 1.0F,
        /* read: */          InputRead::kr,
    },
    {
        /* name: */          "attack",
        /* required: */      false,
        /* default_value: */ 0.01F,
        /* read: */          InputRead::ir,
    },
    {
        /* name: */          "level",
        /* required: */      false,
        /* default_value: */ 1.0F,
        /* read: */          InputRead::ir,
    },
    {
        /* name: */          "release",
        /* required: */      false,
        /* default_value: */ 0.3F,
        /* read: */          InputRead::ir,
    },
    {
        /* name: */          "done",
        /* required: */      false,
        /* default_value: */ 0.0F,
        /* read: */          InputRead::ir,
    },
};

constexpr InputSpec kOutInputs[] = {
    {
        /* name: */          "bus",
        /* required: */      true,
        /* default_value: */ 0.0F,
        /* read: */          InputRead::kr,
    },
};

constexpr InputSpec kInInputs[] = {
    {
        /* name: */          "bus",
        /* required: */      true,
        /* default_value: */ 0.0F,
        /* read: */          InputRead::kr,
    },
};

constexpr std::array kUnits = {
    UnitSpec{
        /* name: */        "add",
        /* rates: */       kIr | kKr | kAr,
        /* inputs: */      kBinaryInputs,
        /* variadic: */    std::nullopt,
        /* min_outputs: */ 1,
        /* max_outputs: */ 1,
        /* done: */        false,
        /* description: */ "a + b",
    },
    UnitSpec{
        /* name: */        "sub",
        /* rates: */       kIr | kKr | kAr,
        /* inputs: */      kBinaryInputs,
        /* variadic: */    std::nullopt,
        /* min_outputs: */ 1,
        /* max_outputs: */ 1,
        /* done: */        false,
        /* description: */ "a - b",
    },
    UnitSpec{
        /* name: */        "mul",
        /* rates: */       kIr | kKr | kAr,
        /* inputs: */      kBinaryInputs,
        /* variadic: */    std::nullopt,
        /* min_outputs: */ 1,
        /* max_outputs: */ 1,
        /* done: */        false,
        /* description: */ "a * b",
    },
    UnitSpec{
        /* name: */        "div",
        /* rates: */       kIr | kKr | kAr,
        /* inputs: */      kBinaryInputs,
        /* variadic: */    std::nullopt,
        /* min_outputs: */ 1,
        /* max_outputs: */ 1,
        /* done: */        false,
        /* description: */ "a / b, 0 when b is 0",
    },
    UnitSpec{
        /* name: */        "sin_osc",
        /* rates: */       kKr | kAr,
        /* inputs: */      kSinOscInputs,
        /* variadic: */    std::nullopt,
        /* min_outputs: */ 1,
        /* max_outputs: */ 1,
        /* done: */        false,
        /* description: */ "sine oscillator of amplitude 1, phase in turns",
    },
    UnitSpec{
        /* name: */        "env_perc",
        /* rates: */       kKr | kAr,
        /* inputs: */      kEnvPercInputs,
        /* variadic: */    std::nullopt,
        /* min_outputs: */ 1,
        /* max_outputs: */ 1,
        /* done: */        true,
        /* description: */ "percussive envelope: attack to level, then release to 0, both curved",
    },
    UnitSpec{
        /* name: */        "env_asr",
        /* rates: */       kKr | kAr,
        /* inputs: */      kEnvAsrInputs,
        /* variadic: */    std::nullopt,
        /* min_outputs: */ 1,
        /* max_outputs: */ 1,
        /* done: */        true,
        /* description: */ "attack, sustain and release envelope driven by gate",
    },
    UnitSpec{
        /* name: */        "out",
        /* rates: */       kKr | kAr,
        /* inputs: */      kOutInputs,
        /* variadic: */    VariadicSpec{
            /* name: */      "channels",
            /* read: */      InputRead::unit,
            /* min_count: */ 1,
            /* max_count: */ 64,
        },
        /* min_outputs: */ 0,
        /* max_outputs: */ 0,
        /* done: */        false,
        /* description: */ "writes channels to consecutive buses starting at bus",
    },
    UnitSpec{
        /* name: */        "in",
        /* rates: */       kKr | kAr,
        /* inputs: */      kInInputs,
        /* variadic: */    std::nullopt,
        /* min_outputs: */ 1,
        /* max_outputs: */ 64,
        /* done: */        false,
        /* description: */ "reads consecutive buses starting at bus",
    },
};

// clang-format on

std::string_view
read_name (InputRead read) {
    switch (read) {
        case InputRead::ir: {
            return "ir";
        }
        case InputRead::kr: {
            return "kr";
        }
        case InputRead::ar: {
            return "ar";
        }
        case InputRead::unit: {
            return "unit";
        }
    }
    return "?";
}

std::string
format_float (float value) {
    std::array<char, 32> buf{};
    const auto           res = std::to_chars (buf.data(), buf.data() + buf.size(), value);
    return std::string (buf.data(), res.ptr);
}

std::string
format_count_range (unsigned min_count, unsigned max_count) {
    if (min_count == max_count) {
        return std::to_string (min_count);
    }
    return std::to_string (min_count) + "-" + std::to_string (max_count);
}

/// canonical text of one unit: a block with one property per line, so that a dump of the
/// catalog stays readable and a change shows up as a small diff.
///
/// exception safety: everything that can throw (only std::bad_alloc from the string) happens
/// while this function fills its own local string. it touches no other state, so a failure
/// leaves nothing half done; the caller commits the result with a single append.
std::string
format_unit (const UnitSpec& unit) {
    std::string text = "unit " + std::string (unit.name) + " {\n";

    text += "    rates: ";
    bool first = true;
    for (const auto rate : { Rate::ir, Rate::kr, Rate::ar }) {
        if (allows_rate (unit, rate)) {
            text += first ? "" : ",";
            text += name_of (rate);
            first = false;
        }
    }
    text += "\n";

    for (const auto& input : unit.inputs) {
        text += "    input: " + std::string (input.name) +
                " read=" + std::string (read_name (input.read));
        text += input.required ? " required" : " default=" + format_float (input.default_value);
        text += "\n";
    }
    if (unit.variadic) {
        text += "    variadic: " + std::string (unit.variadic->name) +
                " read=" + std::string (read_name (unit.variadic->read)) + " count=" +
                format_count_range (unit.variadic->min_count, unit.variadic->max_count) + "\n";
    }
    text += "    outputs: " + format_count_range (unit.min_outputs, unit.max_outputs) + "\n";
    text += unit.done ? "    done: true\n" : "    done: false\n";
    text += "    description: " + std::string (unit.description) + "\n";
    text += "}\n";
    return text;
}

}  // namespace

std::span<const UnitSpec>
catalog () noexcept {
    return kUnits;
}

const UnitSpec*
find_unit (std::string_view name) noexcept {
    for (const auto& u : kUnits) {
        if (u.name == name) {
            return &u;
        }
    }
    return nullptr;
}

bool
allows_rate (const UnitSpec& unit, Rate rate) noexcept {
    const auto raw = static_cast<unsigned> (rate);
    return raw <= static_cast<unsigned> (Rate::ar) && (unit.rates & (1U << raw)) != 0;
}

std::size_t
min_inputs (const UnitSpec& unit) noexcept {
    return unit.inputs.size() + (unit.variadic ? unit.variadic->min_count : 0U);
}

std::size_t
max_inputs (const UnitSpec& unit) noexcept {
    return unit.inputs.size() + (unit.variadic ? unit.variadic->max_count : 0U);
}

bool
has_variable_outputs (const UnitSpec& unit) noexcept {
    return unit.min_outputs != unit.max_outputs;
}

Rate
effective_read (InputRead read, Rate unit_rate) noexcept {
    switch (read) {
        case InputRead::ir:
            return Rate::ir;
        case InputRead::kr:
            return Rate::kr;
        case InputRead::ar:
            return Rate::ar;
        case InputRead::unit:
            break;
    }
    return unit_rate;
}

std::string
catalog_text () {
    std::string out;
    for (const auto& unit : kUnits) {
        // building the block may throw, appending it (strong guarantee) is the commit
        const std::string block = format_unit (unit);
        out += block;
    }
    return out;
}

/// FNV-1a, 64 bit: http://www.isthe.com/chongo/tech/comp/fnv/ (the constants are the
/// published offset basis and prime of that algorithm)
std::uint64_t
catalog_hash () {
    constexpr std::uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ULL;
    constexpr std::uint64_t kFnvPrime       = 0x100000001b3ULL;

    std::uint64_t hash = kFnvOffsetBasis;
    for (const char c : catalog_text()) {
        hash ^= static_cast<std::uint8_t> (c);
        hash *= kFnvPrime;
    }
    return hash;
}

}  // namespace bitvm::proto
