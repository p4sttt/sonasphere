#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "bitvm/proto/transport.hpp"
#include "test_util.hpp"

using namespace bitvm::proto;
using namespace bitvm::proto::testing;

namespace fs = std::filesystem;

namespace
{

// a fresh private directory removed at the end of the test
class TempDir {
public:
    TempDir () {
        std::string pattern = (fs::temp_directory_path() / "bitvm-proto-XXXXXX").string();
        EXPECT_NE (::mkdtemp (pattern.data()), nullptr);
        path_ = pattern;
    }

    ~TempDir () {
        std::error_code ec;
        fs::remove_all (path_, ec);
    }

    TempDir (const TempDir&)            = delete;
    TempDir& operator= (const TempDir&) = delete;

    [[nodiscard]] const std::string&
    path () const {
        return path_;
    }

private:
    std::string path_;
};

}  // namespace

TEST (TransportPath, WithRuntimeDir) {
    EXPECT_EQ (
        socket_path ("", "/run/user/1000", 1000).value(),
        "/run/user/1000/sonasphere/bitvm.sock"
    );
    EXPECT_EQ (
        socket_path ("foo", "/run/user/1000", 1000).value(),
        "/run/user/1000/sonasphere/bitvm-foo.sock"
    );
}

TEST (TransportPath, WithoutRuntimeDir) {
    EXPECT_EQ (socket_path ("", std::nullopt, 1234).value(), "/tmp/sonasphere-1234/bitvm.sock");
    EXPECT_EQ (socket_path ("foo", std::nullopt, 7).value(), "/tmp/sonasphere-7/bitvm-foo.sock");
    // an empty variable counts as unset
    EXPECT_EQ (socket_path ("", std::string_view (""), 5).value(), "/tmp/sonasphere-5/bitvm.sock");
}

TEST (TransportPath, ServerNamesAreValidated) {
    for (const char* bad : { "a b", "a/b", "../x", "1x", "a.b" }) {
        auto r = socket_path (bad, "/run/user/1", 1);
        ASSERT_FALSE (r.has_value()) << bad;
        EXPECT_EQ (r.error().kind, TransportErrc::bad_name);
    }
}

TEST (TransportPath, ReadsTheEnvironment) {
    const char*       old   = std::getenv ("XDG_RUNTIME_DIR");
    const std::string saved = old == nullptr ? "" : old;

    ASSERT_EQ (::setenv ("XDG_RUNTIME_DIR", "/run/user/4242", 1), 0);
    EXPECT_EQ (socket_path().value(), "/run/user/4242/sonasphere/bitvm.sock");
    EXPECT_EQ (socket_path ("band").value(), "/run/user/4242/sonasphere/bitvm-band.sock");

    ASSERT_EQ (::unsetenv ("XDG_RUNTIME_DIR"), 0);
    EXPECT_EQ (
        socket_path().value(),
        "/tmp/sonasphere-" + std::to_string (::getuid()) + "/bitvm.sock"
    );

    ASSERT_EQ (::setenv ("XDG_RUNTIME_DIR", "", 1), 0);
    EXPECT_EQ (
        socket_path().value(),
        "/tmp/sonasphere-" + std::to_string (::getuid()) + "/bitvm.sock"
    );

    if (old != nullptr) {
        ::setenv ("XDG_RUNTIME_DIR", saved.c_str(), 1);
    } else {
        ::unsetenv ("XDG_RUNTIME_DIR");
    }
}

TEST (TransportDirectory, IsCreatedPrivate) {
    TempDir    tmp;
    const auto dir = tmp.path() + "/sonasphere";
    ASSERT_TRUE (prepare_socket_dir (dir).has_value());
    struct stat st{};
    ASSERT_EQ (::stat (dir.c_str(), &st), 0);
    EXPECT_EQ (st.st_mode & 0777, 0700U);
    // second call accepts the directory it made
    EXPECT_TRUE (prepare_socket_dir (dir).has_value());
}

TEST (TransportDirectory, RefusesOpenDirectories) {
    TempDir    tmp;
    const auto dir = tmp.path() + "/open";
    ASSERT_EQ (::mkdir (dir.c_str(), 0755), 0);
    ASSERT_EQ (::chmod (dir.c_str(), 0755), 0);
    auto r = prepare_socket_dir (dir);
    ASSERT_FALSE (r.has_value());
    EXPECT_EQ (r.error().kind, TransportErrc::bad_directory);

    ASSERT_EQ (::chmod (dir.c_str(), 0710), 0);  // group may enter
    EXPECT_EQ (prepare_socket_dir (dir).error().kind, TransportErrc::bad_directory);
    ASSERT_EQ (::chmod (dir.c_str(), 0701), 0);  // others may enter
    EXPECT_EQ (prepare_socket_dir (dir).error().kind, TransportErrc::bad_directory);
    ASSERT_EQ (::chmod (dir.c_str(), 0700), 0);
    EXPECT_TRUE (prepare_socket_dir (dir).has_value());
}

TEST (TransportDirectory, RefusesLinksAndFiles) {
    TempDir    tmp;
    const auto real = tmp.path() + "/real";
    ASSERT_EQ (::mkdir (real.c_str(), 0700), 0);
    const auto link = tmp.path() + "/link";
    ASSERT_EQ (::symlink (real.c_str(), link.c_str()), 0);
    EXPECT_EQ (prepare_socket_dir (link).error().kind, TransportErrc::bad_directory);

    const auto file = tmp.path() + "/file";
    { std::ofstream (file) << "x"; }
    EXPECT_EQ (prepare_socket_dir (file).error().kind, TransportErrc::bad_directory);
}

TEST (TransportDirectory, RefusesDirectoriesOfOtherUsers) {
    // "/" belongs to root with mode 0755: refused whoever runs the test
    auto r = prepare_socket_dir ("/");
    ASSERT_FALSE (r.has_value());
    EXPECT_EQ (r.error().kind, TransportErrc::bad_directory);
}

TEST (TransportListen, ConnectAcceptAndExchange) {
    TempDir    tmp;
    const auto path     = tmp.path() + "/sonasphere/bitvm.sock";
    auto       listener = listen (path);
    ASSERT_TRUE (listener.has_value()) << listener.error().detail;

    // directory 0700, socket not reachable by others
    struct stat st{};
    ASSERT_EQ (::stat (path.c_str(), &st), 0);
    EXPECT_TRUE (S_ISSOCK (st.st_mode));
    EXPECT_EQ (st.st_mode & 077, 0U);

    // nobody is waiting yet
    auto none = accept (*listener);
    ASSERT_FALSE (none.has_value());
    EXPECT_EQ (none.error().kind, TransportErrc::would_block);

    auto client = connect (path);
    ASSERT_TRUE (client.has_value()) << client.error().detail;
    auto server = accept (*listener, false);
    ASSERT_TRUE (server.has_value()) << server.error().detail;

    const auto hello = encode_ok (Message{ 1, Hello{ 0, 1, 42, 0, kEventMask, "test" } });
    ASSERT_TRUE (send_message (client->get(), hello).has_value());
    std::vector<std::byte> buf;
    ASSERT_TRUE (recv_message (server->get(), buf).has_value());
    EXPECT_EQ (decode (buf).value().seq, 1U);

    // both ends have the raised send buffer
    for (const int fd : { client->get(), server->get() }) {
        int       value = 0;
        socklen_t len   = sizeof (value);
        ASSERT_EQ (::getsockopt (fd, SOL_SOCKET, SO_SNDBUF, &value, &len), 0);
        EXPECT_GE (static_cast<std::size_t> (value), kMinSendBuffer);
    }
}

TEST (TransportListen, AcceptedSocketsAreNonBlockingByDefault) {
    TempDir    tmp;
    const auto path     = tmp.path() + "/d/s.sock";
    auto       listener = listen (path);
    ASSERT_TRUE (listener.has_value());
    auto client = connect (path);
    ASSERT_TRUE (client.has_value());
    auto server = accept (*listener);
    ASSERT_TRUE (server.has_value());
    std::vector<std::byte> buf;
    EXPECT_EQ (recv_message (server->get(), buf).error().kind, TransportErrc::would_block);
}

TEST (TransportListen, StaleSocketFileIsReplaced) {
    TempDir    tmp;
    const auto path = tmp.path() + "/d/s.sock";
    {
        auto first = listen (path);
        ASSERT_TRUE (first.has_value());
        // closing the listener leaves the file behind, as a crash would
    }
    struct stat st{};
    ASSERT_EQ (::stat (path.c_str(), &st), 0);
    auto second = listen (path);
    ASSERT_TRUE (second.has_value()) << second.error().detail;
    auto client = connect (path);
    EXPECT_TRUE (client.has_value());
}

TEST (TransportListen, RunningServerIsNotReplaced) {
    TempDir    tmp;
    const auto path  = tmp.path() + "/d/s.sock";
    auto       first = listen (path);
    ASSERT_TRUE (first.has_value());
    auto second = listen (path);
    ASSERT_FALSE (second.has_value());
    EXPECT_EQ (second.error().kind, TransportErrc::already_running);
    // the first one still works
    EXPECT_TRUE (connect (path).has_value());
}

TEST (TransportListen, FullAcceptQueueDoesNotBlockTheExistingServerProbe) {
    TempDir    tmp;
    const auto path     = tmp.path() + "/d/s.sock";
    auto       listener = listen (path, 1);
    ASSERT_TRUE (listener.has_value()) << listener.error().detail;
    auto first_client = connect (path);
    ASSERT_TRUE (first_client.has_value());
    auto second_client = connect (path);
    ASSERT_TRUE (second_client.has_value());

    struct stat before{};
    ASSERT_EQ (::stat (path.c_str(), &before), 0);
    const auto child = ::fork();
    ASSERT_GE (child, 0);
    if (child == 0) {
        ::signal (SIGALRM, SIG_DFL);
        ::alarm (2);
        const auto probe = listen (path);
        ::_exit (!probe && probe.error().kind == TransportErrc::already_running ? 0 : 1);
    }
    int   status = 0;
    pid_t waited = -1;
    do {
        waited = ::waitpid (child, &status, 0);
    } while (waited == -1 && errno == EINTR);
    ASSERT_EQ (waited, child);
    ASSERT_TRUE (WIFEXITED (status)) << "existing server probe blocked until its alarm";
    EXPECT_EQ (WEXITSTATUS (status), 0);

    struct stat after{};
    ASSERT_EQ (::stat (path.c_str(), &after), 0);
    EXPECT_EQ (after.st_dev, before.st_dev);
    EXPECT_EQ (after.st_ino, before.st_ino);
    EXPECT_TRUE (accept (*listener).has_value());
    EXPECT_TRUE (accept (*listener).has_value());
    EXPECT_TRUE (connect (path).has_value());
}

TEST (TransportListen, RegularFilesAreLeftAlone) {
    TempDir    tmp;
    const auto dir = tmp.path() + "/d";
    ASSERT_TRUE (prepare_socket_dir (dir).has_value());
    const auto path = dir + "/s.sock";
    { std::ofstream (path) << "keep me"; }
    auto r = listen (path);
    ASSERT_FALSE (r.has_value());
    EXPECT_EQ (r.error().kind, TransportErrc::not_a_socket);
    EXPECT_TRUE (fs::exists (path));
}

TEST (TransportListen, OpenDirectoryStopsTheServer) {
    TempDir    tmp;
    const auto dir = tmp.path() + "/d";
    ASSERT_EQ (::mkdir (dir.c_str(), 0777), 0);
    ASSERT_EQ (::chmod (dir.c_str(), 0777), 0);
    auto r = listen (dir + "/s.sock");
    ASSERT_FALSE (r.has_value());
    EXPECT_EQ (r.error().kind, TransportErrc::bad_directory);
}

TEST (TransportListen, ConnectWithoutServer) {
    TempDir tmp;
    auto    r = connect (tmp.path() + "/nothing.sock");
    ASSERT_FALSE (r.has_value());
    EXPECT_EQ (r.error().kind, TransportErrc::system);
    EXPECT_EQ (r.error().sys_errno, ENOENT);
}

TEST (TransportListen, PathMustFitSockaddr) {
    const std::string too_long = "/tmp/" + std::string (200, 'a') + "/s.sock";
    auto              l        = listen (too_long);
    ASSERT_FALSE (l.has_value());
    EXPECT_EQ (l.error().kind, TransportErrc::path_too_long);
    auto c = connect (too_long);
    ASSERT_FALSE (c.has_value());
    EXPECT_EQ (c.error().kind, TransportErrc::path_too_long);
}
