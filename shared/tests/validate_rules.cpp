#include <gtest/gtest.h>

#include <limits>

#include "bitvm/proto/validate.hpp"

using namespace bitvm::proto;

namespace
{

bool
passes (const RtCommand& c) {
    return validate (c).has_value();
}

std::vector<Pair>
pairs (std::size_t n) {
    return std::vector<Pair> (n, Pair{ 0, 1.0F });
}

}  // namespace

TEST (ValidateRules, RampRange) {
    const auto set = [] (float ramp) {
        return NodeSet{ 1, ramp, Curve::lin, {} };
    };
    EXPECT_TRUE (passes (set (0.0F)));
    EXPECT_TRUE (passes (set (-0.0F)));
    EXPECT_TRUE (passes (set (0.05F)));
    EXPECT_TRUE (passes (set (3600.0F)));
    EXPECT_FALSE (passes (set (-0.001F)));
    EXPECT_FALSE (passes (set (3600.01F)));
    EXPECT_FALSE (passes (set (1e30F)));
    EXPECT_FALSE (passes (set (std::numeric_limits<float>::quiet_NaN())));
}

TEST (ValidateRules, TargetDiffersFromId) {
    EXPECT_FALSE (passes (NodeNew{ 5, 1, AddAction::head, 5, false, {} }));
    EXPECT_FALSE (passes (NodeNew{ 5, 1, AddAction::replace, 5, false, {} }));
    EXPECT_TRUE (passes (NodeNew{ 5, 1, AddAction::replace, 6, false, {} }));
    EXPECT_FALSE (passes (GroupNew{ 7, AddAction::tail, 7 }));
    EXPECT_TRUE (passes (GroupNew{ 7, AddAction::tail, 0 }));
    EXPECT_FALSE (passes (NodeMove{ 7, AddAction::after, 7 }));
    EXPECT_TRUE (passes (NodeMove{ 7, AddAction::after, 8 }));
    // id 0 with target 0 is the same rule
    EXPECT_FALSE (passes (NodeNew{ 0, 1, AddAction::head, 0, false, {} }));
}

TEST (ValidateRules, NodeFreeList) {
    EXPECT_FALSE (passes (NodeFree{ {} }));
    EXPECT_FALSE (passes (NodeFree{ { 1, 0, 2 } }));
    EXPECT_FALSE (passes (NodeFree{ { 0 } }));
    EXPECT_TRUE (passes (NodeFree{ { 1, 2, 0xFFFFFFFF } }));
}

TEST (ValidateRules, NodeMoveCannotReplace) {
    EXPECT_FALSE (passes (NodeMove{ 1, AddAction::replace, 2 }));
    for (const auto add :
         { AddAction::head, AddAction::tail, AddAction::before, AddAction::after }) {
        EXPECT_TRUE (passes (NodeMove{ 1, add, 2 }));
    }
}

TEST (ValidateRules, CbusSetNeedsValues) {
    EXPECT_FALSE (passes (CbusSet{ 0, {} }));
    EXPECT_TRUE (passes (CbusSet{ 0, { 0.5F } }));
}

TEST (ValidateRules, ParamCountLimit) {
    EXPECT_TRUE (passes (NodeNew{ 1, 1, AddAction::head, 0, false, pairs (256) }));
    EXPECT_FALSE (passes (NodeNew{ 1, 1, AddAction::head, 0, false, pairs (257) }));
    EXPECT_TRUE (passes (NodeSet{ 1, 0.0F, Curve::lin, pairs (256) }));
    EXPECT_FALSE (passes (NodeSet{ 1, 0.0F, Curve::lin, pairs (257) }));
}

TEST (ValidateRules, CommandsWithoutRules) {
    EXPECT_TRUE (passes (NodeRun{ 0, false }));
    EXPECT_TRUE (passes (GroupClear{ 0 }));
}

TEST (ValidateRules, BundleReportsTheFailingCommand) {
    Bundle b;
    b.time     = 10;
    b.commands = { GroupClear{ 0 }, NodeRun{ 1, true }, NodeFree{ {} }, NodeFree{ { 0 } } };
    auto r     = validate (b);
    ASSERT_FALSE (r.has_value());
    EXPECT_EQ (r.error().index, 2);
    EXPECT_EQ (r.error().code, ErrorCode::invalid_arg);
    EXPECT_FALSE (r.error().detail.empty());
    b.commands.erase (b.commands.begin() + 2, b.commands.end());
    EXPECT_TRUE (validate (b).has_value());
}

TEST (ValidateRules, BundleLevelRules) {
    EXPECT_FALSE (validate (Bundle{ 0, {} }).has_value());
    EXPECT_FALSE (validate (Bundle{ -1, { GroupClear{ 0 } } }).has_value());
    EXPECT_EQ (validate (Bundle{ 0, {} }).error().index, kNoCommand);
}
