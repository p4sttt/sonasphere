#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

#include "bitvm/proto/transport.hpp"
#include "test_util.hpp"

using namespace bitvm::proto;
using namespace bitvm::proto::testing;

namespace
{

struct Pair {
    Fd a;
    Fd b;
};

Pair
make_pair () {
    int fds[2] = { -1, -1 };
    EXPECT_EQ (::socketpair (AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, fds), 0);
    return Pair{ Fd (fds[0]), Fd (fds[1]) };
}

}  // namespace

TEST (TransportSocketpair, OneMessageIsOnePacket) {
    auto       p   = make_pair();
    const auto one = encode_ok (Message{ 1, Call{ Status{} } });
    const auto two = encode_ok (Message{ 2, Bundle{ 5, { GroupClear{ 0 } } } });
    ASSERT_TRUE (send_message (p.a.get(), one).has_value());
    ASSERT_TRUE (send_message (p.a.get(), two).has_value());

    std::vector<std::byte> buf;
    ASSERT_TRUE (recv_message (p.b.get(), buf).has_value());
    EXPECT_EQ (buf, one);
    ASSERT_TRUE (recv_message (p.b.get(), buf).has_value());
    EXPECT_EQ (buf, two);

    // and the other way
    ASSERT_TRUE (send_message (p.b.get(), two).has_value());
    ASSERT_TRUE (recv_message (p.a.get(), buf).has_value());
    EXPECT_EQ (decode (buf).value().seq, 2U);
}

TEST (TransportSocketpair, EverySampleSurvivesTheSocket) {
    auto                   p = make_pair();
    std::vector<std::byte> buf;
    for (const auto& m : edge_samples()) {
        const auto bytes = encode_ok (m);
        ASSERT_TRUE (send_message (p.a.get(), bytes).has_value());
        ASSERT_TRUE (recv_message (p.b.get(), buf).has_value());
        EXPECT_EQ (decode (buf).value(), m);
    }
}

TEST (TransportSocketpair, LargestMessageFits) {
    auto p = make_pair();
    ASSERT_TRUE (raise_send_buffer (p.a.get()).has_value());
    // a header-only prefix padded to exactly 65536 bytes; the transport does not parse it
    std::vector<std::byte> big (kMaxMessage, std::byte{ 0x5a });
    ASSERT_TRUE (send_message (p.a.get(), big).has_value());
    std::vector<std::byte> buf;
    ASSERT_TRUE (recv_message (p.b.get(), buf).has_value());
    EXPECT_EQ (buf, big);
}

TEST (TransportSocketpair, SendRefusesOversizeMessages) {
    auto                   p = make_pair();
    std::vector<std::byte> big (kMaxMessage + 1);
    auto                   r = send_message (p.a.get(), big);
    ASSERT_FALSE (r.has_value());
    EXPECT_EQ (r.error().kind, TransportErrc::too_large);
    // nothing was sent
    ASSERT_TRUE (set_non_blocking (p.b.get(), true).has_value());
    std::vector<std::byte> buf;
    EXPECT_EQ (recv_message (p.b.get(), buf).error().kind, TransportErrc::would_block);
}

TEST (TransportSocketpair, RecvDropsOversizePackets) {
    auto p = make_pair();
    ASSERT_TRUE (raise_send_buffer (p.a.get()).has_value());
    // bypass send_message to put a 65537 byte packet on the wire
    std::vector<std::byte> big (kMaxMessage + 1, std::byte{ 1 });
    ASSERT_EQ (
        ::send (p.a.get(), big.data(), big.size(), MSG_NOSIGNAL),
        static_cast<ssize_t> (big.size())
    );
    const auto next = encode_ok (Message{ 3, Call{ Sync{} } });
    ASSERT_TRUE (send_message (p.a.get(), next).has_value());

    std::vector<std::byte> buf;
    auto                   r = recv_message (p.b.get(), buf);
    ASSERT_FALSE (r.has_value());
    EXPECT_EQ (r.error().kind, TransportErrc::too_large);
    // the next packet is intact
    ASSERT_TRUE (recv_message (p.b.get(), buf).has_value());
    EXPECT_EQ (buf, next);
}

TEST (TransportSocketpair, ClosedPeer) {
    auto                   p = make_pair();
    std::vector<std::byte> buf;
    p.a.reset();
    auto r = recv_message (p.b.get(), buf);
    ASSERT_FALSE (r.has_value());
    EXPECT_EQ (r.error().kind, TransportErrc::closed);

    auto q = make_pair();
    q.b.reset();
    const auto bytes = encode_ok (Message{ 1, Call{ Sync{} } });
    auto       s     = send_message (q.a.get(), bytes);
    ASSERT_FALSE (s.has_value());
    EXPECT_EQ (s.error().kind, TransportErrc::closed);
}

TEST (TransportSocketpair, NonBlockingWouldBlock) {
    auto p = make_pair();
    ASSERT_TRUE (set_non_blocking (p.a.get(), true).has_value());
    ASSERT_TRUE (set_non_blocking (p.b.get(), true).has_value());
    std::vector<std::byte> buf;
    EXPECT_EQ (recv_message (p.b.get(), buf).error().kind, TransportErrc::would_block);

    // fill the socket until the sender would block
    ASSERT_TRUE (raise_send_buffer (p.a.get()).has_value());
    std::vector<std::byte> chunk (kMaxMessage, std::byte{ 7 });
    int                    sent = 0;
    while (true) {
        auto r = send_message (p.a.get(), chunk);
        if (!r) {
            EXPECT_EQ (r.error().kind, TransportErrc::would_block);
            break;
        }
        ASSERT_LT (++sent, 1000);
    }
    EXPECT_GE (sent, 4);  // a 256 KiB buffer holds at least four full messages
    // draining one frees room again
    ASSERT_TRUE (recv_message (p.b.get(), buf).has_value());
    EXPECT_TRUE (send_message (p.a.get(), chunk).has_value());
}

TEST (TransportSocketpair, SendBufferIsRaised) {
    auto p = make_pair();
    ASSERT_TRUE (raise_send_buffer (p.a.get()).has_value());
    int       value = 0;
    socklen_t len   = sizeof (value);
    ASSERT_EQ (::getsockopt (p.a.get(), SOL_SOCKET, SO_SNDBUF, &value, &len), 0);
    // the kernel reports twice what was requested
    EXPECT_GE (static_cast<std::size_t> (value), kMinSendBuffer);
}

TEST (TransportSocketpair, FdIsMoveOnlyAndCloses) {
    int raw[2] = { -1, -1 };
    ASSERT_EQ (::socketpair (AF_UNIX, SOCK_SEQPACKET, 0, raw), 0);
    Fd a (raw[0]);
    Fd b (raw[1]);
    Fd moved = std::move (a);
    EXPECT_FALSE (a.valid());
    EXPECT_TRUE (moved.valid());
    const int number = moved.get();
    moved.reset();
    EXPECT_FALSE (moved.valid());
    EXPECT_EQ (::fcntl (number, F_GETFD), -1);  // closed
    EXPECT_EQ (errno, EBADF);
    EXPECT_EQ (b.release(), raw[1]);
    ::close (raw[1]);
}
