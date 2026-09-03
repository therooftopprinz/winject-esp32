#ifndef WINJECT_MANAGER_CONSOLE_CLIENT_H_
#define WINJECT_MANAGER_CONSOLE_CLIENT_H_

#include "config.h"
#include "net_util.h"

#include <netinet/in.h>
#include <stdint.h>

#include <string>
#include <vector>

class ConsoleClient
{
public:
    ~ConsoleClient();

    bool start_connect(const Config& cfg, std::string* error);
    bool finish_connect(std::string* error);
    void close();
    int fd() const
    {
        return sock_.fd();
    }
    in_addr local_ip() const
    {
        return local_ip_;
    }
    bool apply_radio(const Config& cfg, std::string* error);
    bool apply_upstream(const Config& cfg, const UpstreamConfig& up,
                        uint16_t inject_port, uint16_t forward_port,
                        in_addr local_ip, std::string* error);
    bool program(const Config& cfg, const std::vector<uint16_t>& inject_ports,
                 const std::vector<uint16_t>& forward_ports, in_addr* local_ip,
                 std::string* error);

private:
    bool send_cmd(const std::string& cmd, std::string* error);
    bool read_line(std::string* line, std::string* error);
    bool query_status(std::vector<std::string>* lines, std::string* error);
    bool release_inject_port(uint16_t port, const std::string& keep_airport,
                             std::string* error);

    bfc::socket sock_;
    in_addr local_ip_{};
    std::string pending_;
};

#endif  // WINJECT_MANAGER_CONSOLE_CLIENT_H_
