#include "net_util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>

bool parse_host(const std::string& text, in_addr* out)
{
    if (out == nullptr || text.empty())
    {
        return false;
    }
    if (inet_pton(AF_INET, text.c_str(), out) == 1)
    {
        return true;
    }
    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(text.c_str(), nullptr, &hints, &res) != 0 || res == nullptr)
    {
        return false;
    }
    const auto* sin = reinterpret_cast<const sockaddr_in*>(res->ai_addr);
    *out = sin->sin_addr;
    freeaddrinfo(res);
    return out->s_addr != 0;
}

bool parse_host_port(const std::string& text, sockaddr_in* out)
{
    if (out == nullptr)
    {
        return false;
    }
    const auto colon = text.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size())
    {
        return false;
    }
    const std::string host = text.substr(0, colon);
    const std::string port_s = text.substr(colon + 1);
    char* end = nullptr;
    const long port = std::strtol(port_s.c_str(), &end, 10);
    if (end == port_s.c_str() || *end != '\0' || port <= 0 || port > 65535)
    {
        return false;
    }
    in_addr addr = {};
    if (!parse_host(host, &addr))
    {
        return false;
    }
    *out = {};
    out->sin_family = AF_INET;
    out->sin_addr = addr;
    out->sin_port = htons(static_cast<uint16_t>(port));
    return true;
}

bool parse_bus(const std::string& text, uint8_t* bus)
{
    if (bus == nullptr || text.empty())
    {
        return false;
    }
    const char* p = text.c_str();
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
    {
        p += 2;
    }
    const size_t n = strlen(p);
    if (n < 1 || n > 2)
    {
        return false;
    }
    char* end = nullptr;
    const long v = std::strtol(p, &end, 16);
    if (end != p + n || v < 0 || v > 255)
    {
        return false;
    }
    *bus = static_cast<uint8_t>(v);
    return true;
}

bool parse_domain(const std::string& text, uint16_t* domain)
{
    if (domain == nullptr || text.empty())
    {
        return false;
    }
    const char* p = text.c_str();
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
    {
        p += 2;
    }
    char* end = nullptr;
    const unsigned long v = std::strtoul(p, &end, 16);
    if (end == p || *end != '\0' || v < 1 || v > 65535)
    {
        return false;
    }
    *domain = static_cast<uint16_t>(v);
    return true;
}

std::string bus_to_string(uint8_t bus)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "%x", bus);
    return buf;
}

std::string domain_to_string(uint16_t domain)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "%x", domain);
    return buf;
}

std::string sockaddr_to_string(const sockaddr_in& addr)
{
    char host[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &addr.sin_addr, host, sizeof(host));
    char buf[32];
    snprintf(buf, sizeof(buf), "%s:%u", host,
             static_cast<unsigned>(ntohs(addr.sin_port)));
    return buf;
}

std::string ipv4_to_string(in_addr addr)
{
    char host[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &addr, host, sizeof(host));
    return host;
}

bool set_nonblock(int fd)
{
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
    {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool set_blocking(int fd)
{
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
    {
        return false;
    }
    return fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) == 0;
}

void close_socket(bfc::socket* sock)
{
    if (sock == nullptr)
    {
        return;
    }
    bfc::socket discarded(std::move(*sock));
}

static bfc::socket make_socket(int fd)
{
    bfc::socket sock(fd);
    if (sock.fd() < 0)
    {
        return sock;
    }
    const int one = 1;
    sock.set_sock_opt(SOL_SOCKET, SO_REUSEADDR, one);
    if (!set_nonblock(sock.fd()))
    {
        close_socket(&sock);
    }
    return sock;
}

bfc::socket make_udp4()
{
    return make_socket(bfc::create_udp4());
}

bfc::socket make_tcp4()
{
    return make_socket(bfc::create_tcp4());
}

bool bind_udp_any(int fd, uint16_t* port)
{
    if (!bind_udp_port(fd, 0))
    {
        return false;
    }
    if (port != nullptr)
    {
        sockaddr_in addr = {};
        socklen_t len = sizeof(addr);
        if (getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) < 0)
        {
            return false;
        }
        *port = ntohs(addr.sin_port);
    }
    return true;
}

bool bind_udp_port(int fd, uint16_t port)
{
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    return ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) >= 0;
}

bool udp_send_to(int fd, const sockaddr_in& dest, const uint8_t* data,
                 size_t len)
{
    const ssize_t n =
        sendto(fd, data, len, 0, reinterpret_cast<const sockaddr*>(&dest),
               sizeof(dest));
    return n == static_cast<ssize_t>(len);
}

ssize_t udp_recv_from(int fd, uint8_t* data, size_t max, sockaddr_in* from)
{
    sockaddr_in addr = {};
    socklen_t len = sizeof(addr);
    const ssize_t n =
        recvfrom(fd, data, max, 0, reinterpret_cast<sockaddr*>(&addr), &len);
    if (n >= 0 && from != nullptr)
    {
        *from = addr;
    }
    return n;
}
