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

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    return -1;
}

static bool parse_packed_mac(const std::string& text, uint8_t mac[6])
{
    if (text.size() != 12)
    {
        return false;
    }
    for (int i = 0; i < 6; i++)
    {
        const int hi = hex_nibble(text[static_cast<size_t>(i) * 2]);
        const int lo = hex_nibble(text[static_cast<size_t>(i) * 2 + 1]);
        if (hi < 0 || lo < 0)
        {
            return false;
        }
        mac[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

static bool parse_colon_mac(const std::string& text, uint8_t mac[6])
{
    const char* p = text.c_str();
    for (int i = 0; i < 6; i++)
    {
        char* end = nullptr;
        const long value = std::strtol(p, &end, 16);
        if (end == p || value < 0 || value > 255)
        {
            return false;
        }
        const size_t digits = static_cast<size_t>(end - p);
        if (digits == 0 || digits > 2)
        {
            return false;
        }
        mac[i] = static_cast<uint8_t>(value);
        if (i < 5)
        {
            if (*end != ':')
            {
                return false;
            }
            p = end + 1;
        }
        else if (*end != '\0')
        {
            return false;
        }
    }
    return true;
}

bool parse_airport(const std::string& text, uint8_t mac[6])
{
    if (mac == nullptr || text.empty())
    {
        return false;
    }
    if (text == "0")
    {
        memset(mac, 0, 6);
        return true;
    }
    if (text.find(':') != std::string::npos)
    {
        return parse_colon_mac(text, mac);
    }
    return parse_packed_mac(text, mac);
}

bool airport_is_zero(const uint8_t mac[6])
{
    for (int i = 0; i < 6; i++)
    {
        if (mac[i] != 0)
        {
            return false;
        }
    }
    return true;
}

std::string airport_to_string(const uint8_t mac[6])
{
    char buf[24];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1],
             mac[2], mac[3], mac[4], mac[5]);
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
