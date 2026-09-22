#include "radio_sim.hpp"

#include "modulation.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <sstream>
#include <strings.h>
#include <utility>

namespace
{

constexpr size_t k_console_max = 16384;

bool set_nonblock(int fd)
{
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
    {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

void close_fd(int* fd)
{
    if (fd == nullptr || *fd < 0)
    {
        return;
    }
    ::close(*fd);
    *fd = -1;
}

std::string trim_copy(const char* s)
{
    if (s == nullptr)
    {
        return {};
    }
    while (*s == ' ' || *s == '\t')
    {
        ++s;
    }
    std::string out(s);
    while (!out.empty())
    {
        const char c = out.back();
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
        {
            break;
        }
        out.pop_back();
    }
    return out;
}

bool cmd_is(const std::string& cmd, const char* a, const char* b = nullptr)
{
    return strcasecmp(cmd.c_str(), a) == 0 ||
           (b != nullptr && strcasecmp(cmd.c_str(), b) == 0);
}

bool starts_with(const char* s, const char* prefix)
{
    return strncasecmp(s, prefix, strlen(prefix)) == 0;
}

}  // namespace

radio_sim::radio_sim() = default;

radio_sim::~radio_sim()
{
    stop();
}

int radio_sim::open_udp_bound(const sockaddr_in& addr, bool reuse)
{
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        return -1;
    }
    if (reuse)
    {
        const int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    }
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) <
        0)
    {
        const int err = errno;
        ::close(fd);
        errno = err;
        return -1;
    }
    if (!set_nonblock(fd))
    {
        ::close(fd);
        return -1;
    }
    return fd;
}

bool radio_sim::start(const sockaddr_in& console_bind,
                      const sockaddr_in& air_bind,
                      const std::vector<sockaddr_in>& air_peers,
                      uint32_t display_ip)
{
    stop();
    console_bind_ = console_bind;
    air_peers_ = air_peers;
    display_ip_ = display_ip != 0 ? display_ip : console_bind.sin_addr.s_addr;
    if (display_ip_ == 0 || display_ip_ == htonl(INADDR_ANY))
    {
        display_ip_ = htonl(INADDR_LOOPBACK);
    }
    static_ip_ = display_ip_;
    started_at_ = std::chrono::steady_clock::now();
    frameBegin();
    frameSetMode(WINJECT_MODE_BFC_TUNNEL_DEVICE);

    console_fd_ = open_udp_bound(console_bind, true);
    if (console_fd_ < 0)
    {
        return false;
    }
    air_fd_ = open_udp_bound(air_bind, true);
    if (air_fd_ < 0)
    {
        close_fd(&console_fd_);
        return false;
    }
    forward_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (forward_fd_ < 0)
    {
        stop();
        return false;
    }

    stop_.store(false);
    tx_thread_ = std::thread([this]() { tx_worker(); });
    rx_thread_ = std::thread([this]() { rx_forward_worker(); });
    return true;
}

void radio_sim::stop()
{
    stop_.store(true);
    tx_cv_.notify_all();
    rx_cv_.notify_all();
    if (tx_thread_.joinable())
    {
        tx_thread_.join();
    }
    if (rx_thread_.joinable())
    {
        rx_thread_.join();
    }
    clear_upstream_tx();
    close_fd(&console_fd_);
    close_fd(&air_fd_);
    close_fd(&forward_fd_);
    {
        std::lock_guard<std::mutex> g(tx_mu_);
        tx_q_.clear();
    }
    {
        std::lock_guard<std::mutex> g(rx_mu_);
        rx_q_.clear();
    }
}

bool radio_sim::radio_active() const
{
    // Caller may already hold state_mu_; mode_ is a single word.
    return mode_ != WINJECT_MODE_OTA;
}

void radio_sim::ipv4_to_string(uint32_t host_nbo, char* out, size_t n)
{
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&host_nbo);
    snprintf(out, n, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

bool radio_sim::parse_bool(const char* text, bool* out)
{
    if (text == nullptr || out == nullptr)
    {
        return false;
    }
    if (strcasecmp(text, "1") == 0 || strcasecmp(text, "true") == 0 ||
        strcasecmp(text, "on") == 0 || strcasecmp(text, "yes") == 0)
    {
        *out = true;
        return true;
    }
    if (strcasecmp(text, "0") == 0 || strcasecmp(text, "false") == 0 ||
        strcasecmp(text, "off") == 0 || strcasecmp(text, "no") == 0)
    {
        *out = false;
        return true;
    }
    return false;
}

bool radio_sim::parse_host(const char* text, uint32_t* host_nbo)
{
    if (text == nullptr || host_nbo == nullptr)
    {
        return false;
    }
    in_addr a{};
    if (inet_pton(AF_INET, text, &a) != 1)
    {
        return false;
    }
    *host_nbo = a.s_addr;
    return true;
}

bool radio_sim::parse_port(const char* text, uint16_t* port)
{
    if (text == nullptr || port == nullptr)
    {
        return false;
    }
    char* end = nullptr;
    const long v = strtol(text, &end, 10);
    if (end == text || *end != '\0' || v < 1 || v > 65535)
    {
        return false;
    }
    *port = static_cast<uint16_t>(v);
    return true;
}

bool radio_sim::parse_domain(const char* text, uint16_t* domain)
{
    if (text == nullptr || domain == nullptr)
    {
        return false;
    }
    const char* p = text;
    int base = 16;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
    {
        p += 2;
        base = 16;
    }
    char* end = nullptr;
    const unsigned long v = strtoul(p, &end, base);
    if (end == p || *end != '\0' || v < 1 || v > 65535)
    {
        return false;
    }
    *domain = static_cast<uint16_t>(v);
    return true;
}

bool radio_sim::set_upstream_tx(uint16_t port)
{
    if (port == 0)
    {
        return false;
    }
    clear_upstream_tx();
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    inject_fd_ = open_udp_bound(addr, true);
    if (inject_fd_ < 0)
    {
        return false;
    }
    std::lock_guard<std::mutex> g(state_mu_);
    have_sut_ = true;
    sut_port_ = port;
    return true;
}

bool radio_sim::clear_upstream_tx()
{
    close_fd(&inject_fd_);
    std::lock_guard<std::mutex> g(state_mu_);
    have_sut_ = false;
    sut_port_ = 0;
    return true;
}

bool radio_sim::set_upstream_rx(ip_port_s dest)
{
    if (dest.port == 0 || dest.host == 0 || dest.host == 0xFFFFFFFFu)
    {
        return false;
    }
    std::lock_guard<std::mutex> g(state_mu_);
    have_sur_ = true;
    sur_ = dest;
    return true;
}

bool radio_sim::clear_upstream_rx()
{
    std::lock_guard<std::mutex> g(state_mu_);
    have_sur_ = false;
    sur_ = {};
    return true;
}

void radio_sim::tx_worker()
{
    while (!stop_.load())
    {
        mpdu_buf_s pkt;
        std::string mod;
        {
            std::unique_lock<std::mutex> lk(tx_mu_);
            tx_cv_.wait(lk, [this]() {
                return stop_.load() || !tx_q_.empty();
            });
            if (stop_.load())
            {
                break;
            }
            pkt = std::move(tx_q_.front());
            tx_q_.pop_front();
        }
        {
            std::lock_guard<std::mutex> g(state_mu_);
            mod = modulation_;
        }

        const uint64_t air_us = modulation_airtime_us(mod.c_str(), pkt.len);
        const auto t0 = std::chrono::steady_clock::now();
        if (air_us > 0)
        {
            std::this_thread::sleep_for(std::chrono::microseconds(air_us));
        }
        const auto t1 = std::chrono::steady_clock::now();
        last_tx_latency_us_.store(
            static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0)
                    .count()),
            std::memory_order_relaxed);
        tx_latency_valid_.store(true, std::memory_order_relaxed);

        bool dry = false;
        {
            std::lock_guard<std::mutex> g(state_mu_);
            dry = inject_dry_run_;
        }
        if (dry)
        {
            inject_ok_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        bool any_ok = false;
        for (const auto& peer : air_peers_)
        {
            const ssize_t n =
                ::sendto(air_fd_, pkt.data.data(), pkt.len, 0,
                         reinterpret_cast<const sockaddr*>(&peer), sizeof(peer));
            if (n == static_cast<ssize_t>(pkt.len))
            {
                any_ok = true;
            }
        }
        if (air_peers_.empty())
        {
            // Loopback single-radio: still count as inject for local tests.
            any_ok = true;
        }
        if (any_ok)
        {
            inject_ok_.fetch_add(1, std::memory_order_relaxed);
            air_tx_pkt_.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            inject_fail_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void radio_sim::rx_forward_worker()
{
    while (!stop_.load())
    {
        mpdu_buf_s pkt;
        ip_port_s dest{};
        bool have = false;
        {
            std::unique_lock<std::mutex> lk(rx_mu_);
            rx_cv_.wait(lk, [this]() {
                return stop_.load() || !rx_q_.empty();
            });
            if (stop_.load())
            {
                break;
            }
            pkt = std::move(rx_q_.front());
            rx_q_.pop_front();
        }
        {
            std::lock_guard<std::mutex> g(state_mu_);
            have = have_sur_;
            dest = sur_;
        }
        if (!have || forward_fd_ < 0)
        {
            continue;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = dest.host;
        addr.sin_port = htons(dest.port);
        const ssize_t n =
            ::sendto(forward_fd_, pkt.data.data(), pkt.len, 0,
                     reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        if (n == static_cast<ssize_t>(pkt.len))
        {
            udp_fwd_pkt_.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            drop_send_fail_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void radio_sim::on_inject_readable()
{
    if (inject_fd_ < 0 || !radio_active())
    {
        // Drain to avoid POLLIN spin when OTA.
        uint8_t sink[WIFI_RADIO_INJECT_MAX];
        while (::recv(inject_fd_, sink, sizeof(sink), 0) > 0)
        {
        }
        return;
    }

    for (;;)
    {
        mpdu_buf_s pkt;
        const ssize_t n =
            ::recv(inject_fd_, pkt.data.data(), pkt.data.size(), 0);
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            {
                return;
            }
            return;
        }
        if (n == 0)
        {
            return;
        }
        udp_tx_pkt_.fetch_add(1, std::memory_order_relaxed);
        if (static_cast<size_t>(n) < WIFI_RADIO_INJECT_MIN ||
            static_cast<size_t>(n) > WIFI_RADIO_INJECT_MAX)
        {
            drop_inject_size_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        pkt.len = static_cast<size_t>(n);
        bool null_sink = false;
        {
            std::lock_guard<std::mutex> g(state_mu_);
            null_sink = inject_null_sink_;
        }
        if (null_sink)
        {
            continue;
        }
        size_t qsize = 0;
        {
            std::lock_guard<std::mutex> g(tx_mu_);
            if (tx_q_.size() >= k_tx_queue)
            {
                drop_tx_queue_full_.fetch_add(1, std::memory_order_relaxed);
                tx_enqueue_fail_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            tx_q_.push_back(std::move(pkt));
            qsize = tx_q_.size();
            tx_enqueue_ok_.fetch_add(1, std::memory_order_relaxed);
            uint32_t hwm = tx_q_hwm_.load(std::memory_order_relaxed);
            if (qsize > hwm)
            {
                tx_q_hwm_.store(static_cast<uint32_t>(qsize),
                                std::memory_order_relaxed);
            }
        }
        tx_cv_.notify_one();
    }
}

void radio_sim::on_air_readable()
{
    if (air_fd_ < 0)
    {
        return;
    }
    for (;;)
    {
        mpdu_buf_s pkt;
        sockaddr_in from{};
        socklen_t fromlen = sizeof(from);
        const ssize_t n =
            ::recvfrom(air_fd_, pkt.data.data(), pkt.data.size(), 0,
                       reinterpret_cast<sockaddr*>(&from), &fromlen);
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            {
                return;
            }
            return;
        }
        if (n == 0)
        {
            return;
        }
        if (!radio_active())
        {
            continue;
        }
        if (static_cast<size_t>(n) < WIFI_RADIO_INJECT_MIN ||
            static_cast<size_t>(n) > WIFI_RADIO_INJECT_MAX)
        {
            continue;
        }
        pkt.len = static_cast<size_t>(n);

        uint16_t domain = 0;
        {
            std::lock_guard<std::mutex> g(state_mu_);
            domain = domain_;
        }
        if (!frameAddr3Accept(pkt.data.data(), pkt.len, domain))
        {
            continue;
        }
        air_rx_pkt_.fetch_add(1, std::memory_order_relaxed);

        {
            std::lock_guard<std::mutex> g(rx_mu_);
            if (rx_q_.size() >= k_rx_queue)
            {
                drop_rx_queue_full_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            rx_q_.push_back(std::move(pkt));
        }
        rx_cv_.notify_one();
    }
}

void radio_sim::on_console_readable()
{
    if (console_fd_ < 0)
    {
        return;
    }
    for (;;)
    {
        char buf[k_console_max];
        sockaddr_in from{};
        socklen_t fromlen = sizeof(from);
        const ssize_t n =
            ::recvfrom(console_fd_, buf, sizeof(buf) - 1, 0,
                       reinterpret_cast<sockaddr*>(&from), &fromlen);
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            {
                return;
            }
            return;
        }
        if (n == 0)
        {
            return;
        }
        buf[n] = '\0';
        // Strip trailing CR/LF for line commands; ether_test_rx keeps binary.
        std::string reply;
        if (starts_with(buf, "ether_test_rx") || starts_with(buf, "etr"))
        {
            // Minimal: always nok (no shared buffer needed for manager path).
            reply = "nok sim ether_test_rx not required\n";
        }
        else
        {
            size_t end = static_cast<size_t>(n);
            while (end > 0)
            {
                const char c = buf[end - 1];
                if (c != '\n' && c != '\r' && c != ' ' && c != '\t')
                {
                    break;
                }
                --end;
            }
            buf[end] = '\0';
            reply = handle_command(buf);
        }
        if (!reply.empty())
        {
            ::sendto(console_fd_, reply.data(), reply.size(), 0,
                     reinterpret_cast<sockaddr*>(&from), sizeof(from));
        }
    }
}

std::string radio_sim::help_text() const
{
    std::ostringstream o;
    o << "set_mode|sm <BFC_TUNNEL_DEVICE|STANDALONE|OTA>\n"
      << "set_domain|sdom <domain>\n"
      << "unset_domain|udom\n"
      << "set_upstream_rx|sur host=<ip> port=<port>\n"
      << "unset_upstream_rx|uur\n"
      << "set_upstream_tx|sut port=<port>\n"
      << "unset_upstream_tx|uut\n"
      << "set_inject_sink|sis <wifi|null|dry>\n"
      << "set_logger|sl address=<host>:<port> level=<error|warn|info|debug>\n"
      << "unset_logger|ul\n"
      << "set_allow_failed_crc|saf <0|1>\n"
      << "set_channel|sc <channel>\n"
      << "set_modulation|sd <modulation>\n"
      << "set_cca_enabled|sce <0|1>\n"
      << "set_tx_power|stp <2-20>\n"
      << "set_network|sn <STATIC|AUTO>\n"
      << "set_ip|sfi <ip>\n"
      << "save|sv [0-9]\n"
      << "use|u <0-9>\n"
      << "status|s\n"
      << "ping  (rsp: pong)\n"
      << "reset|r\n"
      << "help\n"
      << "modulations: " << modulation_list() << "\n"
      << "sim: behavioral host radio (queues=" << k_tx_queue
      << " airtime by modulation)\n";
    return o.str();
}

std::string radio_sim::status_text()
{
    std::lock_guard<std::mutex> g(state_mu_);
    const auto now = std::chrono::steady_clock::now();
    const auto up_s =
        std::chrono::duration_cast<std::chrono::seconds>(now - started_at_)
            .count();

    char ip_str[16];
    const uint32_t ip = static_ip_ != 0 ? static_ip_ : display_ip_;
    ipv4_to_string(ip, ip_str, sizeof(ip_str));

    char domain_str[8] = "unset";
    if (domain_ != 0)
    {
        snprintf(domain_str, sizeof(domain_str), "%04X", domain_);
    }

    size_t txq = 0;
    size_t rxq = 0;
    {
        std::lock_guard<std::mutex> t(tx_mu_);
        txq = tx_q_.size();
    }
    {
        std::lock_guard<std::mutex> r(rx_mu_);
        rxq = rx_q_.size();
    }

    std::ostringstream o;
    o << "ok\n";
    o << "# device\n";
    o << "device uptime=" << up_s
      << "s reset_reason=sim heap_usage=0 heap_free=0 mode="
      << frameModeName(mode_) << " domain=" << domain_str << "\n";
    o << "# network\n";
    o << "network ip=" << ip_str << "/24 mode="
      << (net_static_ ? "static" : "dhcp")
      << " console_port=" << ntohs(console_bind_.sin_port) << "\n";
    o << "ota waiting\n";
    if (!radio_active())
    {
        o << "# radio\n";
        o << "radio disabled (OTA mode)\n";
        return o.str();
    }
    o << "# wifi\n";
    o << "wifi_txrx channel=" << static_cast<unsigned>(channel_) << "\n";
    o << "wifi_tx modulation=" << modulation_
      << " cca=" << (cca_enabled_ ? "enabled" : "disabled")
      << " tx_power=" << tx_power_dbm_ << "\n";
    o << "wifi_rx radio rssi=- snr=- allow_failed_crc="
      << (allow_failed_crc_ ? "true" : "false") << "\n";
    if (have_sut_ || have_sur_)
    {
        o << "# upstreams\n";
        if (have_sut_)
        {
            o << "upstream_tx port=" << sut_port_ << "\n";
        }
        if (have_sur_)
        {
            char h[16];
            ipv4_to_string(sur_.host, h, sizeof(h));
            o << "upstream_rx host=" << h << " port=" << sur_.port << "\n";
        }
    }
    o << "# channel\n";
    char lat[16] = "-";
    if (tx_latency_valid_.load())
    {
        snprintf(lat, sizeof(lat), "%lluus",
                 static_cast<unsigned long long>(
                     last_tx_latency_us_.load()));
    }
    const char* sink =
        inject_null_sink_ ? "null" : (inject_dry_run_ ? "dry" : "wifi");
    o << "channel_tx queue=" << txq << " queue_hwm=" << tx_q_hwm_.load()
      << " enqueue_ok=" << tx_enqueue_ok_.load()
      << " enqueue_fail=" << tx_enqueue_fail_.load()
      << " pool_free=255 pool_free_min=255 drop_nomem=0 retry_count=0 "
         "retry_nomem=0 retry_other=0 inject_ok="
      << inject_ok_.load() << " inject_fail=" << inject_fail_.load()
      << " in_flight=0 tx_latency=" << lat << " inject_wait=- udp_tx="
      << udp_tx_pkt_.load() << " sink=" << sink << "\n";
    o << "channel_rx queue=" << rxq << " drop_no_pkt_pool=0 drop_queue_full="
      << drop_rx_queue_full_.load() << " drop_crc_error=0 wifi_accept="
      << air_rx_pkt_.load() << " udp_fwd=" << udp_fwd_pkt_.load() << "\n";
    if (have_sut_)
    {
        o << "channels_tx drop_no_pkt_pool=0 drop_queue_full="
          << drop_tx_queue_full_.load() << " pool_free_min=255 sink=" << sink
          << "\n";
    }
    if (have_sur_)
    {
        o << "channels_rx drop_send_fail=" << drop_send_fail_.load() << "\n";
    }
    if (have_logger_)
    {
        char h[16];
        ipv4_to_string(logger_.host, h, sizeof(h));
        o << "# logger\n";
        o << "logger address=" << h << ":" << logger_.port
          << " level=" << logger_level_ << " emitted=0 dropped=0\n";
    }
    return o.str();
}

std::string radio_sim::handle_command(const char* line_in)
{
    const std::string line = trim_copy(line_in);
    if (line.empty() || line[0] == '#')
    {
        return {};
    }

    std::istringstream iss(line);
    std::string cmd;
    iss >> cmd;
    std::vector<std::string> args;
    std::string tok;
    while (iss >> tok)
    {
        args.push_back(tok);
    }

    auto usage = [](const char* u) {
        return std::string("nok usage: ") + u + "\n";
    };
    auto nok = [](const char* m) {
        return std::string("nok ") + m + "\n";
    };
    auto ok = []() {
        return std::string("ok\n");
    };
    auto require_radio = [&]() -> bool {
        std::lock_guard<std::mutex> g(state_mu_);
        return radio_active();
    };

    if (cmd_is(cmd, "help", "?"))
    {
        return help_text();
    }
    if (cmd_is(cmd, "status", "s"))
    {
        if (!args.empty())
        {
            return usage("status");
        }
        return status_text();
    }
    if (cmd_is(cmd, "ping"))
    {
        if (!args.empty())
        {
            return usage("ping");
        }
        return "pong\n";
    }
    if (cmd_is(cmd, "reset", "r"))
    {
        // Soft reset: clear upstreams, keep process alive.
        clear_upstream_tx();
        clear_upstream_rx();
        {
            std::lock_guard<std::mutex> g(state_mu_);
            inject_null_sink_ = false;
            inject_dry_run_ = false;
            domain_ = 0;
            channel_ = WIFI_DEFAULT_CHANNEL;
            modulation_ = WIFI_DEFAULT_MODULATION;
            mode_ = WINJECT_MODE_BFC_TUNNEL_DEVICE;
        }
        frameSetMode(WINJECT_MODE_BFC_TUNNEL_DEVICE);
        return ok();
    }

    if (cmd_is(cmd, "set_mode", "sm"))
    {
        if (args.size() != 1)
        {
            return usage("set_mode <BFC_TUNNEL_DEVICE|STANDALONE|OTA>");
        }
        WinjectMode mode = WINJECT_MODE_BFC_TUNNEL_DEVICE;
        if (!frameParseMode(args[0].c_str(), &mode))
        {
            return usage("set_mode <BFC_TUNNEL_DEVICE|STANDALONE|OTA>");
        }
        {
            std::lock_guard<std::mutex> g(state_mu_);
            mode_ = mode;
        }
        if (!frameSetMode(mode))
        {
            return nok("failed to set mode");
        }
        if (mode == WINJECT_MODE_OTA)
        {
            clear_upstream_tx();
            clear_upstream_rx();
        }
        return ok();
    }

    if (cmd_is(cmd, "set_domain", "sdom"))
    {
        if (!require_radio())
        {
            return nok("unavailable in OTA mode");
        }
        if (args.size() != 1)
        {
            return usage("set_domain <domain>");
        }
        uint16_t domain = 0;
        if (!parse_domain(args[0].c_str(), &domain))
        {
            return usage("set_domain <domain>");
        }
        std::lock_guard<std::mutex> g(state_mu_);
        domain_ = domain;
        return ok();
    }
    if (cmd_is(cmd, "unset_domain", "udom"))
    {
        if (!require_radio())
        {
            return nok("unavailable in OTA mode");
        }
        if (!args.empty())
        {
            return usage("unset_domain");
        }
        std::lock_guard<std::mutex> g(state_mu_);
        domain_ = 0;
        return ok();
    }

    if (cmd_is(cmd, "set_upstream_tx", "sut"))
    {
        if (!require_radio())
        {
            return nok("unavailable in OTA mode");
        }
        if (args.size() != 1 || !starts_with(args[0].c_str(), "port="))
        {
            return usage("set_upstream_tx port=<port>");
        }
        uint16_t port = 0;
        if (!parse_port(args[0].c_str() + 5, &port))
        {
            return usage("set_upstream_tx port=<port>");
        }
        if (!set_upstream_tx(port))
        {
            return nok("failed to bind udp port");
        }
        return ok();
    }
    if (cmd_is(cmd, "unset_upstream_tx", "uut"))
    {
        if (!require_radio())
        {
            return nok("unavailable in OTA mode");
        }
        if (!args.empty())
        {
            return usage("unset_upstream_tx");
        }
        clear_upstream_tx();
        return ok();
    }
    if (cmd_is(cmd, "set_upstream_rx", "sur"))
    {
        if (!require_radio())
        {
            return nok("unavailable in OTA mode");
        }
        if (args.size() != 2 || !starts_with(args[0].c_str(), "host=") ||
            !starts_with(args[1].c_str(), "port="))
        {
            return usage("set_upstream_rx host=<ip> port=<port>");
        }
        ip_port_s dest{};
        if (!parse_host(args[0].c_str() + 5, &dest.host) ||
            !parse_port(args[1].c_str() + 5, &dest.port))
        {
            return usage("set_upstream_rx host=<ip> port=<port>");
        }
        if (!set_upstream_rx(dest))
        {
            return nok("failed to set upstream rx");
        }
        return ok();
    }
    if (cmd_is(cmd, "unset_upstream_rx", "uur"))
    {
        if (!require_radio())
        {
            return nok("unavailable in OTA mode");
        }
        if (!args.empty())
        {
            return usage("unset_upstream_rx");
        }
        clear_upstream_rx();
        return ok();
    }

    if (cmd_is(cmd, "set_inject_sink", "sis"))
    {
        if (!require_radio())
        {
            return nok("unavailable in OTA mode");
        }
        if (args.size() != 1)
        {
            return usage("set_inject_sink <wifi|null|dry>");
        }
        bool null_sink = false;
        bool dry_run = false;
        if (strcasecmp(args[0].c_str(), "null") == 0)
        {
            null_sink = true;
        }
        else if (strcasecmp(args[0].c_str(), "dry") == 0)
        {
            dry_run = true;
        }
        else if (strcasecmp(args[0].c_str(), "wifi") != 0)
        {
            return usage("set_inject_sink <wifi|null|dry>");
        }
        std::lock_guard<std::mutex> g(state_mu_);
        inject_null_sink_ = null_sink;
        inject_dry_run_ = dry_run;
        return ok();
    }

    if (cmd_is(cmd, "set_logger", "sl"))
    {
        // address=<host>:<port> level=<...>
        if (args.size() != 2 || !starts_with(args[0].c_str(), "address=") ||
            !starts_with(args[1].c_str(), "level="))
        {
            return usage(
                "set_logger address=<host>:<port> level=<error|warn|info|debug>");
        }
        const char* hp = args[0].c_str() + 8;
        const char* colon = strchr(hp, ':');
        if (colon == nullptr)
        {
            return nok("bad address");
        }
        std::string host(hp, colon);
        ip_port_s dest{};
        if (!parse_host(host.c_str(), &dest.host) ||
            !parse_port(colon + 1, &dest.port))
        {
            return nok("bad address");
        }
        const char* level = args[1].c_str() + 6;
        if (strcasecmp(level, "error") != 0 && strcasecmp(level, "warn") != 0 &&
            strcasecmp(level, "info") != 0 && strcasecmp(level, "debug") != 0)
        {
            return nok("bad level");
        }
        std::lock_guard<std::mutex> g(state_mu_);
        have_logger_ = true;
        logger_ = dest;
        logger_level_ = level;
        return ok();
    }
    if (cmd_is(cmd, "unset_logger", "ul"))
    {
        if (!args.empty())
        {
            return usage("unset_logger");
        }
        std::lock_guard<std::mutex> g(state_mu_);
        have_logger_ = false;
        logger_ = {};
        return ok();
    }

    if (cmd_is(cmd, "set_allow_failed_crc", "saf"))
    {
        if (!require_radio())
        {
            return nok("unavailable in OTA mode");
        }
        if (args.size() != 1)
        {
            return usage("set_allow_failed_crc <0|1>");
        }
        bool v = false;
        if (!parse_bool(args[0].c_str(), &v))
        {
            return usage("set_allow_failed_crc <0|1>");
        }
        std::lock_guard<std::mutex> g(state_mu_);
        allow_failed_crc_ = v;
        return ok();
    }

    if (cmd_is(cmd, "set_channel", "sc"))
    {
        if (!require_radio())
        {
            return nok("unavailable in OTA mode");
        }
        if (args.size() != 1)
        {
            return usage("set_channel <channel>");
        }
        char* end = nullptr;
        const long ch = strtol(args[0].c_str(), &end, 10);
        if (end == args[0].c_str() || *end != '\0' || ch < WIFI_CHANNEL_MIN ||
            ch > WIFI_CHANNEL_MAX)
        {
            return usage("set_channel <channel>");
        }
        std::string mod;
        {
            std::lock_guard<std::mutex> g(state_mu_);
            mod = modulation_;
        }
        if (!modulation_ok_for_channel(mod.c_str(),
                                       static_cast<uint8_t>(ch)))
        {
            return nok("modulation not allowed on channel");
        }
        std::lock_guard<std::mutex> g(state_mu_);
        channel_ = static_cast<uint8_t>(ch);
        return ok();
    }

    if (cmd_is(cmd, "set_modulation", "sd"))
    {
        if (!require_radio())
        {
            return nok("unavailable in OTA mode");
        }
        if (args.size() != 1)
        {
            return usage("set_modulation <modulation>");
        }
        if (modulation_find(args[0].c_str()) == nullptr)
        {
            return nok("unknown modulation");
        }
        uint8_t ch = 0;
        {
            std::lock_guard<std::mutex> g(state_mu_);
            ch = channel_;
        }
        if (!modulation_ok_for_channel(args[0].c_str(), ch))
        {
            return nok("modulation not allowed on channel");
        }
        std::lock_guard<std::mutex> g(state_mu_);
        modulation_ = modulation_find(args[0].c_str())->name;
        return ok();
    }

    if (cmd_is(cmd, "set_cca_enabled", "sce"))
    {
        if (!require_radio())
        {
            return nok("unavailable in OTA mode");
        }
        if (args.size() != 1)
        {
            return usage("set_cca_enabled <0|1>");
        }
        bool v = false;
        if (!parse_bool(args[0].c_str(), &v))
        {
            return usage("set_cca_enabled <0|1>");
        }
        std::lock_guard<std::mutex> g(state_mu_);
        cca_enabled_ = v;
        return ok();
    }

    if (cmd_is(cmd, "set_tx_power", "stp"))
    {
        if (!require_radio())
        {
            return nok("unavailable in OTA mode");
        }
        if (args.size() != 1)
        {
            return usage("set_tx_power <2-20>");
        }
        char* end = nullptr;
        const long p = strtol(args[0].c_str(), &end, 10);
        if (end == args[0].c_str() || *end != '\0' ||
            p < WIFI_TX_POWER_DBM_MIN || p > WIFI_TX_POWER_DBM_MAX)
        {
            return usage("set_tx_power <2-20>");
        }
        std::lock_guard<std::mutex> g(state_mu_);
        tx_power_dbm_ = static_cast<int>(p);
        return ok();
    }

    if (cmd_is(cmd, "set_network", "sn"))
    {
        if (args.size() != 1)
        {
            return usage("set_network <STATIC|AUTO>");
        }
        if (strcasecmp(args[0].c_str(), "STATIC") == 0)
        {
            std::lock_guard<std::mutex> g(state_mu_);
            net_static_ = true;
            return ok();
        }
        if (strcasecmp(args[0].c_str(), "AUTO") == 0)
        {
            std::lock_guard<std::mutex> g(state_mu_);
            net_static_ = false;
            return ok();
        }
        return usage("set_network <STATIC|AUTO>");
    }

    if (cmd_is(cmd, "set_ip", "sfi") || cmd_is(cmd, "set_fallback_ip", "sfi"))
    {
        if (args.size() != 1)
        {
            return usage("set_ip <ip>");
        }
        uint32_t ip = 0;
        if (!parse_host(args[0].c_str(), &ip))
        {
            return nok("bad ip");
        }
        std::lock_guard<std::mutex> g(state_mu_);
        static_ip_ = ip;
        return ok();
    }

    if (cmd_is(cmd, "save", "sv") || cmd_is(cmd, "use", "u"))
    {
        // Persistence is a no-op in the sim; accept for console parity.
        if (args.size() > 1)
        {
            return usage(cmd_is(cmd, "save", "sv") ? "save [0-9]" : "use <0-9>");
        }
        if (args.size() == 1)
        {
            char* end = nullptr;
            const long slot = strtol(args[0].c_str(), &end, 10);
            if (end == args[0].c_str() || *end != '\0' || slot < 0 ||
                slot > 9)
            {
                return nok("bad slot");
            }
        }
        return ok();
    }

    return nok("unknown command");
}
