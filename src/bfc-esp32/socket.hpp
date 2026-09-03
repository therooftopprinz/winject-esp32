#ifndef BFC_SOCKET_HPP_
#define BFC_SOCKET_HPP_

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "lwip/sockets.h"

namespace bfc
{

inline constexpr uint32_t to_ip(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    uint32_t rv = static_cast<uint32_t>(a) << 24;
    rv |= static_cast<uint32_t>(b) << 16;
    rv |= static_cast<uint32_t>(c) << 8;
    rv |= static_cast<uint32_t>(d);
    return rv;
}

constexpr uint32_t localhost4 = to_ip(127, 0, 0, 1);

inline sockaddr_in ip4_port_to_sockaddr(uint32_t ip, uint16_t port)
{
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(ip);
    addr.sin_port = htons(port);
    return addr;
}

inline int create_udp4()
{
    return ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
}

inline int create_tcp4()
{
    return ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
}

// RAII IPv4 socket (bfc::socket). Move-only; destructor closes.
class socket
{
public:
    socket() = default;

    explicit socket(int fd) : fd_(fd) {}

    ~socket()
    {
        close();
    }

    socket(const socket&) = delete;
    socket& operator=(const socket&) = delete;

    socket(socket&& other) noexcept : fd_(other.fd_)
    {
        other.fd_ = -1;
    }

    socket& operator=(socket&& other) noexcept
    {
        if (this != &other)
        {
            close();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    bool valid() const
    {
        return fd_ >= 0;
    }

    int fd() const
    {
        return fd_;
    }

    void close()
    {
        if (fd_ >= 0)
        {
            ::close(fd_);
            fd_ = -1;
        }
    }

    int bind(const sockaddr* addr, socklen_t size)
    {
        return ::bind(fd_, addr, size);
    }

    template <typename T>
    int bind(const T& addr)
    {
        return bind(reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    }

    ssize_t send(const void* data, size_t size, int flags, const sockaddr* to,
                 socklen_t to_len)
    {
        return ::sendto(fd_, data, size, flags, to, to_len);
    }

    template <typename T>
    auto send(const T& data, int flags, const sockaddr* to,
              socklen_t to_len) -> decltype(data.data(), data.size(), ssize_t())
    {
        return send(data.data(), data.size(), flags, to, to_len);
    }

    ssize_t recv(void* data, size_t size, int flags = 0)
    {
        return ::recv(fd_, data, size, flags);
    }

    template <typename T>
    auto recv(T&& data, int flags = 0) -> decltype(data.data(), data.size(),
                                                   ssize_t())
    {
        return recv(data.data(), data.size(), flags);
    }

    int set_sock_opt(int level, int name, const void* value, socklen_t len)
    {
        return ::setsockopt(fd_, level, name, value, len);
    }

    template <typename T>
    int set_sock_opt(int level, int name, T value)
    {
        return set_sock_opt(level, name, &value, sizeof(value));
    }

    int listen(int backlog = 10)
    {
        return ::listen(fd_, backlog);
    }

    socket accept(sockaddr* addr, socklen_t* addr_len)
    {
        return socket(::accept(fd_, addr, addr_len));
    }

    template <typename T>
    socket accept(T& addr)
    {
        socklen_t size = sizeof(addr);
        return accept(reinterpret_cast<sockaddr*>(&addr), &size);
    }

    int connect(const sockaddr* addr, socklen_t addr_len)
    {
        return ::connect(fd_, addr, addr_len);
    }

    template <typename T>
    int connect(const T& addr)
    {
        return connect(reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    }

    bool set_nonblock()
    {
        const int flags = fcntl(fd_, F_GETFL, 0);
        if (flags < 0)
        {
            return false;
        }
        return fcntl(fd_, F_SETFL, flags | O_NONBLOCK) == 0;
    }

    // bind_ip is network-order (lwIP s_addr). port 0 is ephemeral.
    bool open_udp(uint32_t bind_ip, uint16_t port)
    {
        close();
        fd_ = create_udp4();
        if (fd_ < 0)
        {
            return false;
        }

        set_sock_opt(SOL_SOCKET, SO_REUSEADDR, 1);
        int buf = 64 * 1024;
        set_sock_opt(SOL_SOCKET, SO_RCVBUF, buf);
        set_sock_opt(SOL_SOCKET, SO_SNDBUF, buf);

        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = bind_ip;
        if (bind(addr) < 0 || !set_nonblock())
        {
            close();
            return false;
        }
        return true;
    }

private:
    int fd_ = -1;
};

}  // namespace bfc

#endif  // BFC_SOCKET_HPP_
