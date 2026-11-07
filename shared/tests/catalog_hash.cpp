#include <gtest/gtest.h>

#include <set>

#include "bitvm/proto/catalog.hpp"
#include "test_util.hpp"

using namespace bitvm::proto;
using namespace bitvm::proto::testing;

// the pinned hash: a catalog change must update this value, data/catalog.golden and the
// protocol minor version together (docs/protocol.md section 14)
TEST (CatalogHash, PinnedValue) {
    EXPECT_EQ (catalog_hash(), 0x5ea6a5685445ee48ULL) << "text:\n" << catalog_text();
}

TEST (CatalogHash, CanonicalTextMatchesGolden) {
    EXPECT_EQ (catalog_text(), read_text_file ("catalog.golden"));
}

TEST (CatalogHash, HashIsFnv1aOfTheText) {
    // reference fnv-1a 64 test vectors
    const auto fnv = [] (std::string_view s) {
        std::uint64_t h = 0xcbf29ce484222325ULL;
        for (const char c : s) {
            h = (h ^ static_cast<std::uint8_t> (c)) * 0x100000001b3ULL;
        }
        return h;
    };
    EXPECT_EQ (fnv (""), 0xcbf29ce484222325ULL);
    EXPECT_EQ (fnv ("a"), 0xaf63dc4c8601ec8cULL);
    EXPECT_EQ (fnv ("foobar"), 0x85944171f73967e8ULL);
    EXPECT_EQ (catalog_hash(), fnv (catalog_text()));
}

TEST (CatalogContents, V0Units) {
    const std::set<std::string_view> expected = { "add",      "sub",     "mul", "div", "sin_osc",
                                                  "env_perc", "env_asr", "out", "in" };
    std::set<std::string_view>       actual;
    for (const auto& u : catalog()) {
        actual.insert (u.name);
        EXPECT_NE (u.rates, 0) << u.name;
        EXPECT_FALSE (u.description.empty()) << u.name;
        EXPECT_TRUE (u.name == std::string (u.name.begin(), u.name.end()));
        for (const char c : u.name) {
            EXPECT_TRUE ((c >= 'a' && c <= 'z') || c == '_' || (c >= '0' && c <= '9')) << u.name;
        }
        EXPECT_LE (max_inputs (u), kMaxUnitInputs) << u.name;
        EXPECT_LE (u.max_outputs, kMaxUnitOutputs) << u.name;
    }
    EXPECT_EQ (actual, expected);
    EXPECT_EQ (catalog().size(), expected.size());
}

TEST (CatalogContents, SignaturesFollowTheSpec) {
    const auto* sin = find_unit ("sin_osc");
    ASSERT_NE (sin, nullptr);
    EXPECT_FALSE (allows_rate (*sin, Rate::ir));
    EXPECT_TRUE (allows_rate (*sin, Rate::kr));
    EXPECT_TRUE (allows_rate (*sin, Rate::ar));
    ASSERT_EQ (sin->inputs.size(), 2U);
    EXPECT_EQ (sin->inputs[0].default_value, 440.0F);
    EXPECT_EQ (sin->inputs[0].read, InputRead::unit);
    EXPECT_EQ (sin->inputs[1].read, InputRead::ir);

    const auto* perc = find_unit ("env_perc");
    ASSERT_NE (perc, nullptr);
    ASSERT_EQ (perc->inputs.size(), 5U);
    EXPECT_EQ (perc->inputs[3].name, "curve");
    EXPECT_EQ (perc->inputs[3].default_value, -4.0F);
    EXPECT_TRUE (perc->done);
    EXPECT_TRUE (find_unit ("env_asr")->done);
    EXPECT_FALSE (find_unit ("add")->done);

    const auto* out = find_unit ("out");
    ASSERT_NE (out, nullptr);
    EXPECT_EQ (out->min_outputs, 0);
    EXPECT_EQ (out->max_outputs, 0);
    ASSERT_TRUE (out->variadic.has_value());
    EXPECT_EQ (out->variadic->min_count, 1);
    EXPECT_EQ (out->variadic->max_count, 64);
    EXPECT_EQ (min_inputs (*out), 2U);
    EXPECT_EQ (max_inputs (*out), 65U);

    const auto* in = find_unit ("in");
    ASSERT_NE (in, nullptr);
    EXPECT_TRUE (has_variable_outputs (*in));
    EXPECT_EQ (in->max_outputs, 64);
    EXPECT_FALSE (has_variable_outputs (*out));

    for (const char* name : { "add", "sub", "mul", "div" }) {
        const auto* u = find_unit (name);
        ASSERT_NE (u, nullptr);
        EXPECT_TRUE (allows_rate (*u, Rate::ir)) << name;
        EXPECT_TRUE (u->inputs[0].required && u->inputs[1].required) << name;
    }
    EXPECT_EQ (find_unit ("nope"), nullptr);
    EXPECT_EQ (find_unit (""), nullptr);
}

TEST (CatalogContents, UnknownRatesAreNeverAllowed) {
    for (const auto& unit : catalog()) {
        for (const auto raw : { 3, 31, 32, 255 }) {
            EXPECT_FALSE (allows_rate (unit, static_cast<Rate> (raw))) << unit.name << ": " << raw;
        }
    }
}

TEST (CatalogContents, EffectiveRead) {
    EXPECT_EQ (effective_read (InputRead::unit, Rate::ar), Rate::ar);
    EXPECT_EQ (effective_read (InputRead::unit, Rate::kr), Rate::kr);
    EXPECT_EQ (effective_read (InputRead::ir, Rate::ar), Rate::ir);
    EXPECT_EQ (effective_read (InputRead::kr, Rate::ar), Rate::kr);
    EXPECT_EQ (effective_read (InputRead::ar, Rate::kr), Rate::ar);
}
