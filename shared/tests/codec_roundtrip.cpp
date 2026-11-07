#include <gtest/gtest.h>

#include <limits>

#include "test_util.hpp"

using namespace bitvm::proto;
using namespace bitvm::proto::testing;

TEST (CodecRoundtrip, EveryEdgeSample) {
    const auto samples = edge_samples();
    ASSERT_GT (samples.size(), 40U);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        SCOPED_TRACE ("sample " + std::to_string (i));
        auto bytes = encode (samples[i]);
        ASSERT_TRUE (bytes.has_value()) << bytes.error().detail;
        EXPECT_LE (bytes->size(), kMaxMessage);
        auto back = decode (*bytes);
        ASSERT_TRUE (back.has_value()) << back.error().detail;
        EXPECT_EQ (*back, samples[i]);
        // and the bytes are stable: encode(decode(bytes)) == bytes
        auto again = encode (*back);
        ASSERT_TRUE (again.has_value());
        EXPECT_EQ (*again, *bytes);
    }
}

TEST (CodecRoundtrip, EveryMessageKindAndOpcodeIsCovered) {
    bool kinds[8]   = {};
    bool rt[8]      = {};
    bool calls[11]  = {};
    bool results[7] = {};
    bool events[4]  = {};
    for (const auto& m : edge_samples()) {
        kinds[static_cast<int> (kind_of (m.body))] = true;
        if (const auto* b = std::get_if<Bundle> (&m.body)) {
            for (const auto& c : b->commands) {
                rt[c.index()] = true;
            }
        } else if (const auto* c = std::get_if<Call> (&m.body)) {
            calls[c->command.index()] = true;
        } else if (const auto* r = std::get_if<Reply> (&m.body)) {
            if (const auto* ok = std::get_if<CallResult> (&r->outcome)) {
                results[ok->index()] = true;
            }
        } else if (const auto* e = std::get_if<Event> (&m.body)) {
            events[e->data.index()] = true;
        }
    }
    for (int k = 1; k <= 7; ++k) {
        EXPECT_TRUE (kinds[k]) << "message kind " << k;
    }
    for (int i = 0; i < 8; ++i) {
        EXPECT_TRUE (rt[i]) << "rt command " << i;
    }
    for (int i = 0; i < 11; ++i) {
        EXPECT_TRUE (calls[i]) << "call " << i;
    }
    for (int i = 0; i < 7; ++i) {
        EXPECT_TRUE (results[i]) << "result " << i;
    }
    for (int i = 0; i < 4; ++i) {
        EXPECT_TRUE (events[i]) << "event " << i;
    }
}

TEST (CodecRoundtrip, HeaderLayoutIsLittleEndian) {
    auto bytes = encode_ok (Message{ 0x04030201, Call{ Status{} } });
    // length 16 = header 12 + command 4; kind call = 5; flags 0; seq
    EXPECT_EQ (
        to_hex (bytes),
        "10000000"
        "0500"
        "0000"
        "01020304"
        "0200"
        "0400"
    );
}

TEST (CodecRoundtrip, NodeNewByHand) {
    // computed from the tables in protocol.md section 8, not from the encoder
    const Message m{
        7,
        Bundle{ 0x0102030405060708,
                { NodeNew{ 0x01000002, 1, AddAction::tail, 0x01000001, true, { { 3, 1.0F } } } } }
    };
    const auto expected = from_hex (
        "2e000000 0400 0000 07000000"       // header: length 46, kind 4, seq 7
        "0807060504030201"                  // time
        "0001 1a00"                         // opcode 0x0100, size 26
        "02000001 01000000 01 01000001 01"  // id def add target flags
        "0100 0300 0000803f"
    );  // 1 pair: index 3, 1.0f
    EXPECT_EQ (encode_ok (m), expected);
}

TEST (CodecRoundtrip, ReplyAndEventByHand) {
    EXPECT_EQ (
        to_hex (encode_ok (Message{ 9, Reply{ 0x0010, CallResult{ DefLoaded{ 5 } } } })),
        "14000000"
        "0600"
        "0000"
        "09000000"
        "1000"
        "0000"
        "05000000"
    );
    EXPECT_EQ (
        to_hex (encode_ok (Message{ 2, Reply{ 0, CallError{ ErrorCode::io, "x" } } })),
        "13000000"
        "0600"
        "0000"
        "02000000"
        "0000"
        "0a00"
        "0100"
        "78"
    );
    EXPECT_EQ (
        to_hex (encode_ok (Message{ 1, Event{ NEnd{ 0x01000001 } } })),
        "14000000"
        "0700"
        "0000"
        "01000000"
        "0180"
        "0800"
        "01000001"
    );
}

TEST (CodecRoundtrip, CallListsAreLimitedBySize) {
    // 20000 ids would need 80 KB
    NodeFree big;
    big.ids.assign (20000, 1);
    EXPECT_FALSE (encode (Message{ 1, Bundle{ 0, { big } } }).has_value());
    // the largest bundle that fits still round-trips
    NodeFree fits;
    fits.ids.assign (16000, 7);
    auto bytes = encode (Message{ 1, Bundle{ 0, { fits } } });
    ASSERT_TRUE (bytes.has_value());
    EXPECT_EQ (bytes->size(), 12U + 8U + 4U + 2U + 64000U);
    auto back = decode (*bytes);
    ASSERT_TRUE (back.has_value());
    EXPECT_EQ (std::get<Bundle> (back->body).commands.size(), 1U);
}

TEST (CodecRoundtrip, CommandSizeFieldCannotOverflow) {
    // a single command above 65535 bytes cannot be framed
    CbusSet big;
    big.values.assign (20000, 1.0F);
    EXPECT_FALSE (encode (Message{ 1, Bundle{ 0, { big } } }).has_value());
}

TEST (CodecRoundtrip, EncodeRefusesWhatDecodeWouldRefuse) {
    const auto bad = [] (Message m) {
        return !encode (m).has_value();
    };
    EXPECT_TRUE (bad (Message{ 1, Bundle{ -1, { GroupClear{ 0 } } } }));
    EXPECT_TRUE (bad (Message{ 1, Bundle{ 0, {} } }));
    EXPECT_TRUE (bad (
        Message{
            1,
            Bundle{ 0, { NodeSet{ 1, std::numeric_limits<float>::quiet_NaN(), Curve::lin, {} } } } }
    ));
    EXPECT_TRUE (bad (
        Message{ 1, Bundle{ 0, { CbusSet{ 0, { std::numeric_limits<float>::infinity() } } } } }
    ));
    EXPECT_TRUE (
        bad (Message{ 1, Bundle{ 0, { NodeSet{ 1, 0.0F, static_cast<Curve> (1), {} } } } })
    );
    EXPECT_TRUE (bad (Message{ 1, Bundle{ 0, { GroupNew{ 1, static_cast<AddAction> (5), 0 } } } }));
    EXPECT_TRUE (bad (Message{ 1, Hello{ 0, 1, 0, 0, 8, "x" } }));
    EXPECT_TRUE (bad (Message{ 1, Hello{ 0, 1, 0, 0, 0, std::string (1025, 'x') } }));
    EXPECT_TRUE (bad (Message{ 1, Call{ BufLoad{ "", 0, 0 } } }));
    EXPECT_TRUE (bad (Message{ 1, Call{ BufLoad{ std::string ("a\0b", 3), 0, 0 } } }));
    EXPECT_TRUE (bad (Message{ 1, Call{ BufLoad{ std::string (4096, 'a'), 0, 0 } } }));
    EXPECT_TRUE (bad (Message{ 1, Reply{ 0x0010, CallResult{ Empty{} } } }));  // wrong result type
    EXPECT_TRUE (bad (Message{ 1, Reply{ 0x7777, CallResult{ Empty{} } } }));  // unknown call
    EXPECT_TRUE (bad (Message{ 1, Reply{ 0, CallError{ ErrorCode::ok, "" } } }));  // error with ok
    EXPECT_TRUE (bad (Message{ 1, Reject{ static_cast<ErrorCode> (99), "" } }));
}
