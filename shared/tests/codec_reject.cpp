#include <gtest/gtest.h>

#include "test_util.hpp"

using namespace bitvm::proto;
using namespace bitvm::proto::testing;

namespace
{

Bytes
concat (std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const auto& p : parts) {
        out.insert (out.end(), p.begin(), p.end());
    }
    return out;
}

// header + payload with a correct length
Bytes
message (std::uint16_t kind, const Bytes& payload, std::uint16_t flags = 0, std::uint32_t seq = 1) {
    Bytes out (12);
    write_u16 (out, 4, kind);
    write_u16 (out, 6, flags);
    write_u32 (out, 8, seq);
    out.insert (out.end(), payload.begin(), payload.end());
    fix_length (out);
    return out;
}

Bytes
command (std::uint16_t opcode, const Bytes& fields) {
    Bytes out (4);
    write_u16 (out, 0, opcode);
    write_u16 (out, 2, static_cast<std::uint16_t> (4 + fields.size()));
    out.insert (out.end(), fields.begin(), fields.end());
    return out;
}

const Bytes kTime0 = from_hex ("0000000000000000");

Bytes
bundle (const Bytes& commands, const Bytes& time = kTime0) {
    return message (4, concat ({ time, commands }));
}

// the usual valid commands
const Bytes kGroupClear = command (0x0106, from_hex ("00000000"));

void
expect_error (
    const Bytes&  bytes,
    ErrorCode     code,
    std::size_t   offset,
    std::uint16_t index = kNoCommand
) {
    auto m = decode (bytes);
    ASSERT_FALSE (m.has_value()) << "decoded: " << to_hex (bytes);
    EXPECT_EQ (m.error().code, code) << m.error().detail;
    EXPECT_EQ (m.error().offset, offset) << m.error().detail;
    EXPECT_EQ (m.error().command_index, index) << m.error().detail;
}

}  // namespace

TEST (CodecReject, TruncationAtEveryByte) {
    for (const auto& sample : edge_samples()) {
        const auto bytes = encode_ok (sample);
        ASSERT_FALSE (bytes.empty());
        for (std::size_t n = 0; n < bytes.size(); ++n) {
            // as received: the length field no longer matches the packet
            Bytes cut (bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t> (n));
            EXPECT_FALSE (decode (cut).has_value()) << "prefix " << n;
            // with a repaired length the decoder must still reject, except for a bundle cut
            // exactly between two commands, which is a shorter valid bundle
            if (n >= 12) {
                fix_length (cut);
                auto m = decode (cut);
                if (m.has_value()) {
                    ASSERT_TRUE (std::holds_alternative<Bundle> (m->body)) << "prefix " << n;
                    EXPECT_EQ (encode_ok (*m), cut) << "prefix " << n;
                }
            }
        }
    }
}

TEST (CodecReject, LengthMismatch) {
    auto good   = bundle (kGroupClear);
    auto longer = good;
    write_u32 (longer, 0, static_cast<std::uint32_t> (good.size() + 1));
    expect_error (longer, ErrorCode::malformed, 0);
    auto shorter = good;
    write_u32 (shorter, 0, static_cast<std::uint32_t> (good.size() - 1));
    expect_error (shorter, ErrorCode::malformed, 0);
    auto tiny = good;
    write_u32 (tiny, 0, 11);
    expect_error (tiny, ErrorCode::malformed, 0);
    expect_error ({}, ErrorCode::malformed, 0);
    expect_error (Bytes (11), ErrorCode::malformed, 0);
    // above the limit, even when the length field is honest
    Bytes huge (65537);
    write_u16 (huge, 4, 4);
    fix_length (huge);
    expect_error (huge, ErrorCode::malformed, 0);
}

TEST (CodecReject, TrailingBytesAfterEveryMessage) {
    for (const auto& sample : edge_samples()) {
        auto bytes = encode_ok (sample);
        bytes.push_back (std::byte{ 0 });
        fix_length (bytes);
        EXPECT_FALSE (decode (bytes).has_value()) << to_hex (bytes).substr (0, 60);
    }
}

TEST (CodecReject, UnknownKindAndFlags) {
    for (const std::uint16_t kind : { 0, 8, 9, 255, 0xFFFF }) {
        expect_error (message (kind, {}), ErrorCode::unsupported, 4);
    }
    expect_error (message (4, concat ({ kTime0, kGroupClear }), 1), ErrorCode::unsupported, 6);
    expect_error (message (4, concat ({ kTime0, kGroupClear }), 0x8000), ErrorCode::unsupported, 6);
}

TEST (CodecReject, UnknownOpcodes) {
    expect_error (
        bundle (concat ({ kGroupClear, command (0x0999, {}) })),
        ErrorCode::unsupported,
        28,
        1
    );
    expect_error (bundle (command (0x0000, {})), ErrorCode::unsupported, 20, 0);
    expect_error (message (5, command (0x0999, {})), ErrorCode::unsupported, 12, kNoCommand);
    expect_error (message (7, command (0x8999, {})), ErrorCode::unsupported, 12, kNoCommand);
    // 0x0108 is just past the last rt command
    expect_error (bundle (command (0x0108, {})), ErrorCode::unsupported, 20, 0);
}

TEST (CodecReject, CommandInTheWrongMessage) {
    expect_error (message (5, kGroupClear), ErrorCode::context, 12);
    expect_error (bundle (command (0x0001, {})), ErrorCode::context, 20, 0);
    expect_error (bundle (command (0x8001, from_hex ("01000000"))), ErrorCode::context, 20, 0);
    expect_error (message (5, command (0x8001, from_hex ("01000000"))), ErrorCode::context, 12);
    expect_error (message (7, kGroupClear), ErrorCode::context, 12);
    expect_error (message (7, command (0x0001, {})), ErrorCode::context, 12);
}

TEST (CodecReject, BoolOutsideZeroAndOne) {
    // second command of a bundle: node.run id=0 run=2. the bool sits at byte 20 + 8 + 4 + 4
    const auto run = [] (const char* flag) {
        return bundle (
            concat ({ kGroupClear, command (0x0104, from_hex (std::string ("00000000") + flag)) })
        );
    };
    EXPECT_TRUE (decode (run ("00")).has_value());
    EXPECT_TRUE (decode (run ("01")).has_value());
    expect_error (run ("02"), ErrorCode::malformed, 36, 1);
    expect_error (run ("ff"), ErrorCode::malformed, 36, 1);
    // tree reply: truncated and running flags
    const auto tree = [] (const char* truncated, const char* running) {
        return message (
            6,
            from_hex (
                std::string ("0300 0000 ") + truncated + " 0100 00000000 ffffffff 00 00000000 " +
                running
            )
        );
    };
    EXPECT_TRUE (decode (tree ("01", "00")).has_value());
    expect_error (tree ("02", "00"), ErrorCode::malformed, 16);
    expect_error (tree ("00", "07"), ErrorCode::malformed, 32);
}

TEST (CodecReject, NanAndInfinity) {
    for (const char* bad : { "0000c07f", "0000807f", "000080ff", "0100807f" }) {
        const std::string value = bad;
        // node.set ramp
        expect_error (
            bundle (command (0x0103, from_hex ("01000000 " + value + " 00 0000"))),
            ErrorCode::invalid_arg,
            28,
            0
        );
        // pair value inside node.set
        expect_error (
            bundle (command (0x0103, from_hex ("01000000 00000000 00 0100 0000 " + value))),
            ErrorCode::invalid_arg,
            37,
            0
        );
        // pair value inside node.new
        expect_error (
            bundle (
                command (0x0100, from_hex ("01000000 01000000 00 00000000 00 0100 0000 " + value))
            ),
            ErrorCode::invalid_arg,
            42,
            0
        );
        // cbus.set value
        expect_error (
            bundle (command (0x0107, from_hex ("00000000 0100 " + value))),
            ErrorCode::invalid_arg,
            30,
            0
        );
        // status reply cpu_avg
        expect_error (
            message (
                6,
                from_hex (
                    "0200 0000 0000000000000000 00000000 4000 " + value +
                    " 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 "
                    "00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000"
                )
            ),
            ErrorCode::invalid_arg,
            30
        );
    }
}

TEST (CodecReject, EnumerationsOutsideTheirTables) {
    // add action 5
    expect_error (
        bundle (command (0x0101, from_hex ("01000000 05 00000000"))),
        ErrorCode::malformed,
        28,
        0
    );
    expect_error (
        bundle (command (0x0105, from_hex ("01000000 ff 00000000"))),
        ErrorCode::malformed,
        28,
        0
    );
    // bus kind 2
    expect_error (message (5, command (0x0030, from_hex ("02 0100"))), ErrorCode::malformed, 16);
    expect_error (
        message (5, command (0x0031, from_hex ("02 00000000"))),
        ErrorCode::malformed,
        16
    );
    // curve other than lin is reserved
    expect_error (
        bundle (command (0x0103, from_hex ("01000000 00000000 01 0000"))),
        ErrorCode::unsupported,
        32,
        0
    );
    // node kind 2 in a tree entry
    expect_error (
        message (6, from_hex ("0300 0000 00 0100 00000000 ffffffff 02 00000000 00")),
        ErrorCode::malformed,
        27
    );
    // error code 14 does not exist
    expect_error (message (3, from_hex ("0e00 0000")), ErrorCode::unsupported, 12);
    expect_error (message (6, from_hex ("0000 0e00 0000")), ErrorCode::unsupported, 14);
    // node.new reserved flag bits
    expect_error (
        bundle (command (0x0100, from_hex ("01000000 01000000 00 00000000 02 0000"))),
        ErrorCode::unsupported,
        37,
        0
    );
    // hello with a reserved event bit
    expect_error (
        message (1, from_hex ("42564d50 0000 0100 0000000000000000 00 08000000 0000")),
        ErrorCode::unsupported,
        29
    );
}

TEST (CodecReject, NegativeBundleTime) {
    expect_error (bundle (kGroupClear, from_hex ("ffffffffffffffff")), ErrorCode::invalid_arg, 12);
    expect_error (bundle (kGroupClear, from_hex ("0000000000000080")), ErrorCode::invalid_arg, 12);
    EXPECT_TRUE (decode (bundle (kGroupClear, from_hex ("ffffffffffffff7f"))).has_value());
}

TEST (CodecReject, EmptyBundleAndCall) {
    expect_error (message (4, kTime0), ErrorCode::malformed, 20);
    expect_error (message (5, {}), ErrorCode::malformed, 12);
    expect_error (message (7, {}), ErrorCode::malformed, 12);
    expect_error (
        message (5, concat ({ command (0x0001, {}), command (0x0001, {}) })),
        ErrorCode::malformed,
        12
    );
}

TEST (CodecReject, StringRules) {
    const auto hello = [] (const Bytes& name) {
        return message (
            1,
            concat ({ from_hex ("42564d50 0000 0100 0000000000000000 00 00000000"), name })
        );
    };
    EXPECT_TRUE (decode (hello (from_hex ("0000"))).has_value());
    // longer than the packet
    expect_error (hello (from_hex ("0500 6162")), ErrorCode::malformed, 35);
    // 1025 bytes
    Bytes too_long = from_hex ("0104");
    too_long.insert (too_long.end(), 1025, std::byte{ 'a' });
    expect_error (hello (too_long), ErrorCode::malformed, 33);
    Bytes max = from_hex ("0004");
    max.insert (max.end(), 1024, std::byte{ 'a' });
    EXPECT_TRUE (decode (hello (max)).has_value());

    // path: empty, with a zero byte, 4096 bytes, 4095 bytes
    const auto load = [] (const Bytes& path) {
        return message (5, command (0x0021, concat ({ path, from_hex ("00000000 00000000") })));
    };
    expect_error (load (from_hex ("0000")), ErrorCode::malformed, 16);
    expect_error (load (from_hex ("0300 61 00 62")), ErrorCode::malformed, 16);
    Bytes p4096 = from_hex ("0010");
    p4096.insert (p4096.end(), 4096, std::byte{ 'a' });
    expect_error (load (p4096), ErrorCode::malformed, 16);
    Bytes p4095 = from_hex ("ff0f");
    p4095.insert (p4095.end(), 4095, std::byte{ 'a' });
    EXPECT_TRUE (decode (load (p4095)).has_value());
}

TEST (CodecReject, CommandSizeRules) {
    // size below 4
    Bytes tiny = kGroupClear;
    write_u16 (tiny, 2, 3);
    expect_error (bundle (tiny), ErrorCode::malformed, 22, 0);
    write_u16 (tiny, 2, 0);
    expect_error (bundle (tiny), ErrorCode::malformed, 22, 0);
    // size past the end of the message
    Bytes past = kGroupClear;
    write_u16 (past, 2, 9);
    expect_error (bundle (past), ErrorCode::malformed, 22, 0);
    // size larger than the fields: the extra byte is trailing
    Bytes more = concat ({ kGroupClear, Bytes{ std::byte{ 0 } } });
    write_u16 (more, 2, 9);
    expect_error (bundle (more), ErrorCode::malformed, 28, 0);
    // size smaller than the fields: the command is cut short
    Bytes less = kGroupClear;
    write_u16 (less, 2, 6);
    less.resize (6);
    expect_error (bundle (less), ErrorCode::malformed, 24, 0);
    // a lone header byte after a valid command
    expect_error (bundle (concat ({ kGroupClear, from_hex ("06") })), ErrorCode::malformed, 28, 1);
}

TEST (CodecReject, ListCountLargerThanTheData) {
    // node.free claims 65535 ids and carries none
    expect_error (bundle (command (0x0102, from_hex ("ffff"))), ErrorCode::malformed, 26, 0);
    // node.new claims 65535 pairs
    expect_error (
        bundle (command (0x0100, from_hex ("01000000 01000000 00 00000000 00 ffff"))),
        ErrorCode::malformed,
        40,
        0
    );
    // cbus.set, tree reply
    expect_error (
        bundle (command (0x0107, from_hex ("00000000 ffff"))),
        ErrorCode::malformed,
        30,
        0
    );
    expect_error (message (6, from_hex ("0300 0000 00 ffff")), ErrorCode::malformed, 19);
}

TEST (CodecReject, HandshakeRules) {
    expect_error (
        message (1, from_hex ("00000000 0000 0100 0000000000000000 00 00000000 0000")),
        ErrorCode::malformed,
        12
    );
    expect_error (message (2, from_hex ("00000000")), ErrorCode::malformed, 12);
}

TEST (CodecReject, ReplyRules) {
    // unknown status
    expect_error (message (6, from_hex ("1000 6300 0000")), ErrorCode::unsupported, 14);
    // ok reply to an opcode that is not a call
    expect_error (message (6, from_hex ("0001 0000")), ErrorCode::unsupported, 12);
    expect_error (message (6, from_hex ("e703 0000")), ErrorCode::unsupported, 12);
    // empty result with extra bytes
    expect_error (message (6, from_hex ("0100 0000 00")), ErrorCode::malformed, 16);
    // truncated message string
    expect_error (message (6, from_hex ("0100 0100 0900 61")), ErrorCode::malformed, 18);
}

TEST (CodecReject, ErrorDetailsPointAtTheCommand) {
    // second of three commands is bad
    auto m = decode (bundle (
        concat ({ kGroupClear, command (0x0105, from_hex ("01000000 09 00000000")), kGroupClear })
    ));
    ASSERT_FALSE (m.has_value());
    EXPECT_EQ (m.error().command_index, 1);
    EXPECT_EQ (m.error().code, ErrorCode::malformed);
    EXPECT_FALSE (m.error().detail.empty());
}
