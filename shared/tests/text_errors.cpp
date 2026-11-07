#include <gtest/gtest.h>

#include <memory>

#include "bitvm/proto/text.hpp"
#include "test_util.hpp"

using namespace bitvm::proto;
using namespace bitvm::proto::testing;

namespace
{

TextContext
context () {
    TextContext ctx;
    ctx.param_names = [] (std::uint32_t def) -> const std::vector<std::string>* {
        static const std::vector<std::string> kNames = { "freq", "amp" };
        return def == 1 ? &kNames : nullptr;
    };
    return ctx;
}

void
expect_at (
    const std::string& input,
    std::size_t        line,
    std::size_t        column,
    const std::string& fragment
) {
    auto ctx = context();
    auto r   = assemble (input, ctx);
    ASSERT_FALSE (r.has_value()) << "assembled: " << input;
    EXPECT_EQ (r.error().line, line) << input << " -> " << to_string (r.error());
    EXPECT_EQ (r.error().column, column) << input << " -> " << to_string (r.error());
    EXPECT_NE (r.error().message.find (fragment), std::string::npos)
        << input << " -> " << to_string (r.error());
}

void
expect_graph_error (
    const std::string& input,
    std::size_t        line,
    std::size_t        column,
    const std::string& fragment
) {
    auto r = assemble_graph (input);
    ASSERT_FALSE (r.has_value()) << "assembled: " << input;
    EXPECT_EQ (r.error().line, line) << input << " -> " << to_string (r.error());
    EXPECT_EQ (r.error().column, column) << input << " -> " << to_string (r.error());
    EXPECT_NE (r.error().message.find (fragment), std::string::npos)
        << input << " -> " << to_string (r.error());
}

}  // namespace

TEST (TextErrors, PositionsOfFieldProblems) {
    expect_at ("call sync\ncall tree\n", 2, 6, "missing field `root`");
    expect_at ("@now group.new id=1 add=head target=0 extra=1\n", 1, 39, "unknown field `extra`");
    expect_at ("@now group.new id=1 add=middle target=0\n", 1, 25, "unknown name `middle`");
    expect_at ("@now group.new id=xyz add=head target=0\n", 1, 19, "unsigned integer");
    expect_at ("@now group.new id=1 id=2 add=head target=0\n", 1, 21, "duplicate field `id`");
    expect_at ("@now group.new id=1 add=head target\n", 1, 30, "key=value");
    expect_at ("@now group.clear id=4294967296\n", 1, 21, "unsigned integer");
    expect_at ("@now node.run id=1 run=2\n", 1, 24, "expected 0 or 1");
}

TEST (TextErrors, NumbersMustBeFinite) {
    expect_at ("@now node.set id=1 ramp=nan\n", 1, 25, "finite");
    expect_at ("@now node.set id=1 ramp=inf\n", 1, 25, "finite");
    expect_at ("@now node.set id=1 ramp=-inf\n", 1, 25, "finite");
    expect_at ("@now node.set id=1 ramp=1e999\n", 1, 25, "finite");
    expect_at ("@now cbus.set index=0 values=1,nan\n", 1, 32, "finite");
    expect_at ("@now node.set id=1 $0=inf\n", 1, 23, "finite");
    expect_at ("@now node.set id=1 $0=0x10\n", 1, 23, "finite");
}

TEST (TextErrors, Parameters) {
    expect_at (
        "@now node.new id=1 def=1 add=head target=0 $nope=1\n",
        1,
        44,
        "unknown parameter `nope`"
    );
    expect_at ("@now node.new id=1 def=9 add=head target=0 $freq=1\n", 1, 44, "def is unknown");
    expect_at ("@now node.set id=1 $freq=1\n", 1, 20, "def is unknown");
    expect_at ("@now node.new id=1 def=1 add=head target=0 $freq\n", 1, 44, "$name=value");
    expect_at ("@now node.new id=1 def=1 add=head target=0 $70000=1\n", 1, 44, "out of range");
    expect_at ("@now group.clear id=1 $0=1\n", 1, 23, "takes no parameters");
}

TEST (TextErrors, MessageHeads) {
    expect_at ("frobnicate/1\n", 1, 1, "unknown message");
    expect_at ("welcome major=0\n", 1, 1, "needs /<seq>");
    expect_at ("reply sync ok\n", 1, 1, "needs /<seq>");
    expect_at ("event n_end id=1\n", 1, 1, "needs /<seq>");
    expect_at ("call/x sync\n", 1, 6, "sequence number");
    expect_at ("call/1 nonsense\n", 1, 8, "unknown call");
    expect_at ("call/1 node.new\n", 1, 8, "unknown call");
    expect_at ("call/1\n", 1, 7, "call name");
    expect_at ("event/1 late2\n", 1, 9, "unknown event");
}

TEST (TextErrors, TimeSyntax) {
    expect_at ("@later group.clear id=0\n", 1, 2, "invalid time");
    expect_at ("@-5 group.clear id=0\n", 1, 2, "invalid time");
    expect_at ("@+x group.clear id=0\n", 1, 3, "relative time");
    expect_at ("@+-1 group.clear id=0\n", 1, 3, "relative time");
    expect_at ("@+nan group.clear id=0\n", 1, 3, "relative time");
    expect_at ("@12/z group.clear id=0\n", 1, 5, "sequence number");
}

TEST (TextErrors, BundleStructure) {
    expect_at ("@now\n", 1, 5, "command or `{`");
    expect_at ("@now {\n  group.clear id=0\n", 1, 6, "unterminated");
    expect_at ("@now {\n}\n", 1, 6, "empty bundle");
    expect_at ("@now {\n  group.clear id=0\n  sync\n}\n", 3, 3, "call used inside a bundle");
    expect_at ("@now {\n  group.clear id=0\n  frob\n}\n", 3, 3, "unknown bundle command");
    expect_at ("@now sync\n", 1, 6, "call used inside a bundle");
    expect_at ("@now n_end id=1\n", 1, 6, "unknown bundle command");
}

TEST (TextErrors, StringsAndQuotes) {
    expect_at ("call buf.load path=\"abc\n", 1, 20, "unterminated string");
    expect_at ("call buf.load path=\"a\"b\n", 1, 23, "after the closing quote");
    expect_at ("call buf.load path=a\"b\"\n", 1, 21, "quote inside");
    expect_at ("call buf.load path=\"a\\qb\"\n", 1, 22, "unknown escape");
    expect_at ("call buf.load path=\"a\\x4\"\n", 1, 22, "\\x escape");
    expect_at ("call buf.load path=\"\"\n", 1, 1, "cannot be encoded");
    EXPECT_TRUE (
        assemble ("call buf.load path=\"a\\x41\\t\\n\\\\\\\"\"\n", *std::make_unique<TextContext>())
            .has_value()
    );
}

TEST (TextErrors, GraphFiles) {
    auto ctx      = context();
    ctx.read_file = [] (std::string_view path) -> std::expected<std::string, std::string> {
        if (path == "bad.bvg") {
            return std::string ("graph g\nfoo.ar 1\n");
        }
        return std::unexpected ("no such file");
    };
    auto r = assemble ("call def.load graph=@bad.bvg\n", ctx);
    ASSERT_FALSE (r.has_value());
    EXPECT_EQ (r.error().line, 1U);
    EXPECT_EQ (r.error().column, 21U);
    EXPECT_NE (r.error().message.find ("bad.bvg:2:1"), std::string::npos) << r.error().message;
    r = assemble ("call def.load graph=@missing.bvg\n", ctx);
    ASSERT_FALSE (r.has_value());
    EXPECT_NE (r.error().message.find ("no such file"), std::string::npos);
    // without a loader
    TextContext bare;
    r = assemble ("call def.load graph=@x.bvg\n", bare);
    ASSERT_FALSE (r.has_value());
    EXPECT_NE (r.error().message.find ("no file loader"), std::string::npos);
    expect_at ("call def.load graph=base64:!!!!\n", 1, 28, "base64");
    expect_at ("call def.load graph=raw\n", 1, 21, "base64");
}

TEST (TextErrors, ReplyAndEventForms) {
    expect_at ("reply/1 sync\n", 1, 13, "ok");
    expect_at ("reply/1 sync fine\n", 1, 14, "expected `ok` or `err=");
    expect_at ("reply/1 sync err=ok msg=\"\"\n", 1, 14, "cannot be ok");
    expect_at ("reply/1 nope ok\n", 1, 9, "unknown call");
    expect_at ("reply/1 0x0100 ok\n", 1, 9, "not a call");
    expect_at ("reply/1 tree ok truncated=0 entries=1:2:group:0\n", 1, 37, "tree entry");
    expect_at ("reply/1 tree ok truncated=0 entries=1:2:blob:0:1\n", 1, 37, "tree entry");
    expect_at (
        "event/1 error bundle_seq=1 index=0 opcode=zzz code=io level=1 msg=\"\"\n",
        1,
        43,
        "opcode"
    );
}

TEST (TextErrors, HelloEvents) {
    expect_at (
        "hello major=0 minor=1 catalog_hash=0 requested_client=0 events=n_end,loud name=x\n",
        1,
        70,
        "unknown event"
    );
    expect_at (
        "hello major=0 minor=1 catalog_hash=0 requested_client=300 events=none name=x\n",
        1,
        55,
        "unsigned integer"
    );
}

TEST (TextErrors, LineNumbersCountEveryLine) {
    expect_at ("# comment\n\n   \ncall sync\ncall tree\n", 5, 6, "missing field");
    expect_at ("call sync\r\ncall tree\r\n", 2, 6, "missing field");
}

TEST (TextErrors, GraphSyntax) {
    expect_graph_error ("", 1, 1, "missing `graph");
    expect_graph_error ("param x 1\n", 1, 1, "expected `graph");
    expect_graph_error ("graph g\nparam x\n", 2, 1, "expected `param");
    expect_graph_error ("graph g\nparam 9x 1\n", 2, 7, "invalid parameter name");
    expect_graph_error ("graph g\nparam x 1\nparam x 2\n", 3, 7, "duplicate parameter");
    expect_graph_error ("graph g\nparam x nan\n", 2, 9, "invalid number");
    expect_graph_error ("graph g\nadd.kr 1 2\nparam x 1\n", 3, 1, "param after units");
    expect_graph_error ("graph g\nnope.kr 1\n", 2, 1, "unknown unit type");
    expect_graph_error ("graph g\nadd 1 2\n", 2, 1, "<type>.<rate>");
    expect_graph_error ("graph g\nadd.xr 1 2\n", 2, 5, "rate must be");
    expect_graph_error ("graph g\nadd.kr 1\n", 2, 9, "missing required argument `b`");
    expect_graph_error ("graph g\nadd.kr 1 2 3\n", 2, 12, "too many arguments");
    expect_graph_error ("graph g\nout.ar 0\n", 2, 9, "missing argument `channels`");
    expect_graph_error ("graph g\nadd.kr $nope 1\n", 2, 8, "unknown parameter");
    expect_graph_error ("graph g\nadd.kr $3 1\n", 2, 8, "out of range");
    expect_graph_error ("graph g\nadd.kr %x 1\n", 2, 8, "unknown label");
    expect_graph_error ("graph g\n%a = add.kr 1 1\nadd.kr %a.1 1\n", 3, 8, "no such output");
    expect_graph_error ("graph g\n%a = add.kr 1 1\n%a = add.kr 1 1\n", 3, 1, "duplicate label");
    expect_graph_error ("graph g\n%a add.kr 1 1\n", 2, 1, "expected `=`");
    expect_graph_error ("graph g\n%a:2 = add.kr 1 1\n", 2, 4, "fixed number of outputs");
    expect_graph_error ("graph g\n%a:99 = in.ar 0\n", 2, 4, "1..64");
    expect_graph_error ("graph g\nadd.kr 1 abc\n", 2, 10, "expected a number");
    expect_graph_error ("graph g\nadd.kr 1 nan\n", 2, 10, "expected a number");
    expect_graph_error ("graph \"unterminated\n", 1, 7, "unterminated");
}

TEST (TextErrors, GraphIrAndRatesAreLeftToTheValidator) {
    // the text layer builds the graph; the validator rejects it
    auto g = assemble_graph ("graph g\nsin_osc.ir 1 0\n");
    ASSERT_TRUE (g.has_value());
    EXPECT_FALSE (validate (*g).has_value());
}
