#include <gtest/gtest.h>

#include "bitvm/proto/types.hpp"

using namespace bitvm::proto;

TEST (TypesNames, EnumerationsRoundtripByName) {
    for (std::uint8_t v = 0; v <= 4; ++v) {
        const auto e = add_action_from_wire (v);
        ASSERT_TRUE (e.has_value());
        EXPECT_EQ (add_action_from_name (name_of (*e)), e);
    }
    EXPECT_FALSE (add_action_from_wire (5).has_value());
    EXPECT_FALSE (bus_kind_from_wire (2).has_value());
    EXPECT_FALSE (rate_from_wire (3).has_value());
    EXPECT_FALSE (node_kind_from_wire (2).has_value());
    for (std::uint16_t v = 0; v < kErrorCodeCount; ++v) {
        const auto e = error_code_from_wire (v);
        ASSERT_TRUE (e.has_value());
        EXPECT_FALSE (name_of (*e).empty());
        EXPECT_EQ (error_code_from_name (name_of (*e)), e);
    }
    EXPECT_FALSE (error_code_from_wire (kErrorCodeCount).has_value());
    EXPECT_EQ (name_of (static_cast<AddAction> (77)), "");
}

TEST (TypesNames, ErrorCodeValuesMatchTheSpec) {
    EXPECT_EQ (static_cast<int> (ErrorCode::malformed), 1);
    EXPECT_EQ (static_cast<int> (ErrorCode::context), 4);
    EXPECT_EQ (static_cast<int> (ErrorCode::id_range), 8);
    EXPECT_EQ (static_cast<int> (ErrorCode::graph), 12);
    EXPECT_EQ (static_cast<int> (ErrorCode::internal), 13);
}

TEST (TypesNames, OpcodeClasses) {
    EXPECT_EQ (classify (0x0001), OpcodeClass::call);
    EXPECT_EQ (classify (0x0031), OpcodeClass::call);
    EXPECT_EQ (classify (0x0100), OpcodeClass::rt);
    EXPECT_EQ (classify (0x0107), OpcodeClass::rt);
    EXPECT_EQ (classify (0x8001), OpcodeClass::event);
    EXPECT_EQ (classify (0x8004), OpcodeClass::event);
    for (const std::uint16_t bad : { 0x0000, 0x0005, 0x0108, 0x8000, 0x8005, 0xFFFF }) {
        EXPECT_FALSE (classify (bad).has_value()) << bad;
    }
    EXPECT_EQ (opcode_from_name ("node.new"), Opcode::node_new);
    EXPECT_EQ (opcode_from_name ("client.reset"), Opcode::client_reset);
    EXPECT_FALSE (opcode_from_name ("node_new").has_value());
}

TEST (TypesNames, NameSyntax) {
    for (const char* ok : { "a", "_", "A9", "freq", "_x_1" }) {
        EXPECT_TRUE (is_valid_name (ok)) << ok;
    }
    for (const char* bad : { "", "1a", "a-b", "a b", "a.b", "\xc3\xa9" }) {
        EXPECT_FALSE (is_valid_name (bad)) << bad;
    }
    EXPECT_TRUE (is_valid_name (std::string (63, 'a')));
    EXPECT_FALSE (is_valid_name (std::string (64, 'a')));
}

TEST (TypesNames, ConstantsFollowTheSpec) {
    EXPECT_EQ (kMagic, 0x504D5642U);
    EXPECT_EQ (kGraphMagic, 0x44475642U);
    EXPECT_EQ (kMaxMessage, 65536U);
    EXPECT_EQ (kClientIdSize, 1U << 24);
}
