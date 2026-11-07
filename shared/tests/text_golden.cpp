#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <sstream>

#include "bitvm/proto/text.hpp"
#include "graph_fixtures.hpp"
#include "test_util.hpp"

using namespace bitvm::proto;
using namespace bitvm::proto::testing;

namespace
{

TextContext
make_context () {
    TextContext ctx;
    ctx.time_base_ns = 10'000'000'000;
    ctx.param_names  = [] (std::uint32_t def) -> const std::vector<std::string>* {
        static const std::vector<std::string> kPing = { "freq", "amp" };
        static const std::vector<std::string> kGate = { "gate" };
        if (def == 1) {
            return &kPing;
        }
        if (def == 2) {
            return &kGate;
        }
        return nullptr;
    };
    ctx.read_file = [] (std::string_view path) -> std::expected<std::string, std::string> {
        std::ifstream in (
            std::string (BITVM_TEST_DATA_DIR) + "/" + std::string (path),
            std::ios::binary
        );
        if (!in) {
            return std::unexpected ("not found");
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    };
    return ctx;
}

std::vector<std::string>
lines_of (const std::string& text) {
    std::vector<std::string> out;
    std::istringstream       in (text);
    for (std::string line; std::getline (in, line);) {
        out.push_back (line);
    }
    return out;
}

bool
update_mode () {
    const char* v = std::getenv ("BITVM_UPDATE_GOLDEN");
    return v != nullptr && std::string (v) == "1";
}

void
write_data_file (const std::string& name, const std::string& contents) {
    std::ofstream out (std::string (BITVM_TEST_DATA_DIR) + "/" + name, std::ios::binary);
    out << contents;
}

// compares `actual` with the golden file; rewrites the file in update mode
void
expect_golden (const std::string& name, const std::string& actual) {
    if (update_mode()) {
        write_data_file (name, actual);
    }
    EXPECT_EQ (actual, read_text_file (name)) << "golden file " << name;
}

}  // namespace

TEST (TextGraph, RepeatedLiteralsKeepTheirValuesPast65536Occurrences) {
    std::string source = "graph many_literals\n";
    for (int i = 0; i < 1024; ++i) {
        source += "out.ar 0";
        for (int channel = 0; channel < 64; ++channel) {
            source += " 1";
        }
        source += '\n';
    }
    source += "add.kr 777 888\n";

    const auto graph = assemble_graph (source);
    ASSERT_TRUE (graph.has_value()) << to_string (graph.error());
    ASSERT_TRUE (validate (*graph).has_value());
    const auto& last = graph->units.back();
    ASSERT_EQ (last.inputs.size(), 2U);
    EXPECT_EQ (graph->consts[last.inputs[0].index], 777.0F);
    EXPECT_EQ (graph->consts[last.inputs[1].index], 888.0F);
    EXPECT_EQ (graph->consts.size(), 4U);
}

TEST (TextGolden, MessagesAssembleToTheGoldenBytes) {
    auto       ctx      = make_context();
    const auto bvs      = read_text_file ("messages.bvs");
    auto       messages = assemble (bvs, ctx);
    ASSERT_TRUE (messages.has_value()) << to_string (messages.error());
    ASSERT_GT (messages->size(), 30U);

    std::string hex;
    for (const auto& m : *messages) {
        hex += to_hex (encode_ok (m)) + "\n";
    }
    expect_golden ("messages.hex", hex);

    // and the other way: the golden bytes disassemble to the golden text
    std::string text;
    for (const auto& line : lines_of (read_text_file ("messages.hex"))) {
        text += disassemble (from_hex (line), ctx);
    }
    EXPECT_EQ (text, bvs);
}

TEST (TextGolden, MessagesRoundtripThroughStructures) {
    auto       ctx      = make_context();
    const auto bvs      = read_text_file ("messages.bvs");
    auto       messages = assemble (bvs, ctx);
    ASSERT_TRUE (messages.has_value());
    std::string text;
    for (const auto& m : *messages) {
        text += disassemble (m, ctx);
    }
    EXPECT_EQ (text, bvs);
}

TEST (TextGolden, FirstMessagesAreTheCanonicalSamples) {
    auto ctx      = make_context();
    auto messages = assemble (read_text_file ("messages.bvs"), ctx);
    ASSERT_TRUE (messages.has_value());
    const auto samples = canonical_samples();
    ASSERT_LE (samples.size(), messages->size());
    for (std::size_t i = 0; i < samples.size(); ++i) {
        EXPECT_EQ ((*messages)[i], samples[i]) << "message " << i + 1;
    }
}

TEST (TextGolden, EverySampleSurvivesTextAndBack) {
    for (const bool with_names : { false, true }) {
        auto ctx = make_context();
        if (!with_names) {
            ctx.param_names = nullptr;
        }
        for (const auto& sample : edge_samples()) {
            const auto text = disassemble (sample, ctx);
            auto       back = assemble (text, ctx);
            ASSERT_TRUE (back.has_value()) << to_string (back.error()) << "\n"
                                           << text.substr (0, 300);
            ASSERT_EQ (back->size(), 1U);
            EXPECT_EQ (back->front(), sample) << text.substr (0, 300);
            // the bytes agree as well
            EXPECT_EQ (encode_ok (back->front()), encode_ok (sample));
            // and the printed form of the reassembled message is the same text
            EXPECT_EQ (disassemble (back->front(), ctx), text);
        }
    }
}

TEST (TextGolden, LooseSyntaxNormalizes) {
    auto ctx      = make_context();
    auto messages = assemble (read_text_file ("loose.bvs"), ctx);
    ASSERT_TRUE (messages.has_value()) << to_string (messages.error());
    std::string text;
    for (const auto& m : *messages) {
        text += disassemble (m, ctx);
    }
    expect_golden ("loose.canonical.bvs", text);
    EXPECT_EQ (ctx.next_seq, 10U);  // nine messages were numbered by the assembler
}

TEST (TextGolden, ExplicitSequenceNumbersWin) {
    auto ctx      = make_context();
    auto messages = assemble ("call/40 sync\ncall sync\n@now/7 group.clear id=0\n", ctx);
    ASSERT_TRUE (messages.has_value());
    EXPECT_EQ ((*messages)[0].seq, 40U);
    EXPECT_EQ ((*messages)[1].seq, 1U);
    EXPECT_EQ ((*messages)[2].seq, 7U);
}

TEST (TextGolden, RelativeTimesUseTheBase) {
    auto ctx         = make_context();
    ctx.time_base_ns = 1'000;
    auto messages    = assemble (
        "@+0 group.clear id=0\n@+0.000000001 group.clear id=0\n@+2.5 group.clear id=0\n",
        ctx
    );
    ASSERT_TRUE (messages.has_value());
    EXPECT_EQ (std::get<Bundle> ((*messages)[0].body).time, 1'000);
    EXPECT_EQ (std::get<Bundle> ((*messages)[1].body).time, 1'001);
    EXPECT_EQ (std::get<Bundle> ((*messages)[2].body).time, 2'500'001'000);
}

// ---- graphs ----

TEST (TextGolden, PingGraph) {
    auto g = assemble_graph (read_text_file ("ping.bvg"));
    ASSERT_TRUE (g.has_value()) << to_string (TextError{ g.error() });
    EXPECT_EQ (*g, ping_graph());
    EXPECT_TRUE (validate (*g).has_value());
    expect_golden ("ping.canonical.bvg", disassemble_graph (*g));
    expect_golden ("ping.bvgd.hex", to_hex (*encode_graph (*g)) + "\n");
    EXPECT_EQ (to_hex (*encode_graph (*g)), to_hex (from_hex (ping_hex())));
}

TEST (TextGolden, StereoGraphWithVariableOutputs) {
    auto g = assemble_graph (read_text_file ("stereo.bvg"));
    ASSERT_TRUE (g.has_value()) << to_string (TextError{ g.error() });
    EXPECT_TRUE (validate (*g).has_value());
    ASSERT_EQ (g->units.size(), 4U);
    EXPECT_EQ (g->units[0].n_out, 2);
    EXPECT_EQ (g->units[2].inputs[0].output, 1);
    expect_golden ("stereo.canonical.bvg", disassemble_graph (*g));
    expect_golden ("stereo.bvgd.hex", to_hex (*encode_graph (*g)) + "\n");
}

TEST (TextGolden, GraphBinaryTextBinary) {
    for (const auto* name : { "ping.bvgd.hex", "stereo.bvgd.hex" }) {
        const auto bytes = from_hex (read_text_file (name));
        auto       g     = decode_graph (bytes);
        ASSERT_TRUE (g.has_value()) << name;
        const auto text  = disassemble_graph (*g);
        auto       again = assemble_graph (text);
        ASSERT_TRUE (again.has_value()) << name << ": " << to_string (TextError{ again.error() });
        EXPECT_EQ (*encode_graph (*again), bytes) << name;
        EXPECT_EQ (disassemble_graph (*again), text);
    }
}

TEST (TextGolden, CanonicalGraphTextIsAFixedPoint) {
    for (const auto* name : { "ping.canonical.bvg", "stereo.canonical.bvg" }) {
        const auto text = read_text_file (name);
        auto       g    = assemble_graph (text);
        ASSERT_TRUE (g.has_value()) << name;
        EXPECT_EQ (disassemble_graph (*g), text) << name;
    }
}

TEST (TextGolden, DefaultsFillOmittedArguments) {
    auto g = assemble_graph ("graph d\nenv_perc.kr\nsin_osc.ar\n");
    ASSERT_TRUE (g.has_value()) << to_string (TextError{ g.error() });
    EXPECT_EQ (
        disassemble_graph (*g),
        "graph d\n\n"
        "%0 = env_perc.kr 0.01 1 1 -4 0\n"
        "%1 = sin_osc.ar 440 0\n"
    );
}

// ---- base64 ----

TEST (TextGolden, Base64Vectors) {
    const auto enc = [] (const std::string& s) {
        Bytes b;
        for (const char c : s) {
            b.push_back (static_cast<std::byte> (c));
        }
        return base64_encode (b);
    };
    EXPECT_EQ (enc (""), "");
    EXPECT_EQ (enc ("f"), "Zg==");
    EXPECT_EQ (enc ("fo"), "Zm8=");
    EXPECT_EQ (enc ("foo"), "Zm9v");
    EXPECT_EQ (enc ("foob"), "Zm9vYg==");
    EXPECT_EQ (enc ("fooba"), "Zm9vYmE=");
    EXPECT_EQ (enc ("foobar"), "Zm9vYmFy");
    for (const char* s : { "", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==" }) {
        auto back = base64_decode (s);
        ASSERT_TRUE (back.has_value()) << s;
        EXPECT_EQ (base64_encode (*back), s);
    }
    for (const char* bad :
         { "Z", "Zg=", "Zg==Zg==", "Z===", "Zm9!", "=Zg=", "Zg=a", "Zm 9", "Zh==", "Zm9=" }) {
        EXPECT_FALSE (base64_decode (bad).has_value()) << bad;
    }
}

// ---- disassembling bytes that do not decode ----

TEST (TextGolden, DisassemblerShowsUnknownCommandsAndContinues) {
    auto ctx = make_context();
    // a bundle: group.clear, an unknown opcode 0x0999 with two data bytes, node.run
    const auto bytes = from_hex (
        "00000000 0400 0000 09000000"
        "0000000000000000"
        "0601 0800 00000000"
        "9909 0600 abcd"
        "0401 0900 01000000 01"
    );
    Bytes fixed = bytes;
    fix_length (fixed);
    ASSERT_FALSE (decode (fixed).has_value());
    const auto text = disassemble (fixed, ctx);
    EXPECT_NE (text.find ("# error: unsupported"), std::string::npos) << text;
    EXPECT_NE (text.find ("command 1"), std::string::npos) << text;
    EXPECT_NE (text.find ("group.clear id=0x00000000"), std::string::npos) << text;
    EXPECT_NE (text.find ("? opcode=0x0999 data=abcd"), std::string::npos) << text;
    EXPECT_NE (text.find ("node.run id=0x00000001 run=1"), std::string::npos) << text;
}

TEST (TextGolden, DisassemblerNeverFailsOnGarbage) {
    auto ctx = make_context();
    for (const auto& sample : edge_samples()) {
        const auto bytes = encode_ok (sample);
        for (std::size_t n = 0; n < bytes.size(); n += 7) {
            const Bytes cut (bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t> (n));
            EXPECT_FALSE (disassemble (cut, ctx).empty());
        }
    }
    EXPECT_FALSE (disassemble (std::span<const std::byte>{}, ctx).empty());
    // for valid input the byte form and the structure form agree
    for (const auto& sample : edge_samples()) {
        EXPECT_EQ (disassemble (encode_ok (sample), ctx), disassemble (sample, ctx));
    }
}
