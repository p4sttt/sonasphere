#include <gtest/gtest.h>

#include <limits>

#include "graph_fixtures.hpp"
#include "test_util.hpp"

using namespace bitvm::proto;
using namespace bitvm::proto::testing;

namespace
{

const auto kConst = [] (std::uint16_t i) {
    return GraphInput{ InputSource::constant, i, 0 };
};
const auto kParam = [] (std::uint16_t i) {
    return GraphInput{ InputSource::param, i, 0 };
};
const auto kUnit = [] (std::uint16_t i, std::uint8_t o = 0) {
    return GraphInput{ InputSource::unit, i, o };
};

// a graph with a few units to mutate
GraphDef
base () {
    return ping_graph();
}

void
expect_invalid (const GraphDef& g, const std::string& fragment) {
    auto r = validate (g);
    ASSERT_FALSE (r.has_value()) << "expected: " << fragment;
    EXPECT_NE (r.error().message.find (fragment), std::string::npos)
        << "message: " << r.error().message << ", wanted: " << fragment;
}

GraphDef
one_unit (const std::string& type, Rate rate, std::uint8_t n_out, std::vector<GraphInput> inputs) {
    GraphDef g;
    g.name   = "t";
    g.consts = { 0.0F, 1.0F };
    g.params = { { "p", 0.0F } };
    g.types  = { type };
    g.units  = { { 0, rate, n_out, std::move (inputs) } };
    return g;
}

}  // namespace

TEST (GraphdefValidate, TheExampleIsValid) {
    EXPECT_TRUE (validate (base()).has_value());
}

// rule 2
TEST (GraphdefValidate, DuplicateParamNames) {
    auto g = base();
    g.params.push_back ({ "freq", 1.0F });
    expect_invalid (g, "duplicate");
}

TEST (GraphdefValidate, BadParamNameOrDefault) {
    auto g           = base();
    g.params[0].name = "9lives";
    expect_invalid (g, "invalid name");
    g                         = base();
    g.params[0].default_value = std::numeric_limits<float>::quiet_NaN();
    expect_invalid (g, "not finite");
    g           = base();
    g.consts[0] = std::numeric_limits<float>::infinity();
    expect_invalid (g, "not finite");
}

// rule 3: limits, both sides
TEST (GraphdefValidate, ParamLimit) {
    GraphDef g = base();
    g.params.clear();
    for (int i = 0; i < 256; ++i) {
        g.params.push_back ({ "p" + std::to_string (i), 0.0F });
    }
    g.units[0].inputs[0] = kParam (255);
    g.units[3].inputs[1] = kParam (0);
    EXPECT_TRUE (validate (g).has_value());
    g.params.push_back ({ "p256", 0.0F });
    expect_invalid (g, "too many params");
}

TEST (GraphdefValidate, ConstantLimit) {
    GraphDef g = base();
    g.consts.assign (4096, 0.5F);
    EXPECT_TRUE (validate (g).has_value());
    g.consts.push_back (0.5F);
    expect_invalid (g, "too many constants");
}

TEST (GraphdefValidate, UnitLimit) {
    GraphDef g;
    g.name   = "chain";
    g.consts = { 1.0F };
    g.types  = { "add" };
    g.units.push_back ({ 0, Rate::kr, 1, { kConst (0), kConst (0) } });
    for (int i = 1; i < 4096; ++i) {
        g.units.push_back (
            { 0, Rate::kr, 1, { kUnit (static_cast<std::uint16_t> (i - 1)), kConst (0) } }
        );
    }
    EXPECT_TRUE (validate (g).has_value());
    g.units.push_back ({ 0, Rate::kr, 1, { kUnit (4095), kConst (0) } });
    expect_invalid (g, "too many units");
}

TEST (GraphdefValidate, InputAndOutputLimits) {
    // out with bus + 64 channels: 65 inputs, the maximum
    std::vector<GraphInput> inputs (65, kConst (0));
    EXPECT_TRUE (validate (one_unit ("out", Rate::ar, 0, inputs)).has_value());
    inputs.push_back (kConst (0));
    expect_invalid (one_unit ("out", Rate::ar, 0, inputs), "too many inputs");
    // in with 64 outputs is the maximum, 65 is not
    EXPECT_TRUE (validate (one_unit ("in", Rate::ar, 64, { kConst (0) })).has_value());
    expect_invalid (one_unit ("in", Rate::ar, 65, { kConst (0) }), "too many outputs");
}

// rule 4
TEST (GraphdefValidate, UnknownUnitType) {
    expect_invalid (one_unit ("nope", Rate::kr, 1, {}), "unknown unit type");
    auto g          = base();
    g.units[0].type = 7;
    expect_invalid (g, "type index");
}

TEST (GraphdefValidate, RateNotAllowedForType) {
    expect_invalid (
        one_unit ("sin_osc", Rate::ir, 1, { kConst (0), kConst (0) }),
        "does not support rate"
    );
    expect_invalid (
        one_unit ("out", Rate::ir, 0, { kConst (0), kConst (0) }),
        "does not support rate"
    );
    expect_invalid (
        one_unit (
            "env_perc",
            Rate::ir,
            1,
            { kConst (0), kConst (0), kConst (0), kConst (0), kConst (0) }
        ),
        "does not support rate"
    );
    EXPECT_TRUE (validate (one_unit ("add", Rate::ir, 1, { kConst (0), kConst (1) })).has_value());
}

TEST (GraphdefValidate, UnknownRateValuesAreRejected) {
    for (const auto raw : { 3, 31, 32, 255 }) {
        expect_invalid (
            one_unit ("add", static_cast<Rate> (raw), 1, { kConst (0), kConst (1) }),
            "does not support rate"
        );
    }
}

TEST (GraphdefValidate, UnknownInputSourcesAreRejected) {
    for (const auto raw : { 3, 255 }) {
        expect_invalid (
            one_unit (
                "add",
                Rate::kr,
                1,
                { { static_cast<InputSource> (raw), 0xFFFF, 0xFF }, kConst (0) }
            ),
            "invalid input source"
        );
    }
}

TEST (GraphdefValidate, WrongOutputCount) {
    expect_invalid (one_unit ("sin_osc", Rate::ar, 2, { kConst (0), kConst (0) }), "outputs");
    expect_invalid (one_unit ("sin_osc", Rate::ar, 0, { kConst (0), kConst (0) }), "outputs");
    expect_invalid (one_unit ("out", Rate::ar, 1, { kConst (0), kConst (1) }), "outputs");
    expect_invalid (one_unit ("in", Rate::ar, 0, { kConst (0) }), "outputs");
}

TEST (GraphdefValidate, WrongInputCount) {
    expect_invalid (one_unit ("add", Rate::kr, 1, { kConst (0) }), "inputs");
    expect_invalid (
        one_unit ("add", Rate::kr, 1, { kConst (0), kConst (0), kConst (0) }),
        "inputs"
    );
    expect_invalid (one_unit ("sin_osc", Rate::ar, 1, { kConst (0) }), "inputs");
    expect_invalid (one_unit ("out", Rate::ar, 0, { kConst (0) }), "inputs");  // no channel
    expect_invalid (one_unit ("in", Rate::ar, 1, {}), "inputs");
    EXPECT_TRUE (validate (one_unit ("out", Rate::ar, 0, { kConst (0), kConst (1) })).has_value());
}

// rule 5
TEST (GraphdefValidate, DanglingReferences) {
    auto g               = base();
    g.units[0].inputs[1] = kConst (5);
    expect_invalid (g, "constant index");
    g                    = base();
    g.units[0].inputs[0] = kParam (2);
    expect_invalid (g, "param index");
    g                    = base();
    g.units[2].inputs[0] = kUnit (2);  // itself
    expect_invalid (g, "not earlier");
    g                    = base();
    g.units[2].inputs[0] = kUnit (3);  // later
    expect_invalid (g, "not earlier");
    g                    = base();
    g.units[2].inputs[0] = kUnit (0, 1);  // sin_osc has one output
    expect_invalid (g, "missing output");
    g                    = base();
    g.units[4].inputs[0] = kUnit (0, 0);
    EXPECT_TRUE (validate (g).has_value());
}

TEST (GraphdefValidate, OutputNumberOfConstantsAndParamsIsZero) {
    auto g                      = base();
    g.units[0].inputs[1].output = 1;
    expect_invalid (g, "output must be 0");
    g                           = base();
    g.units[0].inputs[0].output = 3;
    expect_invalid (g, "output must be 0");
}

TEST (GraphdefValidate, ReferencesToLaterUnitsCannotFormCycles) {
    GraphDef g;
    g.name   = "loop";
    g.types  = { "add" };
    g.consts = { 1.0F };
    g.units  = { { 0, Rate::kr, 1, { kUnit (1), kConst (0) } },
                 { 0, Rate::kr, 1, { kUnit (0), kConst (0) } } };
    expect_invalid (g, "not earlier");
}

// rule 6
TEST (GraphdefValidate, IrUnitsReadOnlyIrSources) {
    GraphDef g;
    g.name   = "ir";
    g.consts = { 1.0F };
    g.params = { { "p", 0.0F } };
    g.types  = { "add" };
    // unit 0 is ir, 1 is kr, 2 is ar
    g.units = { { 0, Rate::ir, 1, { kConst (0), kParam (0) } },
                { 0, Rate::kr, 1, { kConst (0), kConst (0) } },
                { 0, Rate::ar, 1, { kConst (0), kConst (0) } } };
    EXPECT_TRUE (validate (g).has_value());

    auto from = [&] (std::uint16_t src) {
        auto h = g;
        h.units.push_back ({ 0, Rate::ir, 1, { kUnit (src), kConst (0) } });
        return h;
    };
    EXPECT_TRUE (validate (from (0)).has_value());
    expect_invalid (from (1), "ir unit reads");
    expect_invalid (from (2), "ir unit reads");

    // the other direction is allowed: kr and ar units read ir, kr and ar sources
    auto h = g;
    h.units.push_back ({ 0, Rate::kr, 1, { kUnit (0), kUnit (2) } });
    h.units.push_back ({ 0, Rate::ar, 1, { kUnit (1), kUnit (0) } });
    EXPECT_TRUE (validate (h).has_value());
}

TEST (GraphdefValidate, LoadGraphRunsDecodeThenValidate) {
    auto bad          = ping_graph();
    bad.units[0].rate = Rate::ir;  // sin_osc has no ir
    auto bytes        = encode_graph (bad);
    ASSERT_TRUE (bytes.has_value());
    EXPECT_TRUE (decode_graph (*bytes).has_value());
    EXPECT_FALSE (load_graph (*bytes).has_value());
}

TEST (GraphdefValidate, MessagesNameTheUnit) {
    auto g               = base();
    g.units[3].inputs[1] = kParam (9);
    auto r               = validate (g);
    ASSERT_FALSE (r.has_value());
    EXPECT_NE (r.error().message.find ("unit 3"), std::string::npos) << r.error().message;
}
