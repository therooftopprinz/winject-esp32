#ifndef WINJECT_MANAGER_CONSOLE_CLIENT_H_
#define WINJECT_MANAGER_CONSOLE_CLIENT_H_

#include "config.h"

#include <netinet/in.h>
#include <stdint.h>
#include <string>
#include <vector>

#include "net_util.h"

class console_client
{
public:
    ~console_client();

    bool start_connect(const config& cfg, std::string* error);
    bool finish_connect(std::string* error);
    void close();
    int fd() const
    {
        return sock.fd();
    }
    in_addr local_ip() const
    {
        return local_ip_;
    }
    bool apply_radio(const config& cfg, std::string* error);
    bool apply_upstream(const config& cfg, uint16_t inject_port,
                        uint16_t forward_port, in_addr local_ip,
                        std::string* error);
    bool apply_ci(in_addr local_ip, uint16_t ci_port, std::string* error);
    bool program(const config& cfg, const std::vector<uint16_t>& inject_ports,
                 const std::vector<uint16_t>& forward_ports, uint16_t ci_port,
                 in_addr* local_ip, std::string* error);
    bool set_modulation(const std::string& name, std::string* error);
    // Second console socket for tx_grant (does not stall on ping traffic).
    bool open_grant_channel(std::string* error);
    bool connect_grant_peer(const config& cfg, std::string* error);
    void close_grant_channel();
    int grant_fd() const
    {
        return grant_sock.fd();
    }
    bool send_tx_grant_request(std::string* error);
    bool try_consume_tx_grant(uint8_t* frames);
    bool request_tx_grant(uint8_t* frames, std::string* error);

    // Nonblocking keepalive helpers (socket must already be connected).
    bool send_ping(std::string* error);
    void append_recv(const char* data, size_t n);
    bool pop_line(std::string* line);
    void clear_pending();
    // True if send_cmd consumed a keepalive pong while waiting for a reply.
    bool take_pong();

private:
    bool send_cmd(const std::string& cmd, std::string* error);
    bool recv_datagram(std::string* payload, std::string* error);
    bool query_status(std::vector<std::string>* lines, std::string* error);
    bool release_inject_port(uint16_t port, std::string* error);

    bfc::socket sock;
    bfc::socket grant_sock;
    in_addr local_ip_{};
    std::string pending;
    bool pong_seen_ = false;
};

#endif  // WINJECT_MANAGER_CONSOLE_CLIENT_H_
