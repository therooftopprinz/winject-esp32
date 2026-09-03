#include "console_client.h"

#include "log.h"
#include "net_util.h"

#include <chrono>
#include <cstdlib>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <vector>

ConsoleClient::~ConsoleClient()
{
    close();
}

void ConsoleClient::close()
{
    close_socket(&sock_);
    pending_.clear();
}

bool ConsoleClient::start_connect(const Config& cfg, std::string* error)
{
    close();
    in_addr ip = {};
    if (!parse_host(cfg.device, &ip))
    {
        *error = "cannot resolve " + cfg.device;
        return false;
    }
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr = ip;
    addr.sin_port = htons(cfg.console_port);
    sock_ = make_tcp4();
    if (sock_.fd() < 0)
    {
        *error = strerror(errno);
        return false;
    }
    const int cr = sock_.connect(addr);
    if (cr < 0 && errno != EINPROGRESS)
    {
        *error = "console " + cfg.device + ":" +
                 std::to_string(cfg.console_port) + ": " + strerror(errno);
        close();
        return false;
    }
    return true;
}

bool ConsoleClient::finish_connect(std::string* error)
{
    if (sock_.fd() < 0)
    {
        *error = "no socket";
        return false;
    }
    int soerr = 0;
    socklen_t slen = sizeof(soerr);
    getsockopt(sock_.fd(), SOL_SOCKET, SO_ERROR, &soerr, &slen);
    if (soerr != 0)
    {
        *error = std::string("console: ") + strerror(soerr);
        return false;
    }
    const int flags = fcntl(sock_.fd(), F_GETFL, 0);
    if (flags >= 0)
    {
        fcntl(sock_.fd(), F_SETFL, flags & ~O_NONBLOCK);
    }
    sockaddr_in local = {};
    socklen_t len = sizeof(local);
    if (getsockname(sock_.fd(), reinterpret_cast<sockaddr*>(&local), &len) == 0)
    {
        local_ip_ = local.sin_addr;
    }
    return true;
}

bool ConsoleClient::read_line(std::string* line, std::string* error)
{
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::seconds(3);
    while (true)
    {
        const auto nl = pending_.find('\n');
        if (nl != std::string::npos)
        {
            *line = pending_.substr(0, nl);
            if (!line->empty() && line->back() == '\r')
            {
                line->pop_back();
            }
            pending_.erase(0, nl + 1);
            return true;
        }
        const auto now = clock::now();
        if (now >= deadline)
        {
            *error = "console read timeout";
            return false;
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - now)
                              .count();
        pollfd pfd = {};
        pfd.fd = sock_.fd();
        pfd.events = POLLIN;
        const int pr = poll(&pfd, 1, static_cast<int>(left));
        if (pr == 0)
        {
            *error = "console read timeout";
            return false;
        }
        if (pr < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            *error = strerror(errno);
            return false;
        }
        char buf[256];
        const ssize_t n = recv(sock_.fd(), buf, sizeof(buf), 0);
        if (n <= 0)
        {
            *error = n == 0 ? "console closed" : strerror(errno);
            return false;
        }
        pending_.append(buf, static_cast<size_t>(n));
    }
}

bool ConsoleClient::send_cmd(const std::string& cmd, std::string* error)
{
    std::string wire = cmd;
    if (wire.empty() || wire.back() != '\n')
    {
        wire.push_back('\n');
    }
    size_t off = 0;
    while (off < wire.size())
    {
        const ssize_t n = send(sock_.fd(), wire.data() + off, wire.size() - off, 0);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            *error = strerror(errno);
            return false;
        }
        off += static_cast<size_t>(n);
    }

    while (true)
    {
        std::string line;
        if (!read_line(&line, error))
        {
            return false;
        }
        if (line == "ok")
        {
            return true;
        }
        if (line.rfind("error", 0) == 0)
        {
            *error = cmd + " -> " + line;
            return false;
        }
        // Banner / unsolicited lines (e.g. "type help") before the reply.
        continue;
    }
}

static bool parse_upstream_rx(const std::string& line, std::string* airport,
                              uint16_t* rx_port)
{
    static const char kPrefix[] = "upstream_";
    if (line.rfind(kPrefix, 0) != 0 || line.rfind("upstream unset", 0) == 0)
    {
        return false;
    }
    const auto sp = line.find(' ', sizeof(kPrefix) - 1);
    if (sp == std::string::npos)
    {
        return false;
    }
    *airport = line.substr(sizeof(kPrefix) - 1, sp - (sizeof(kPrefix) - 1));
    const auto rx = line.find(" rx=");
    if (rx == std::string::npos)
    {
        return false;
    }
    const char* p = line.c_str() + rx + 4;
    if (*p == '-' || *p == '\0')
    {
        *rx_port = 0;
        return true;
    }
    char* end = nullptr;
    const unsigned long v = strtoul(p, &end, 10);
    if (end == p || v > 65535)
    {
        return false;
    }
    *rx_port = static_cast<uint16_t>(v);
    return true;
}

bool ConsoleClient::query_status(std::vector<std::string>* lines,
                                 std::string* error)
{
    lines->clear();
    const char* wire = "status\n";
    if (send(sock_.fd(), wire, 7, 0) < 0)
    {
        *error = strerror(errno);
        return false;
    }

    using clock = std::chrono::steady_clock;
    const auto hard = clock::now() + std::chrono::seconds(2);
    auto last_data = clock::now();
    bool got = false;

    auto drain = [&]()
    {
        while (true)
        {
            const auto nl = pending_.find('\n');
            if (nl == std::string::npos)
            {
                return;
            }
            std::string line = pending_.substr(0, nl);
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            pending_.erase(0, nl + 1);
            if (!line.empty())
            {
                lines->push_back(std::move(line));
                got = true;
                last_data = clock::now();
            }
        }
    };

    while (clock::now() < hard)
    {
        drain();
        const auto now = clock::now();
        const int idle_ms = 250;
        if (got)
        {
            const auto idle =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - last_data)
                    .count();
            if (idle >= idle_ms)
            {
                return true;
            }
        }
        const auto left_hard =
            std::chrono::duration_cast<std::chrono::milliseconds>(hard - now)
                .count();
        int wait = got ? idle_ms : static_cast<int>(left_hard);
        if (wait < 1)
        {
            wait = 1;
        }
        pollfd pfd = {};
        pfd.fd = sock_.fd();
        pfd.events = POLLIN;
        const int pr = poll(&pfd, 1, wait);
        if (pr == 0)
        {
            continue;
        }
        if (pr < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            *error = strerror(errno);
            return false;
        }
        char buf[512];
        const ssize_t n = recv(sock_.fd(), buf, sizeof(buf), 0);
        if (n <= 0)
        {
            *error = n == 0 ? "console closed" : strerror(errno);
            return false;
        }
        pending_.append(buf, static_cast<size_t>(n));
    }
    drain();
    if (got)
    {
        return true;
    }
    *error = "status timeout";
    return false;
}

bool ConsoleClient::release_inject_port(uint16_t port,
                                        const std::string& keep_airport,
                                        std::string* error)
{
    std::vector<std::string> lines;
    std::string status_err;
    if (!query_status(&lines, &status_err))
    {
        LOG_INF("status unavailable (%s); trying set_upstream_rx anyway",
                status_err.c_str());
        return true;
    }
    for (const auto& line : lines)
    {
        std::string airport;
        uint16_t rx_port = 0;
        if (!parse_upstream_rx(line, &airport, &rx_port) || rx_port != port)
        {
            continue;
        }
        if (airport == keep_airport)
        {
            continue;
        }
        LOG_INF("unset stale rx %s (held inject %u)", airport.c_str(), port);
        if (!send_cmd("unset_upstream_rx " + airport, error))
        {
            return false;
        }
    }
    return true;
}

bool ConsoleClient::program(const Config& cfg,
                            const std::vector<uint16_t>& inject_ports,
                            const std::vector<uint16_t>& forward_ports,
                            in_addr* local_ip_out, std::string* error)
{
    if (local_ip_out == nullptr)
    {
        *error = "local_ip_out is null";
        return false;
    }
    close();
    in_addr ip = {};
    if (!parse_host(cfg.device, &ip))
    {
        *error = "cannot resolve " + cfg.device;
        return false;
    }
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr = ip;
    addr.sin_port = htons(cfg.console_port);
    sock_ = make_tcp4();
    if (sock_.fd() < 0)
    {
        *error = strerror(errno);
        return false;
    }
    const int flags = fcntl(sock_.fd(), F_GETFL, 0);
    if (flags >= 0)
    {
        fcntl(sock_.fd(), F_SETFL, flags & ~O_NONBLOCK);
    }
    if (sock_.connect(addr) < 0)
    {
        *error = "console " + cfg.device + ":" +
                 std::to_string(cfg.console_port) + ": " + strerror(errno);
        close();
        return false;
    }
    sockaddr_in local = {};
    socklen_t len = sizeof(local);
    if (getsockname(sock_.fd(), reinterpret_cast<sockaddr*>(&local), &len) == 0)
    {
        local_ip_ = local.sin_addr;
    }
    *local_ip_out = local_ip_;
    sock_.set_sock_opt(IPPROTO_TCP, TCP_NODELAY, 1);

    struct timeval tv = {};
    tv.tv_sec = 0;
    tv.tv_usec = 800000;
    setsockopt(sock_.fd(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char drain[4096];
    recv(sock_.fd(), drain, sizeof(drain), 0);

    auto run_cmd = [&](const std::string& cmd) -> bool
    {
        std::string wire = cmd;
        wire.push_back('\n');
        if (send(sock_.fd(), wire.data(), wire.size(), 0) < 0)
        {
            *error = strerror(errno);
            return false;
        }
        pending_.clear();
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
        std::string buf;
        while (std::chrono::steady_clock::now() < deadline)
        {
            std::string line;
            if (!read_line(&line, error))
            {
                return false;
            }
            buf += line + "\n";
            if (line == "ok")
            {
                return true;
            }
            if (line.rfind("error", 0) == 0)
            {
                *error = cmd + " -> " + line;
                return false;
            }
        }
        *error = "console read timeout for " + cmd;
        return false;
    };

    if (!run_cmd(std::string("set_mode ") + cfg.radio_mode_name()) ||
        !run_cmd("set_channel " + std::to_string(cfg.channel)) ||
        !run_cmd("set_modulation " + cfg.modulation) ||
        !run_cmd("set_tx_power " + std::to_string(cfg.power_dbm)))
    {
        close();
        return false;
    }
    LOG_INF("radio programmed mode=%s ch=%u mod=%s pwr=%d",
            cfg.radio_mode_name(), cfg.channel, cfg.modulation.c_str(),
            cfg.power_dbm);

    for (size_t i = 0; i < cfg.upstreams.size(); i++)
    {
        if (i >= inject_ports.size() || i >= forward_ports.size())
        {
            *error = "inject/forward port list too short";
            close();
            return false;
        }
        const std::string airport = airport_to_string(cfg.upstreams[i].airport);
        if (!release_inject_port(inject_ports[i], airport, error))
        {
            close();
            return false;
        }
        std::string sur = "set_upstream_rx ";
        std::string sut = "set_upstream_tx ";
        if (cfg.radio_mode == RadioMode::Standalone)
        {
            sur += airport + " ";
            sut += airport + " ";
        }
        sur += std::to_string(inject_ports[i]);
        sut += ipv4_to_string(local_ip_) + " " + std::to_string(forward_ports[i]);
        if (!run_cmd(sur) || !run_cmd(sut))
        {
            close();
            return false;
        }
        LOG_INF("upstream-%zu airport=%s inject=%u forward=%s:%u", i,
                airport.c_str(), inject_ports[i],
                ipv4_to_string(local_ip_).c_str(), forward_ports[i]);
    }
    close();
    return true;
}

bool ConsoleClient::apply_radio(const Config& cfg, std::string* error)
{
    if (!send_cmd(std::string("set_mode ") + cfg.radio_mode_name(), error) ||
        !send_cmd("set_channel " + std::to_string(cfg.channel), error) ||
        !send_cmd("set_modulation " + cfg.modulation, error) ||
        !send_cmd("set_tx_power " + std::to_string(cfg.power_dbm), error))
    {
        return false;
    }
    LOG_INF("radio programmed mode=%s ch=%u mod=%s pwr=%d",
            cfg.radio_mode_name(), cfg.channel, cfg.modulation.c_str(),
            cfg.power_dbm);
    return true;
}

bool ConsoleClient::apply_upstream(const Config& cfg, const UpstreamConfig& up,
                                  uint16_t inject_port, uint16_t forward_port,
                                  in_addr local_ip, std::string* error)
{
    const std::string airport = airport_to_string(up.airport);
    if (!release_inject_port(inject_port, airport, error))
    {
        return false;
    }
    std::string sur = "set_upstream_rx ";
    std::string sut = "set_upstream_tx ";
    if (cfg.radio_mode == RadioMode::Standalone)
    {
        sur += airport + " ";
        sut += airport + " ";
    }
    sur += std::to_string(inject_port);
    sut += ipv4_to_string(local_ip) + " " + std::to_string(forward_port);
    if (!send_cmd(sur, error) || !send_cmd(sut, error))
    {
        return false;
    }
    LOG_INF("upstream-%zu airport=%s inject=%u forward=%s:%u", up.index,
            airport.c_str(), inject_port, ipv4_to_string(local_ip).c_str(),
            forward_port);
    return true;
}
