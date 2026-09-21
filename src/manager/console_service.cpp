#include "console_service.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#include "log.h"

namespace
{
bool parse_u(const char* text, unsigned long* out)
{
    if (text == nullptr || out == nullptr || *text == '\0')
    {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long v = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0')
    {
        return false;
    }
    *out = v;
    return true;
}

bool parse_fec_type(const char* text, fec_type_e* out)
{
    if (text == nullptr || out == nullptr)
    {
        return false;
    }
    if (strcmp(text, "NONE") == 0 || strcmp(text, "none") == 0)
    {
        *out = fec_type_e::none;
        return true;
    }
    if (strcmp(text, "RS_BLOCK_ERASURE") == 0)
    {
        *out = fec_type_e::rs_block_erasure;
        return true;
    }
    return false;
}

const char* fec_type_name(fec_type_e type)
{
    switch (type)
    {
        case fec_type_e::none:
            return "NONE";
        case fec_type_e::rs_block_erasure:
            return "RS_BLOCK_ERASURE";
    }
    return "NONE";
}

void trim_line(char* line)
{
    if (line == nullptr)
    {
        return;
    }
    size_t n = strlen(line);
    while (n > 0)
    {
        const char c = line[n - 1];
        if (c != '\n' && c != '\r' && c != ' ' && c != '\t')
        {
            break;
        }
        line[--n] = '\0';
    }
}
}  // namespace

console_service::~console_service()
{
    stop();
}

bool console_service::cmd_is(const char* cmd, const char* full,
                             const char* abbrev)
{
    return cmd != nullptr && ((full != nullptr && strcmp(cmd, full) == 0) ||
                              (abbrev != nullptr && strcmp(cmd, abbrev) == 0));
}

bool console_service::start(::reactor& reactor, const sockaddr_in& console_in,
                            const sockaddr_in& console_out, set_fec_fn set_fec,
                            set_budget_fn set_budget, get_fec_fn get_fec,
                            get_budget_fn get_budget, get_ci_fn get_ci,
                            set_modulation_fn set_modulation,
                            get_modulation_fn get_modulation,
                            std::string* error)
{
    auto fail = [&](const char* msg) -> bool
    {
        if (error != nullptr)
        {
            *error = msg;
        }
        return false;
    };
    if (!set_fec || !set_budget || !get_fec || !get_budget || !get_ci ||
        !set_modulation || !get_modulation)
    {
        return fail("invalid manager console args");
    }
    stop();
    this->reactor = &reactor;
    this->set_fec = std::move(set_fec);
    this->set_budget = std::move(set_budget);
    this->get_fec = std::move(get_fec);
    this->get_budget = std::move(get_budget);
    this->get_ci = std::move(get_ci);
    this->set_modulation = std::move(set_modulation);
    this->get_modulation = std::move(get_modulation);
    out_addr = console_out;
    sock = make_udp4();
    if (sock.fd() < 0)
    {
        return fail(strerror(errno));
    }
    if (sock.bind(console_in) < 0 || !set_nonblock(sock.fd()))
    {
        const char* why = strerror(errno);
        stop();
        return fail(why);
    }
    if (!reactor.add_read_rdy(sock.fd(),
                              [this]()
                              {
                                  on_datagram();
                              }))
    {
        stop();
        return fail("epoll add failed");
    }
    LOG_INF("manager console in=%s out=%s",
            sockaddr_to_string(console_in).c_str(),
            sockaddr_to_string(console_out).c_str());
    return true;
}

void console_service::stop()
{
    if (sock.fd() >= 0)
    {
        if (reactor != nullptr)
        {
            reactor->rem_read_rdy(sock.fd());
        }
        close_socket(&sock);
    }
    reactor = nullptr;
    set_fec = nullptr;
    set_budget = nullptr;
    get_fec = nullptr;
    get_budget = nullptr;
    get_ci = nullptr;
    set_modulation = nullptr;
    get_modulation = nullptr;
    out_addr = {};
}

void console_service::reply(const char* text)
{
    if (sock.fd() < 0 || text == nullptr)
    {
        return;
    }
    const size_t n = strlen(text);
    if (n == 0)
    {
        return;
    }
    udp_send_to(sock.fd(), out_addr, reinterpret_cast<const uint8_t*>(text), n);
}

void console_service::reply_ok()
{
    reply("ok\n");
}

void console_service::reply_ok_args(const char* args)
{
    std::string text = "ok";
    if (args != nullptr && args[0] != '\0')
    {
        text += ' ';
        text += args;
    }
    if (text.back() != '\n')
    {
        text += '\n';
    }
    reply(text.c_str());
}

void console_service::reply_nok(const char* msg)
{
    char buf[320];
    snprintf(buf, sizeof(buf), "nok %s\n", msg != nullptr ? msg : "error");
    reply(buf);
}

void console_service::on_datagram()
{
    if (sock.fd() < 0)
    {
        return;
    }
    while (true)
    {
        char buf[k_line_max];
        const ssize_t n =
            udp_recv_from(sock.fd(), reinterpret_cast<uint8_t*>(buf),
                          sizeof(buf) - 1, nullptr);
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                return;
            }
            LOG_WRN("manager console recv: %s", strerror(errno));
            return;
        }
        if (n == 0)
        {
            continue;
        }
        buf[n] = '\0';
        trim_line(buf);
        if (buf[0] == '\0' || buf[0] == '#')
        {
            continue;
        }
        handle_line(buf);
    }
}

void console_service::handle_line(const char* line)
{
    char copy[k_line_max];
    strncpy(copy, line, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = '\0';

    char* save = nullptr;
    char* cmd = strtok_r(copy, " \t", &save);
    if (cmd == nullptr || *cmd == '\0')
    {
        return;
    }

    if (cmd_is(cmd, "help", "?"))
    {
        reply(
            "set_upstream_fec|suf <index> <NONE|RS_BLOCK_ERASURE> <k> <n>\n"
            "get_upstream_fec|guf <index>\n"
            "set_upstream_scheduler_budget|sus <index> <budget>\n"
            "get_upstream_scheduler_budget|gus <index>\n"
            "set_modulation|sd <modulation>\n"
            "get_modulation|gd\n"
            "get_channel_info|gci\n"
            "ping\n"
            "help\n");
        return;
    }
    if (cmd_is(cmd, "ping", "ping"))
    {
        reply("pong\n");
        return;
    }

    if (cmd_is(cmd, "set_upstream_fec", "suf") ||
        strcmp(cmd, "set_upsteram_fec") == 0)
    {
        char* a_index = strtok_r(nullptr, " \t", &save);
        char* a_type = strtok_r(nullptr, " \t", &save);
        char* a_k = strtok_r(nullptr, " \t", &save);
        char* a_n = strtok_r(nullptr, " \t", &save);
        char* extra = strtok_r(nullptr, " \t", &save);
        unsigned long idx = 0;
        unsigned long k = 0;
        unsigned long n = 0;
        fec_type_e type = fec_type_e::none;
        if (extra != nullptr || !parse_u(a_index, &idx) ||
            !parse_fec_type(a_type, &type) || !parse_u(a_k, &k) ||
            !parse_u(a_n, &n))
        {
            reply_nok(
                "usage set_upstream_fec <index> "
                "<NONE|RS_BLOCK_ERASURE> <k> <n>");
            return;
        }
        if (type != fec_type_e::none && (k < 1 || n <= k || n > 255))
        {
            reply_nok("need 1 <= k < n <= 255");
            return;
        }
        std::string err;
        if (!set_fec(static_cast<size_t>(idx), type, static_cast<int>(k),
                     static_cast<int>(n), &err))
        {
            reply_nok(err.empty() ? "failed" : err.c_str());
            return;
        }
        reply_ok();
        return;
    }

    if (cmd_is(cmd, "get_upstream_fec", "guf") ||
        strcmp(cmd, "get_upsteram_fec") == 0)
    {
        char* a_index = strtok_r(nullptr, " \t", &save);
        char* extra = strtok_r(nullptr, " \t", &save);
        unsigned long idx = 0;
        if (extra != nullptr || !parse_u(a_index, &idx))
        {
            reply_nok("usage get_upstream_fec <index>");
            return;
        }
        fec_type_e type = fec_type_e::none;
        int k = 0;
        int n = 0;
        std::string err;
        if (!get_fec(static_cast<size_t>(idx), &type, &k, &n, &err))
        {
            reply_nok(err.empty() ? "failed" : err.c_str());
            return;
        }
        char args[64];
        if (type == fec_type_e::none)
        {
            snprintf(args, sizeof(args), "%s 0 0", fec_type_name(type));
        }
        else
        {
            snprintf(args, sizeof(args), "%s %d %d", fec_type_name(type), k, n);
        }
        reply_ok_args(args);
        return;
    }

    if (cmd_is(cmd, "set_upstream_scheduler_budget", "sus") ||
        strcmp(cmd, "set_upsteram_scheduler_budget") == 0)
    {
        char* a_index = strtok_r(nullptr, " \t", &save);
        char* a_budget = strtok_r(nullptr, " \t", &save);
        char* extra = strtok_r(nullptr, " \t", &save);
        unsigned long idx = 0;
        unsigned long budget = 0;
        if (extra != nullptr || !parse_u(a_index, &idx) ||
            !parse_u(a_budget, &budget) || budget == 0)
        {
            reply_nok("usage set_upstream_scheduler_budget <index> <budget>");
            return;
        }
        std::string err;
        if (!set_budget(static_cast<size_t>(idx), static_cast<size_t>(budget),
                        &err))
        {
            reply_nok(err.empty() ? "failed" : err.c_str());
            return;
        }
        reply_ok();
        return;
    }

    if (cmd_is(cmd, "get_upstream_scheduler_budget", "gus") ||
        strcmp(cmd, "get_upsteram_scheduler_budget") == 0)
    {
        char* a_index = strtok_r(nullptr, " \t", &save);
        char* extra = strtok_r(nullptr, " \t", &save);
        unsigned long idx = 0;
        if (extra != nullptr || !parse_u(a_index, &idx))
        {
            reply_nok("usage get_upstream_scheduler_budget <index>");
            return;
        }
        size_t budget = 0;
        std::string err;
        if (!get_budget(static_cast<size_t>(idx), &budget, &err))
        {
            reply_nok(err.empty() ? "failed" : err.c_str());
            return;
        }
        char args[32];
        snprintf(args, sizeof(args), "%zu", budget);
        reply_ok_args(args);
        return;
    }

    if (cmd_is(cmd, "set_modulation", "sd"))
    {
        char* a_mod = strtok_r(nullptr, " \t", &save);
        char* extra = strtok_r(nullptr, " \t", &save);
        if (extra != nullptr || a_mod == nullptr || a_mod[0] == '\0')
        {
            reply_nok("usage set_modulation <modulation>");
            return;
        }
        std::string err;
        if (!set_modulation(a_mod, &err))
        {
            reply_nok(err.empty() ? "failed" : err.c_str());
            return;
        }
        reply_ok();
        return;
    }

    if (cmd_is(cmd, "get_modulation", "gd"))
    {
        char* extra = strtok_r(nullptr, " \t", &save);
        if (extra != nullptr)
        {
            reply_nok("usage get_modulation");
            return;
        }
        std::string name;
        std::string err;
        if (!get_modulation(&name, &err))
        {
            reply_nok(err.empty() ? "failed" : err.c_str());
            return;
        }
        reply_ok_args(name.c_str());
        return;
    }

    if (cmd_is(cmd, "get_channel_info", "gci"))
    {
        char* extra = strtok_r(nullptr, " \t", &save);
        if (extra != nullptr)
        {
            reply_nok("usage get_channel_info");
            return;
        }
        channel_info_view_s view;
        std::string err;
        if (!get_ci(&view, &err))
        {
            reply_nok(err.empty() ? "failed" : err.c_str());
            return;
        }
        char flow[32];
        char rssi[16];
        char snr[16];
        if (view.flow_valid)
        {
            snprintf(flow, sizeof(flow), "%u/%u", view.tx_queue_size,
                     view.tx_queue_capacity);
        }
        else
        {
            snprintf(flow, sizeof(flow), "-");
        }
        if (view.air_valid)
        {
            snprintf(rssi, sizeof(rssi), "%d", static_cast<int>(view.rssi));
            snprintf(snr, sizeof(snr), "%d", static_cast<int>(view.snr));
        }
        else
        {
            snprintf(rssi, sizeof(rssi), "-");
            snprintf(snr, sizeof(snr), "-");
        }
        std::string args;
        char line[384];
        snprintf(line, sizeof(line),
                 "flow=%s flow_t=%s rssi=%s rssi_t=%s snr=%s", flow,
                 view.flow_t, rssi, view.rssi_t, snr);
        args = line;
        snprintf(line, sizeof(line),
                 "\nstream tx_byte=%llu rx_byte=%llu tx_pkt=%llu rx_pkt=%llu "
                 "rx_pkt_loss=%llu",
                 static_cast<unsigned long long>(view.tx_byte),
                 static_cast<unsigned long long>(view.rx_byte),
                 static_cast<unsigned long long>(view.tx_pkt),
                 static_cast<unsigned long long>(view.rx_pkt),
                 static_cast<unsigned long long>(view.rx_pkt_loss));
        args += line;
        for (const stream_rate_view_s& row : view.streams)
        {
            const char* type = row.type != nullptr ? row.type : "-";
            const char* fec = row.fec[0] != '\0' ? row.fec : "none";
            int n = snprintf(
                line, sizeof(line),
                "\nstream-%zu type=%s fec=%s tx_byte=%llu rx_byte=%llu "
                "tx_pkt=%llu rx_pkt=%llu air_tx_pkt=%llu drop_txq=%llu "
                "rx_pkt_loss=%llu fec_rec=%llu fec_lost=%llu",
                row.index, type, fec,
                static_cast<unsigned long long>(row.tx_byte),
                static_cast<unsigned long long>(row.rx_byte),
                static_cast<unsigned long long>(row.tx_pkt),
                static_cast<unsigned long long>(row.rx_pkt),
                static_cast<unsigned long long>(row.air_tx_pkt),
                static_cast<unsigned long long>(row.drop_txq),
                static_cast<unsigned long long>(row.rx_pkt_loss),
                static_cast<unsigned long long>(row.fec_recovered),
                static_cast<unsigned long long>(row.fec_fail));
            if (row.tcp && n > 0 && static_cast<size_t>(n) < sizeof(line))
            {
                snprintf(line + n, sizeof(line) - static_cast<size_t>(n),
                         " queue=%zu unacked=%zu", row.queue, row.unacked);
            }
            args += line;
        }
        reply_ok_args(args.c_str());
        return;
    }

    reply_nok("unknown command, type help");
}
