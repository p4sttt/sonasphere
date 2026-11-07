#pragma once

/// @file
/// @brief Unix socket transport (docs/protocol.md section 2).
///
/// Responsibility: where the socket lives and who may touch it (path and directory
/// permissions), `listen` / `connect`, and sending or receiving exactly one message per
/// packet over SOCK_SEQPACKET. There are no threads and no event loop here: the server and the
/// client bring their own, the descriptors are plain `int` for epoll or poll.

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "bitvm/proto/types.hpp"

namespace bitvm::proto
{

/// @brief what went wrong in the transport
enum class TransportErrc : std::uint8_t {
    system,           ///< a failed system call, see TransportError::sys_errno
    too_large,        ///< message over 65536 bytes (send, or an incoming packet that was cut)
    closed,           ///< the peer closed the connection
    would_block,      ///< non-blocking socket has nothing to send or receive
    bad_directory,    ///< socket directory is a link, not ours or open to others
    already_running,  ///< another server answers on the socket
    path_too_long,    ///< does not fit sockaddr_un
    bad_name,         ///< server name is not a valid name
    not_a_socket,     ///< the path exists and is not a socket
};

/// @brief a failed transport call
struct TransportError {
    TransportErrc kind      = TransportErrc::system;
    int           sys_errno = 0;  ///< errno, 0 if the failure is not a system call
    std::string   detail;
};

/// @brief owning file descriptor: closes it on destruction, moves but does not copy.
///
/// @note This small class is kept on purpose instead of a Boost type. The standard library has
/// no unique file descriptor, and the closest Boost types are Asio's descriptor wrappers, which
/// belong to an `io_context` and its event loop (exactly what this module leaves to the server
/// and the client), and the iostreams device wrappers, which are meant for streams. A Boost
/// dependency would be paid for by every program that links the protocol, sona included.
class Fd {
public:
    Fd () noexcept = default;
    explicit Fd (int fd) noexcept : fd_ (fd) {}
    Fd (const Fd&)            = delete;
    Fd& operator= (const Fd&) = delete;
    Fd (Fd&& other) noexcept : fd_ (std::exchange (other.fd_, -1)) {}
    Fd& operator= (Fd&& other) noexcept;
    ~Fd ();

    [[nodiscard]] int
    get () const noexcept {
        return fd_;
    }
    [[nodiscard]] bool
    valid () const noexcept {
        return fd_ >= 0;
    }
    [[nodiscard]] explicit
    operator bool () const noexcept {
        return valid();
    }

    /// gives the descriptor up without closing it
    int
    release () noexcept {
        return std::exchange (fd_, -1);
    }
    void reset (int fd = -1) noexcept;

private:
    int fd_ = -1;
};

/// @brief path of the server socket: `$XDG_RUNTIME_DIR/sonasphere/bitvm[-name].sock`, or
///        `/tmp/sonasphere-<uid>/bitvm[-name].sock` when the variable is unset or empty.
/// @param server_name name of a named server (`bitvmd --name`), empty for the default one
/// @param runtime_dir value of `XDG_RUNTIME_DIR`, if set
/// @param uid user id for the fallback directory
/// @return the path, or `bad_name` if `server_name` is not a valid name
/// @details the overload with the environment calls the pure one with `getenv` and `getuid`
[[nodiscard]] std::expected<std::string, TransportError>
socket_path (std::string_view server_name, std::optional<std::string_view> runtime_dir, uid_t uid);

[[nodiscard]] std::expected<std::string, TransportError> socket_path (
    std::string_view server_name = {}
);

/// @brief makes sure the socket directory is private.
/// @details creates `dir` with mode 0700 when missing (one level). An existing one must be a real
///          directory (not a link) of the current user without group or other access, else
///          `bad_directory`.
[[nodiscard]] std::expected<void, TransportError> prepare_socket_dir (const std::string& dir);

/// @brief starts listening on `path`.
/// @details prepares the directory, removes a stale socket file (nobody answers on it, so connect
///          gets ECONNREFUSED) and fails with `already_running` if somebody does. SO_SNDBUF is
///          raised to at least `kMinSendBuffer`, close-on-exec is set, the file is mode 0600.
///          The listener is non-blocking by default, so accept returns `would_block` in an
///          event loop.
/// @param backlog length of the accept queue
[[nodiscard]] std::expected<Fd, TransportError>
listen (const std::string& path, int backlog = 16, bool non_blocking = true);

/// @brief accepts one connection.
/// @param non_blocking makes the accepted socket non-blocking
/// @return the connection, or `would_block` when none is pending on a non-blocking listener
[[nodiscard]] std::expected<Fd, TransportError>
accept (const Fd& listener, bool non_blocking = true);

/// @brief connects to a server socket; the socket is blocking, with the send buffer raised.
[[nodiscard]] std::expected<Fd, TransportError> connect (const std::string& path);

/// @brief raises SO_SNDBUF to at least `kMinSendBuffer` so a full message fits (listen, accept
///        and connect already do it)
[[nodiscard]] std::expected<void, TransportError> raise_send_buffer (int fd);

/// @brief switches O_NONBLOCK of a descriptor
[[nodiscard]] std::expected<void, TransportError> set_non_blocking (int fd, bool non_blocking);

/// @brief sends one message as one packet.
/// @return `too_large` when over `kMaxMessage` (nothing is sent), `would_block` on a full
///         non-blocking socket, `closed` when the peer is gone
[[nodiscard]] std::expected<void, TransportError>
send_message (int fd, std::span<const std::byte> message);

/// @brief receives one packet into `buffer`, which is resized to the packet.
/// @return `too_large` when the packet was longer than `kMaxMessage` (it is consumed, the next
///         one is intact), `closed` on end of stream or an empty packet, `would_block` on a
///         non-blocking socket without data
[[nodiscard]] std::expected<void, TransportError>
recv_message (int fd, std::vector<std::byte>& buffer);

}  // namespace bitvm::proto
