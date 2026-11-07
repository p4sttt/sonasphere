#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include "graph_fixtures.hpp"
#include "test_util.hpp"

using namespace bitvm::proto;
using namespace bitvm::proto::testing;

TEST (GraphdefCodec, EncodeMatchesHandWrittenBytes) {
    auto bytes = encode_graph (ping_graph());
    ASSERT_TRUE (bytes.has_value()) << bytes.error().message;
    EXPECT_EQ (*bytes, from_hex (ping_hex()));
}

TEST (GraphdefCodec, DecodeRoundtrip) {
    auto g = decode_graph (from_hex (ping_hex()));
    ASSERT_TRUE (g.has_value()) << g.error().message;
    EXPECT_EQ (*g, ping_graph());
    EXPECT_TRUE (validate (*g).has_value());
    EXPECT_TRUE (load_graph (from_hex (ping_hex())).has_value());
}

TEST (GraphdefCodec, EncoderWritesTheCanonicalForm) {
    // same graph with repeated and unused constants and types, in a different order
    GraphDef messy = ping_graph();
    messy.consts   = { 9.0F, 0.0F, 0.005F, 0.4F, 1.0F, -4.0F, 1.0F, 0.0F };
    messy.types    = { "env_perc", "unused", "mul", "sin_osc", "out", "mul" };
    const auto c   = [] (std::uint16_t i) {
        return GraphInput{ InputSource::constant, i, 0 };
    };
    messy.units[0]      = { 3, Rate::ar, 1, { { InputSource::param, 0, 0 }, c (7) } };
    messy.units[1]      = { 0, Rate::kr, 1, { c (2), c (3), c (6), c (5), c (4) } };
    messy.units[2].type = 5;
    messy.units[3].type = 2;
    messy.units[4]      = { 4,
                            Rate::ar,
                            0,
                            { c (1), { InputSource::unit, 3, 0 }, { InputSource::unit, 3, 0 } } };

    auto bytes = encode_graph (messy);
    ASSERT_TRUE (bytes.has_value()) << bytes.error().message;
    // first-use order of types: sin_osc, env_perc, mul, out; of constants: 0, .005, .4, 1, -4
    EXPECT_EQ (*bytes, from_hex (ping_hex()));
    EXPECT_EQ (*canonicalize (ping_graph()), ping_graph());
    EXPECT_EQ (*canonicalize (messy), ping_graph());
}

TEST (GraphdefCodec, ConstantsAreComparedBitwise) {
    GraphDef g;
    g.name       = "z";
    g.consts     = { 0.0F, -0.0F, 0.0F };
    g.types      = { "add" };
    const auto c = [] (std::uint16_t i) {
        return GraphInput{ InputSource::constant, i, 0 };
    };
    g.units    = { { 0, Rate::kr, 1, { c (0), c (1) } }, { 0, Rate::kr, 1, { c (2), c (1) } } };
    auto canon = canonicalize (g);
    ASSERT_TRUE (canon.has_value());
    ASSERT_EQ (canon->consts.size(), 2U);
    EXPECT_FALSE (std::signbit (canon->consts[0]));
    EXPECT_TRUE (std::signbit (canon->consts[1]));
    // and the bit pattern survives a roundtrip
    auto back = decode_graph (*encode_graph (g));
    ASSERT_TRUE (back.has_value());
    EXPECT_TRUE (std::signbit (back->consts[1]));
}

TEST (GraphdefCodec, DecoderAcceptsNonCanonicalGraphs) {
    // append an unused constant: count 5 -> 6, 4 more bytes after the fifth constant
    std::string hex = ping_hex();
    const auto  at  = hex.find ("0500 00000000");
    ASSERT_NE (at, std::string::npos);
    hex.replace (at, 4, "0600");
    const auto types = hex.find ("0400 0700");
    ASSERT_NE (types, std::string::npos);
    hex.insert (types, "0000803f ");
    auto g = decode_graph (from_hex (hex));
    ASSERT_TRUE (g.has_value()) << g.error().message;
    EXPECT_EQ (g->consts.size(), 6U);
    EXPECT_TRUE (validate (*g).has_value());
    // encoding it again drops the unused constant
    EXPECT_EQ (*encode_graph (*g), from_hex (ping_hex()));
}

TEST (GraphdefCodec, RejectsBrokenBytes) {
    const auto good = from_hex (ping_hex());
    // every strict prefix
    for (std::size_t n = 0; n < good.size(); ++n) {
        const Bytes cut (good.begin(), good.begin() + static_cast<std::ptrdiff_t> (n));
        EXPECT_FALSE (decode_graph (cut).has_value()) << n;
        EXPECT_FALSE (load_graph (cut).has_value()) << n;
    }
    // trailing byte
    auto extra = good;
    extra.push_back (std::byte{ 0 });
    EXPECT_FALSE (decode_graph (extra).has_value());
    // magic and version
    auto bad_magic = good;
    bad_magic[0]   = std::byte{ 'X' };
    EXPECT_FALSE (decode_graph (bad_magic).has_value());
    auto bad_version = good;
    bad_version[4]   = std::byte{ 2 };
    EXPECT_FALSE (decode_graph (bad_version).has_value());
    EXPECT_FALSE (decode_graph ({}).has_value());
}

TEST (GraphdefCodec, RejectsBadFields) {
    const auto with = [] (const std::string& from, const std::string& to) {
        std::string hex = ping_hex();
        const auto  at  = hex.find (from);
        EXPECT_NE (at, std::string::npos) << from;
        hex.replace (at, from.size(), to);
        return decode_graph (from_hex (hex));
    };
    EXPECT_TRUE (with ("0400 66726571", "0400 66726571").has_value());
    // parameter name starting with a digit
    EXPECT_FALSE (with ("0400 66726571", "0400 31726571").has_value());
    // nan default
    EXPECT_FALSE (with ("0000dc43", "0000c07f").has_value());
    // nan and inf constants
    EXPECT_FALSE (with ("0ad7a33b", "0000807f").has_value());
    EXPECT_FALSE (with ("0ad7a33b", "000080ff").has_value());
    // rate 3
    EXPECT_FALSE (with ("0000 02 01 02", "0000 03 01 02").has_value());
    // input source 3
    EXPECT_FALSE (with ("01 0000 00  00 0000 00", "03 0000 00  00 0000 00").has_value());
    // unit type name that is not a name
    EXPECT_FALSE (with ("0300 6d756c 0300", "0300 6d2d6c 0300").has_value());
}

TEST (GraphdefCodec, EncodeRefusesDanglingReferences) {
    auto g          = ping_graph();
    g.units[0].type = 9;
    EXPECT_FALSE (encode_graph (g).has_value());
    g                          = ping_graph();
    g.units[0].inputs[1].index = 40;
    EXPECT_FALSE (encode_graph (g).has_value());
    g                = ping_graph();
    g.params[0].name = "bad name";
    EXPECT_FALSE (encode_graph (g).has_value());
    g                         = ping_graph();
    g.params[0].default_value = std::numeric_limits<float>::infinity();
    EXPECT_FALSE (encode_graph (g).has_value());
    g      = ping_graph();
    g.name = std::string (1025, 'n');
    EXPECT_FALSE (encode_graph (g).has_value());
}

TEST (GraphdefCodec, GraphFitsInDefLoad) {
    auto bytes = encode_graph (ping_graph());
    ASSERT_TRUE (bytes.has_value());
    const Message m{ 1, Call{ DefLoad{ *bytes } } };
    auto          wire = encode (m);
    ASSERT_TRUE (wire.has_value());
    EXPECT_EQ (decode (*wire).value(), m);
}
