#include "console_client.h"

#include <chrono>
#include <cstdlib>
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <vector>

#include "log.h"
#include "net_util.h"

console_client::~console_client()
{
    close();
}

void console_client::close()
{
    close_socket(&sock);
    pending.clear();
    pong_seen_ = false;
}

bool console_client::start_connect(const config& cfg, std::string* error)
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
    sock = make_udp4();
    if (sock.fd() < 0)
    {
        *error = strerror(errno);
        return false;
    }
    const int cr = sock.connect(addr);
    if (cr < 0 && errno != EINPROGRESS)
    {
        *error = "console " + cfg.device + ":" +
                 std::to_string(cfg.console_port) + ": " + strerror(errno);
        close();
        return false;
    }
    return true;
}

bool console_client::finish_connect(std::string* error)
{
    if (sock.fd() < 0)
    {
        *error = "no socket";
        return false;
    }
    int soerr = 0;
    socklen_t slen = sizeof(soerr);
    getsockopt(sock.fd(), SOL_SOCKET, SO_ERROR, &soerr, &slen);
    if (soerr != 0)
    {
        *error = std::string("console: ") + strerror(soerr);
        return false;
    }
    sockaddr_in local = {};
    socklen_t len = sizeof(local);
    if (getsockname(sock.fd(), reinterpret_cast<sockaddr*>(&local), &len) == 0)
    {
        local_ip_ = local.sin_addr;
    }
    return true;
}

bool console_client::recv_datagram(std::string* payload, std::string* error)
{
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::seconds(3);
    while (true)
    {
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
        pfd.fd = sock.fd();
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
        char buf[16384];
        const ssize_t n = recv(sock.fd(), buf, sizeof(buf), 0);
        if (n < 0)
        {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            {
                continue;
            }
            *error = strerror(errno);
            return false;
        }
        if (n == 0)
        {
            continue;
        }
        payload->assign(buf, static_cast<size_t>(n));
        return true;
    }
}

bool console_client::send_cmd(const std::string& cmd, std::string* error)
{
    std::string wire = cmd;
    if (wire.empty() || wire.back() != '\n')
    {
        wire.push_back('\n');
    }
    using clock = std::chrono::steady_clock;
    constexpr int k_attempts = 3;
    std::string last_err = "console read timeout";

    for (int attempt = 0; attempt < k_attempts; ++attempt)
    {
        // Drop stale datagrams so a late reply to a prior attempt is not
        // mistaken for this command's ok/nok.
        while (true)
        {
            char junk[2048];
            const ssize_t n = recv(sock.fd(), junk, sizeof(junk), MSG_DONTWAIT);
            if (n < 0)
            {
                break;
            }
        }

        const auto write_deadline = clock::now() + std::chrono::seconds(3);
        bool sent = false;
        while (!sent)
        {
            const ssize_t n =
                send(sock.fd(), wire.data(), wire.size(), MSG_NOSIGNAL);
            if (n > 0)
            {
                sent = true;
                break;
            }
            if (n == 0)
            {
                *error = "console send failed";
                return false;
            }
            if (errno == EINTR)
            {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                const auto now = clock::now();
                if (now >= write_deadline)
                {
                    last_err = "console write timeout";
                    break;
                }
                const auto left =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        write_deadline - now)
                        .count();
                pollfd pfd = {};
                pfd.fd = sock.fd();
                pfd.events = POLLOUT;
                const int pr = poll(&pfd, 1, static_cast<int>(left));
                if (pr == 0)
                {
                    last_err = "console write timeout";
                    break;
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
                continue;
            }
            *error = strerror(errno);
            return false;
        }
        if (!sent)
        {
            if (attempt + 1 < k_attempts)
            {
                LOG_WRN("console %s write timeout, retry %d/%d", cmd.c_str(),
                        attempt + 1, k_attempts - 1);
                continue;
            }
            *error = last_err;
            return false;
        }

        while (true)
        {
            std::string dgram;
            std::string recv_err;
            if (!recv_datagram(&dgram, &recv_err))
            {
                last_err = recv_err;
                break;
            }
            std::string line = dgram;
            const auto nl = line.find('\n');
            if (nl != std::string::npos)
            {
                line.resize(nl);
            }
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            if (line == "pong")
            {
                pong_seen_ = true;
                continue;
            }
            if (line == "ok" || line.rfind("ok ", 0) == 0)
            {
                return true;
            }
            if (line.rfind("nok ", 0) == 0)
            {
                // Soft / transient: retry. Hard command errors still fail.
                const bool soft = line.find("timeout") != std::string::npos ||
                                  line.find("busy") != std::string::npos ||
                                  line.find("no mem") != std::string::npos;
                last_err = cmd + " -> " + line;
                if (soft && attempt + 1 < k_attempts)
                {
                    LOG_WRN("console %s, retry %d/%d", last_err.c_str(),
                            attempt + 1, k_attempts - 1);
                    break;
                }
                *error = last_err;
                return false;
            }
        }
        if (attempt + 1 < k_attempts)
        {
            LOG_WRN("console %s: %s, retry %d/%d", cmd.c_str(), last_err.c_str(),
                    attempt + 1, k_attempts - 1);
        }
    }
    *error = last_err;
    return false;
}

bool console_client::set_modulation(const std::string& name, std::string* error)
{
    return send_cmd("set_modulation " + name, error);
}

static bool parse_upstream_tx_port(const std::string& line, uint16_t* inject_port)
{
    static const char k_prefix[] = "upstream_tx ";
    if (line.rfind(k_prefix, 0) != 0 || inject_port == nullptr)
    {
        return false;
    }
    const auto port_pos = line.find("port=");
    if (port_pos == std::string::npos)
    {
        return false;
    }
    char* end = nullptr;
    const unsigned long port = strtoul(line.c_str() + port_pos + 5, &end, 10);
    if (end == line.c_str() + port_pos + 5 || port == 0 || port > 65535)
    {
        return false;
    }
    *inject_port = static_cast<uint16_t>(port);
    return true;
}

static void split_lines(const std::string& dgram,
                        std::vector<std::string>* lines)
{
    lines->clear();
    size_t off = 0;
    while (off < dgram.size())
    {
        const auto nl = dgram.find('\n', off);
        std::string line = nl == std::string::npos
                               ? dgram.substr(off)
                               : dgram.substr(off, nl - off);
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        if (!line.empty())
        {
            lines->push_back(std::move(line));
        }
        if (nl == std::string::npos)
        {
            break;
        }
        off = nl + 1;
    }
}

bool console_client::query_status(std::vector<std::string>* lines,
                                  std::string* error)
{
    lines->clear();
    const char* wire = "status\n";
    while (true)
    {
        const ssize_t n = send(sock.fd(), wire, 7, MSG_NOSIGNAL);
        if (n > 0)
        {
            break;
        }
        if (n < 0 && errno == EINTR)
        {
            continue;
        }
        *error = strerror(errno);
        return false;
    }

    while (true)
    {
        std::string dgram;
        if (!recv_datagram(&dgram, error))
        {
            return false;
        }
        std::vector<std::string> body;
        split_lines(dgram, &body);
        if (body.empty())
        {
            continue;
        }
        if (body[0] == "pong")
        {
            pong_seen_ = true;
            continue;
        }
        if (body[0].rfind("nok ", 0) == 0)
        {
            *error = std::string("status -> ") + body[0];
            return false;
        }
        if (body[0] == "ok" || body[0].rfind("ok ", 0) == 0)
        {
            if (body[0] == "ok")
            {
                body.erase(body.begin());
            }
            else
            {
                body[0].erase(0, 3);
            }
            *lines = std::move(body);
            return true;
        }
    }
}

bool console_client::release_inject_port(uint16_t port, std::string* error)
{
    std::vector<std::string> lines;
    std::string status_err;
    if (!query_status(&lines, &status_err))
    {
        LOG_INF("status unavailable (%s); trying set_upstream_tx anyway",
                status_err.c_str());
        return true;
    }
    for (const auto& line : lines)
    {
        uint16_t inject_port = 0;
        if (!parse_upstream_tx_port(line, &inject_port) || inject_port != port)
        {
            continue;
        }
        LOG_INF("unset stale upstream_tx (held inject %u)", port);
        if (!send_cmd("unset_upstream_tx", error))
        {
            return false;
        }
    }
    return true;
}

bool console_client::program(const config& cfg,
                             const std::vector<uint16_t>& inject_ports,
                             const std::vector<uint16_t>& forward_ports,
                             in_addr* local_ip_out, std::string* error)
{
    if (local_ip_out == nullptr)
    {
        *error = "local_ip_out is null";
        return false;
    }
    if (!start_connect(cfg, error) || !finish_connect(error))
    {
        close();
        return false;
    }
    *local_ip_out = local_ip_;

    // Channel 14 is 802.11b-only: apply DSSS/CCK before switching to 14,
    // and leave 14 before applying OFDM (matches firmware
    // settings::apply_snapshot). Mode/Addr3 prefix is manager-local
    // (mpdu_init_rules); do not send set_mode to the radio.
    if (cfg.channel == 14)
    {
        if (!send_cmd("set_modulation " + cfg.modulation, error) ||
            !send_cmd("set_channel " + std::to_string(cfg.channel), error))
        {
            close();
            return false;
        }
    }
    else if (!send_cmd("set_channel " + std::to_string(cfg.channel), error) ||
             !send_cmd("set_modulation " + cfg.modulation, error))
    {
        close();
        return false;
    }
    if (!send_cmd("set_tx_power " + std::to_string(cfg.power_dbm), error) ||
        !send_cmd("set_domain " + domain_to_string(cfg.domain), error))
    {
        close();
        return false;
    }
    LOG_INF("radio programmed ch=%u mod=%s pwr=%d domain=%s (addr3 mode=%s)",
            cfg.channel, cfg.modulation.c_str(), cfg.power_dbm,
            domain_to_string(cfg.domain).c_str(), cfg.radio_mode_name());

    if (inject_ports.empty() || forward_ports.empty())
    {
        *error = "inject/forward port missing";
        close();
        return false;
    }
    if (!apply_upstream(cfg, inject_ports[0], forward_ports[0], local_ip_,
                        error))
    {
        close();
        return false;
    }
    pending.clear();
    return true;
}

bool console_client::apply_radio(const config& cfg, std::string* error)
{
    // Mode/Addr3 prefix is manager-local (mpdu_init_rules); do not send
    // set_mode to the radio.
    if (cfg.channel == 14)
    {
        if (!send_cmd("set_modulation " + cfg.modulation, error) ||
            !send_cmd("set_channel " + std::to_string(cfg.channel), error))
        {
            return false;
        }
    }
    else if (!send_cmd("set_channel " + std::to_string(cfg.channel), error) ||
             !send_cmd("set_modulation " + cfg.modulation, error))
    {
        return false;
    }
    if (!send_cmd("set_tx_power " + std::to_string(cfg.power_dbm), error) ||
        !send_cmd("set_domain " + domain_to_string(cfg.domain), error))
    {
        return false;
    }
    LOG_INF("radio programmed ch=%u mod=%s pwr=%d domain=%s (addr3 mode=%s)",
            cfg.channel, cfg.modulation.c_str(), cfg.power_dbm,
            domain_to_string(cfg.domain).c_str(), cfg.radio_mode_name());
    return true;
}

bool console_client::apply_upstream(const config& cfg, uint16_t inject_port,
                                    uint16_t forward_port, in_addr local_ip,
                                    std::string* error)
{
    (void)cfg;
    if (!release_inject_port(inject_port, error))
    {
        return false;
    }
    const std::string sut =
        "set_upstream_tx port=" + std::to_string(inject_port);
    if (!send_cmd(sut, error))
    {
        return false;
    }
    const std::string sur = "set_upstream_rx host=" + ipv4_to_string(local_ip) +
                            " port=" + std::to_string(forward_port);
    if (!send_cmd(sur, error))
    {
        return false;
    }
    LOG_INF("radio upstream inject=%u forward=%s:%u", inject_port,
            ipv4_to_string(local_ip).c_str(), forward_port);
    return true;
}

bool console_client::send_ping(std::string* error)
{
    static const char k_wire[] = "ping\n";
    size_t off = 0;
    while (off < sizeof(k_wire) - 1)
    {
        const ssize_t n = send(sock.fd(), k_wire + off,
                               sizeof(k_wire) - 1 - off, MSG_NOSIGNAL);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                *error = "console ping would block";
                return false;
            }
            *error = strerror(errno);
            return false;
        }
        if (n == 0)
        {
            *error = "console closed";
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
}

void console_client::append_recv(const char* data, size_t n)
{
    if (data == nullptr || n == 0)
    {
        return;
    }
    pending.append(data, n);
}

bool console_client::pop_line(std::string* line)
{
    const auto nl = pending.find('\n');
    if (nl == std::string::npos)
    {
        return false;
    }
    *line = pending.substr(0, nl);
    if (!line->empty() && line->back() == '\r')
    {
        line->pop_back();
    }
    pending.erase(0, nl + 1);
    return true;
}

void console_client::clear_pending()
{
    pending.clear();
}

bool console_client::take_pong()
{
    const bool seen = pong_seen_;
    pong_seen_ = false;
    return seen;
}
