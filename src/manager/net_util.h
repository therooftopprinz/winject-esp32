#ifndef WINJECT_MANAGER_NET_UTIL_H_
#define WINJECT_MANAGER_NET_UTIL_H_

#include <bfc/socket.hpp>

#include <netinet/in.h>
#include <stdint.h>

#include <string>

constexpr size_t k_wifi_payload_max = 1476;

bool parse_host_port(const std::string& text, sockaddr_in* out);
bool parse_host(const std::string& text, in_addr* out);
bool parse_airport(const std::string& text, uint8_t mac[6]);
bool airport_is_zero(const uint8_t mac[6]);
std::string airport_to_string(const uint8_t mac[6]);
std::string sockaddr_to_string(const sockaddr_in& addr);
std::string ipv4_to_string(in_addr addr);
bool set_nonblock(int fd);
bool set_blocking(int fd);
void close_socket(bfc::socket* sock);
bfc::socket make_udp4();
bfc::socket make_tcp4();
bool bind_udp_port(int fd, uint16_t port);
bool bind_udp_any(int fd, uint16_t* port);
bool udp_send_to(int fd, const sockaddr_in& dest, const uint8_t* data,
                 size_t len);
ssize_t udp_recv_from(int fd, uint8_t* data, size_t max, sockaddr_in* from);

#endif  // WINJECT_MANAGER_NET_UTIL_H_
