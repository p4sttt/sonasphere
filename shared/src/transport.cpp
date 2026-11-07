#include "bitvm/proto/transport.hpp"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace bitvm::proto
{
namespace
{

std::unexpected<TransportError>
system_error (std::string detail, int err = errno) {
    return std::unexpected (TransportError{ TransportErrc::system, err, std::move (detail) });
}

std::unexpected<TransportError>
failure (TransportErrc kind, std::string detail) {
    return std::unexpected (TransportError{ kind, 0, std::move (detail) });
}

std::expected<sockaddr_un, TransportError>
make_address (const std::string& path) {
    sockaddr_un addr{};
    if (path.empty() || path.size() >= sizeof (addr.sun_path)) {
        return failure (TransportErrc::path_too_long, "socket path does not fit sockaddr_un");
    }
    addr.sun_family = AF_UNIX;
    std::memcpy (addr.sun_path, path.c_str(), path.size() + 1);
    return addr;
}

std::expected<Fd, TransportError>
make_socket (bool non_blocking = false) {
    const int flags = SOCK_SEQPACKET | SOCK_CLOEXEC | (non_blocking ? SOCK_NONBLOCK : 0);
    Fd        fd (::socket (AF_UNIX, flags, 0));
    if (!fd) {
        return system_error ("socket");
    }
    return fd;
}

// parent directory of a path, "." if there is none
std::string
dirname_of (const std::string& path) {
    const auto slash = path.rfind ('/');
    if (slash == std::string::npos) {
        return ".";
    }
    return slash == 0 ? "/" : path.substr (0, slash);
}

// a socket file with nobody behind it is removed; one that answers means a running server
std::expected<void, TransportError>
clear_stale_socket (const std::string& path) {
    struct stat st{};
    if (::lstat (path.c_str(), &st) != 0) {
        if (errno == ENOENT) {
            return {};
        }
        return system_error ("lstat " + path);
    }
    if (!S_ISSOCK (st.st_mode)) {
        return failure (TransportErrc::not_a_socket, path + " exists and is not a socket");
    }
    auto addr = make_address (path);
    if (!addr) {
        return std::unexpected (addr.error());
    }
    auto probe = make_socket (true);
    if (!probe) {
        return std::unexpected (probe.error());
    }
    while (::connect (probe->get(), reinterpret_cast<const sockaddr*> (&*addr), sizeof (*addr)) !=
           0) {
        if (errno == EINTR) {
            continue;
        }
        if (errno == ECONNREFUSED) {
            if (::unlink (path.c_str()) != 0 && errno != ENOENT) {
                return system_error ("unlink " + path);
            }
            return {};
        }
        if (errno == EINPROGRESS || errno == EALREADY || errno == EAGAIN || errno == EISCONN) {
            return failure (TransportErrc::already_running, "a server already listens on " + path);
        }
        return system_error ("connect " + path);
    }
    return failure (TransportErrc::already_running, "a server already listens on " + path);
}

}  // namespace

Fd&
Fd::operator= (Fd&& other) noexcept {
    if (this != &other) {
        reset (other.release());
    }
    return *this;
}

Fd::~Fd () {
    reset();
}

void
Fd::reset (int fd) noexcept {
    if (fd_ >= 0) {
        ::close (fd_);
    }
    fd_ = fd;
}

std::expected<std::string, TransportError>
socket_path (std::string_view server_name, std::optional<std::string_view> runtime_dir, uid_t uid) {
    if (!server_name.empty() && !is_valid_name (server_name)) {
        return failure (TransportErrc::bad_name, "invalid server name");
    }
    const std::string file =
        server_name.empty() ? "bitvm.sock" : "bitvm-" + std::string (server_name) + ".sock";
    std::string dir;
    if (runtime_dir && !runtime_dir->empty()) {
        dir = std::string (*runtime_dir) + "/sonasphere";
    } else {
        dir = "/tmp/sonasphere-" + std::to_string (uid);
    }
    return dir + "/" + file;
}

std::expected<std::string, TransportError>
socket_path (std::string_view server_name) {
    const char* xdg = std::getenv ("XDG_RUNTIME_DIR");
    return socket_path (
        server_name,
        xdg == nullptr ? std::nullopt : std::optional<std::string_view> (xdg),
        ::getuid()
    );
}

std::expected<void, TransportError>
prepare_socket_dir (const std::string& dir) {
    if (::mkdir (dir.c_str(), 0700) != 0 && errno != EEXIST) {
        return system_error ("mkdir " + dir);
    }
    struct stat st{};
    if (::lstat (dir.c_str(), &st) != 0) {
        return system_error ("lstat " + dir);
    }
    if (!S_ISDIR (st.st_mode)) {
        return failure (TransportErrc::bad_directory, dir + " is not a directory");
    }
    if (st.st_uid != ::geteuid()) {
        return failure (TransportErrc::bad_directory, dir + " belongs to another user");
    }
    if ((st.st_mode & 077) != 0) {
        return failure (TransportErrc::bad_directory, dir + " is accessible to others");
    }
    return {};
}

std::expected<void, TransportError>
raise_send_buffer (int fd) {
    int       current = 0;
    socklen_t len     = sizeof (current);
    if (::getsockopt (fd, SOL_SOCKET, SO_SNDBUF, &current, &len) != 0) {
        return system_error ("getsockopt SO_SNDBUF");
    }
    // the kernel reports twice the requested value
    if (static_cast<std::size_t> (current) / 2 >= kMinSendBuffer) {
        return {};
    }
    const int wanted = static_cast<int> (kMinSendBuffer);
    if (::setsockopt (fd, SOL_SOCKET, SO_SNDBUF, &wanted, sizeof (wanted)) != 0) {
        return system_error ("setsockopt SO_SNDBUF");
    }
    return {};
}

std::expected<void, TransportError>
set_non_blocking (int fd, bool non_blocking) {
    const int flags = ::fcntl (fd, F_GETFL, 0);
    if (flags < 0) {
        return system_error ("fcntl F_GETFL");
    }
    const int wanted = non_blocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    if (wanted != flags && ::fcntl (fd, F_SETFL, wanted) != 0) {
        return system_error ("fcntl F_SETFL");
    }
    return {};
}

std::expected<Fd, TransportError>
listen (const std::string& path, int backlog, bool non_blocking) {
    auto addr = make_address (path);
    if (!addr) {
        return std::unexpected (addr.error());
    }
    if (auto ok = prepare_socket_dir (dirname_of (path)); !ok) {
        return std::unexpected (ok.error());
    }
    if (auto ok = clear_stale_socket (path); !ok) {
        return std::unexpected (ok.error());
    }
    auto fd = make_socket (non_blocking);
    if (!fd) {
        return fd;
    }
    if (::bind (fd->get(), reinterpret_cast<const sockaddr*> (&*addr), sizeof (*addr)) != 0) {
        return system_error ("bind " + path);
    }
    if (::chmod (path.c_str(), 0600) != 0) {
        return system_error ("chmod " + path);
    }
    if (::listen (fd->get(), backlog) != 0) {
        return system_error ("listen " + path);
    }
    if (auto ok = raise_send_buffer (fd->get()); !ok) {
        return std::unexpected (ok.error());
    }
    return fd;
}

std::expected<Fd, TransportError>
accept (const Fd& listener, bool non_blocking) {
    int flags = SOCK_CLOEXEC;
    if (non_blocking) {
        flags |= SOCK_NONBLOCK;
    }
    while (true) {
        Fd fd (::accept4 (listener.get(), nullptr, nullptr, flags));
        if (fd) {
            if (auto ok = raise_send_buffer (fd.get()); !ok) {
                return std::unexpected (ok.error());
            }
            return fd;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return std::unexpected (TransportError{ TransportErrc::would_block, errno, "accept" });
        }
        return system_error ("accept");
    }
}

std::expected<Fd, TransportError>
connect (const std::string& path) {
    auto addr = make_address (path);
    if (!addr) {
        return std::unexpected (addr.error());
    }
    auto fd = make_socket();
    if (!fd) {
        return fd;
    }
    while (::connect (fd->get(), reinterpret_cast<const sockaddr*> (&*addr), sizeof (*addr)) != 0) {
        if (errno != EINTR) {
            return system_error ("connect " + path);
        }
    }
    if (auto ok = raise_send_buffer (fd->get()); !ok) {
        return std::unexpected (ok.error());
    }
    return fd;
}

std::expected<void, TransportError>
send_message (int fd, std::span<const std::byte> message) {
    if (message.size() > kMaxMessage) {
        return failure (TransportErrc::too_large, "message over 65536 bytes");
    }
    while (true) {
        const auto n = ::send (fd, message.data(), message.size(), MSG_NOSIGNAL);
        if (n >= 0) {
            return {};
        }
        switch (errno) {
            case EINTR:
                continue;
            case EAGAIN:
#if EAGAIN != EWOULDBLOCK
            case EWOULDBLOCK:
#endif
                return std::unexpected (
                    TransportError{ TransportErrc::would_block, errno, "send" }
                );
            case EPIPE:
            case ECONNRESET:
                return std::unexpected (TransportError{ TransportErrc::closed, errno, "send" });
            case EMSGSIZE:
                return std::unexpected (TransportError{ TransportErrc::too_large, errno, "send" });
            default:
                return system_error ("send");
        }
    }
}

std::expected<void, TransportError>
recv_message (int fd, std::vector<std::byte>& buffer) {
    buffer.resize (kMaxMessage);
    while (true) {
        // MSG_TRUNC makes the kernel report the real packet length even when it was cut
        const auto n = ::recv (fd, buffer.data(), buffer.size(), MSG_TRUNC);
        if (n > 0) {
            if (static_cast<std::size_t> (n) > kMaxMessage) {
                buffer.clear();
                return failure (TransportErrc::too_large, "incoming packet over 65536 bytes");
            }
            buffer.resize (static_cast<std::size_t> (n));
            return {};
        }
        if (n == 0) {
            buffer.clear();
            return failure (TransportErrc::closed, "peer closed the connection");
        }
        switch (errno) {
            case EINTR:
                continue;
            case EAGAIN:
#if EAGAIN != EWOULDBLOCK
            case EWOULDBLOCK:
#endif
                buffer.clear();
                return std::unexpected (
                    TransportError{ TransportErrc::would_block, errno, "recv" }
                );
            case ECONNRESET:
                buffer.clear();
                return std::unexpected (TransportError{ TransportErrc::closed, errno, "recv" });
            default:
                buffer.clear();
                return system_error ("recv");
        }
    }
}

}  // namespace bitvm::proto
