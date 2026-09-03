#include "console.h"

#include "config.h"
#include "frame.h"
#include "ota.h"
#include "upstream_rx.h"
#include "upstream_tx.h"
#include "wifi.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "manager.h"
#include "settings.h"

static const char* TAG = "console";

static const char* reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason)
    {
        case ESP_RST_POWERON:
            return "power-on";
        case ESP_RST_EXT:
            return "external";
        case ESP_RST_SW:
            return "software";
        case ESP_RST_PANIC:
            return "panic";
        case ESP_RST_INT_WDT:
            return "interrupt-wdt";
        case ESP_RST_TASK_WDT:
            return "task-wdt";
        case ESP_RST_WDT:
            return "wdt";
        case ESP_RST_BROWNOUT:
            return "brownout";
        default:
            return "unknown";
    }
}

static bool set_nonblock(int fd)
{
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
    {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static void close_fd(int* fd)
{
    if (fd == nullptr || *fd < 0)
    {
        return;
    }
    close(*fd);
    *fd = -1;
}

static void ipv4_to_string(uint32_t addr, char* out, size_t out_len)
{
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&addr);
    snprintf(out, out_len, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

static bool parse_bool(const char* text, bool* value)
{
    if (text == nullptr || value == nullptr)
    {
        return false;
    }
    if (strcasecmp(text, "1") == 0 || strcasecmp(text, "true") == 0 ||
        strcasecmp(text, "on") == 0 || strcasecmp(text, "yes") == 0)
    {
        *value = true;
        return true;
    }
    if (strcasecmp(text, "0") == 0 || strcasecmp(text, "false") == 0 ||
        strcasecmp(text, "off") == 0 || strcasecmp(text, "no") == 0)
    {
        *value = false;
        return true;
    }
    return false;
}

static bool parse_port(const char* text, uint16_t* port)
{
    if (text == nullptr || port == nullptr || *text == '\0')
    {
        return false;
    }
    char* end = nullptr;
    const long value = strtol(text, &end, 10);
    if (end == text || *end != '\0' || value <= 0 || value > 65535)
    {
        return false;
    }
    *port = static_cast<uint16_t>(value);
    return true;
}

static bool parse_channel(const char* text, uint8_t* channel)
{
    if (text == nullptr || channel == nullptr || *text == '\0')
    {
        return false;
    }
    char* end = nullptr;
    const long value = strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < 1 || value > 13)
    {
        return false;
    }
    *channel = static_cast<uint8_t>(value);
    return true;
}

static bool parse_tx_power(const char* text, int8_t* dbm)
{
    if (text == nullptr || dbm == nullptr || *text == '\0')
    {
        return false;
    }
    char* end = nullptr;
    const long value = strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < WIFI_TX_POWER_DBM_MIN ||
        value > WIFI_TX_POWER_DBM_MAX)
    {
        return false;
    }
    *dbm = static_cast<int8_t>(value);
    return true;
}

static bool parse_host(const char* text, uint32_t* ip)
{
    if (text == nullptr || ip == nullptr || *text == '\0')
    {
        return false;
    }
    struct in_addr addr = {};
    if (inet_pton(AF_INET, text, &addr) == 1)
    {
        *ip = addr.s_addr;
        return true;
    }

    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo* res = nullptr;
    if (getaddrinfo(text, nullptr, &hints, &res) != 0 || res == nullptr)
    {
        return false;
    }
    const auto* sin = reinterpret_cast<const struct sockaddr_in*>(res->ai_addr);
    *ip = sin->sin_addr.s_addr;
    freeaddrinfo(res);
    return *ip != 0;
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

static bool parse_packed_mac(const char* text, uint8_t mac[6])
{
    if (text == nullptr || mac == nullptr || strlen(text) != 12)
    {
        return false;
    }
    for (int i = 0; i < 6; i++)
    {
        const int hi = hex_nibble(text[i * 2]);
        const int lo = hex_nibble(text[i * 2 + 1]);
        if (hi < 0 || lo < 0)
        {
            return false;
        }
        mac[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

static bool parse_colon_mac(const char* text, uint8_t mac[6])
{
    if (text == nullptr || mac == nullptr)
    {
        return false;
    }
    const char* p = text;
    for (int i = 0; i < 6; i++)
    {
        char* end = nullptr;
        const long value = strtol(p, &end, 16);
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

static bool parse_airport(const char* text, uint8_t mac[6])
{
    if (text == nullptr || mac == nullptr || *text == '\0')
    {
        return false;
    }
    if (strcmp(text, "0") == 0)
    {
        memset(mac, 0, 6);
        return true;
    }
    if (strchr(text, ':') != nullptr)
    {
        return parse_colon_mac(text, mac);
    }
    return parse_packed_mac(text, mac);
}

static bool airport_allowed(const uint8_t mac[6])
{
    if (frameGetMode() == WINJECT_MODE_STANDALONE)
    {
        return frameAirportValidStandalone(mac);
    }
    return frameAirportIsZero(mac);
}

static const char* airport_mode_error()
{
    if (frameGetMode() == WINJECT_MODE_STANDALONE)
    {
        return "error: airport must be 00:00:00:xx:xx:xx (broadcast) or "
               "xx:xx:xx:yy:yy:yy (P2P) in STANDALONE\n";
    }
    return "error: airport must be 0 in BFC_TUNNEL_DEVICE\n";
}

static bool cmd_is(const char* cmd, const char* name, const char* alias)
{
    return strcasecmp(cmd, name) == 0 || strcasecmp(cmd, alias) == 0;
}

console::tcp_client_s* console::client_by_fd(int fd)
{
    if (fd < 0)
    {
        return nullptr;
    }
    for (tcp_client_s& client : clients_)
    {
        if (client.fd == fd)
        {
            return &client;
        }
    }
    return nullptr;
}

size_t console::client_count() const
{
    size_t n = 0;
    for (const tcp_client_s& client : clients_)
    {
        if (client.fd >= 0)
        {
            n++;
        }
    }
    return n;
}

void console::close_client(int fd)
{
    tcp_client_s* client = client_by_fd(fd);
    if (client == nullptr)
    {
        return;
    }
    if (reactor_ != nullptr)
    {
        reactor_->rem_read_rdy(fd);
    }
    close_fd(&client->fd);
    client->len = 0;
}

void console::close_all_clients()
{
    for (tcp_client_s& client : clients_)
    {
        if (client.fd >= 0)
        {
            close_fd(&client.fd);
            client.len = 0;
        }
    }
}

void console::stop_tcp()
{
    if (listen_fd_ >= 0 && reactor_ != nullptr)
    {
        reactor_->rem_read_rdy(listen_fd_);
    }
    close_all_clients();
    close_fd(&listen_fd_);
}

void console::write(const out_s& out, const char* text)
{
    if (text == nullptr || *text == '\0' || out.fd < 0)
    {
        return;
    }
    if (client_by_fd(out.fd) == nullptr)
    {
        return;
    }

    size_t len = strlen(text);
    size_t off = 0;
    while (off < len)
    {
        const int n = send(out.fd, text + off, len - off, 0);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                return;
            }
            close_client(out.fd);
            return;
        }
        if (n == 0)
        {
            close_client(out.fd);
            return;
        }
        off += static_cast<size_t>(n);
    }
}

void console::print(const out_s& out, const char* fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    write(out, buf);
}

void console::print_mac(const out_s& out, const uint8_t mac[6])
{
    print(out, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3],
          mac[4], mac[5]);
}

void console::print_banner(const out_s& out)
{
    write(out, "WInject-ESP32  bfc-tunnel external multicast radio\n");
    write(out, "type help\n");
}

void console::print_help(const out_s& out)
{
    write(out, "set_mode|sm <BFC_TUNNEL_DEVICE|STANDALONE>\n");
    write(out, "set_upstream_rx|sur [airport] <port>\n");
    write(out, "set_upstream_tx|sut [airport] <host> <port>\n");
    write(out, "unset_upstream_rx|uur [airport]\n");
    write(out, "unset_upstream_tx|uut [airport]\n");
    write(out, "set_allow_failed_crc|saf <0|1>\n");
    write(out, "set_channel|sc <channel>\n");
    write(out, "set_modulation|sd <modulation>\n");
    write(out, "set_cca_enabled|sce <0|1>\n");
    write(out, "set_tx_power|stp <2-20>\n");
    write(out, "set_network|sn <STATIC|AUTO>\n");
    write(out, "set_enable_dhcp_server|sed <0|1>\n");
    write(out, "set_ip|sfi <ip>\n");
    write(out, "save|sv [0-9]\n");
    write(out, "use|u <0-9>\n");
    write(out, "status|s\n");
    write(out, "help\n");
    write(out, "modulations: ");
    write(out, wifi::modulation_list());
    write(out, "\n");
    write(out, "ota: HTTP POST /update\n");
}

void console::print_path_metrics(const out_s& out, const wifi_status_s& radio)
{
    print(out,
          " udp_rx_pkt=%u udp_fwd_pkt=%u udp_tx_pkt=%u udp_tx_dropped=%u "
          "udp_tx_failed=%u udp_rx_crc_err=%u udp_rx_dropped=%u tx_queue=%u "
          "rx_queue=%u\n",
          (unsigned)radio.udp_rx_pkt, (unsigned)radio.udp_fwd_pkt,
          (unsigned)radio.udp_tx_pkt, (unsigned)radio.udp_tx_dropped,
          (unsigned)radio.udp_tx_failed, (unsigned)radio.udp_rx_crc_err,
          (unsigned)radio.udp_rx_dropped, (unsigned)radio.tx_queue,
          (unsigned)radio.rx_queue);
}

void console::print_status(const out_s& out)
{
    wifi_status_s radio = {};
    static upstream_bind_s rx[WIFI_AIRPORT_MAX];
    static upstream_dest_s tx[WIFI_AIRPORT_MAX];
    uint8_t rx_count = 0;
    uint8_t tx_count = 0;
    wifi::instance().get_status(&radio);
    rx_->fill_status(rx, &rx_count);
    tx_->fill_status(tx, &tx_count);

    const size_t heap_total = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    const size_t heap_free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const size_t heap_usage =
        heap_total > heap_free ? heap_total - heap_free : 0;
    print(out, "uptime=%lus reset_reason=%s heap_usage=%u heap_free=%u\n",
          (unsigned long)(esp_timer_get_time() / 1000000),
          reset_reason_name(esp_reset_reason()), (unsigned)heap_usage,
          (unsigned)heap_free);

    uint32_t eth_ip = 0;
    const bool have_ip = netmgr_->local_ipv4(&eth_ip);
    char ip_str[16] = "-";
    if (have_ip)
    {
        ipv4_to_string(eth_ip, ip_str, sizeof(ip_str));
    }
    print(out, "network ip=%s console_port=%u\n", ip_str, CONTROL_CONSOLE_PORT);

    uint32_t static_ip = 0;
    uint32_t pool_start = 0;
    uint32_t pool_end = 0;
    if (netmgr_->static_ipv4(&static_ip) &&
        netmgr_->dhcp_pool(&pool_start, &pool_end))
    {
        char static_str[16];
        char pool_str[16];
        ipv4_to_string(static_ip, static_str, sizeof(static_str));
        ipv4_to_string(pool_start, pool_str, sizeof(pool_str));
        const uint8_t* net = reinterpret_cast<const uint8_t*>(&static_ip);
        const uint8_t* pool_hi = reinterpret_cast<const uint8_t*>(&pool_end);
        const bool auto_mode = netmgr_->network_mode() == NETMGR_MODE_AUTO;
        const char* dhcps = "off";
        if (auto_mode)
        {
            dhcps = netmgr_->dhcp_server_enabled() ? "blocked" : "off";
        }
        else if (netmgr_->dhcp_server_active())
        {
            dhcps = "active";
        }
        else if (netmgr_->dhcp_server_enabled())
        {
            dhcps = "enabled";
        }
        print(out,
              "dhcp mode=%s static_ip=%s dhcps=%s pool=%s-%u "
              "netmask=%u.%u.%u.0/24\n",
              manager::network_mode_name(netmgr_->network_mode()), static_str,
              dhcps, pool_str, pool_hi[3], net[0], net[1], net[2]);
    }

    if (otaActive() && have_ip)
    {
        print(out, "ota url=http://%s:%u/update\n", ip_str, OTA_HTTP_PORT);
    }
    else
    {
        write(out, "ota waiting\n");
    }

    print(out,
          "wifi mode=%s channel=%u modulation=%s cca=%s tx_power=%d "
          "allow_failed_crc=%s\n",
          frameModeName(frameGetMode()), radio.channel,
          radio.modulation != nullptr ? radio.modulation : "-",
          radio.cca_enabled ? "enabled" : "disabled", radio.tx_power_dbm,
          radio.allow_failed_crc ? "true" : "false");

    if (radio.rx_air_valid && radio.rx_modulation != nullptr)
    {
        print(out, "radio modulation=%s rssi=%d snr=%d\n", radio.rx_modulation,
              radio.rx_rssi, radio.rx_snr);
    }
    else
    {
        write(out, "radio modulation=- rssi=- snr=-\n");
    }

    bool tx_used[WIFI_AIRPORT_MAX] = {};
    uint8_t printed = 0;
    for (uint8_t i = 0; i < rx_count; i++)
    {
        const upstream_dest_s* match = nullptr;
        for (uint8_t j = 0; j < tx_count; j++)
        {
            if (memcmp(rx[i].airport, tx[j].airport, 6) == 0)
            {
                match = &tx[j];
                tx_used[j] = true;
                break;
            }
        }
        write(out, "upstream_");
        print_mac(out, rx[i].airport);
        if (match != nullptr)
        {
            char host_str[16];
            ipv4_to_string(match->host, host_str, sizeof(host_str));
            print(out, " tx=%s:%u rx=%u", host_str, match->port, rx[i].port);
        }
        else
        {
            print(out, " tx=- rx=%u", rx[i].port);
        }
        print_path_metrics(out, radio);
        printed++;
    }
    for (uint8_t j = 0; j < tx_count; j++)
    {
        if (tx_used[j])
        {
            continue;
        }
        char host_str[16];
        ipv4_to_string(tx[j].host, host_str, sizeof(host_str));
        write(out, "upstream_");
        print_mac(out, tx[j].airport);
        print(out, " tx=%s:%u rx=-", host_str, tx[j].port);
        print_path_metrics(out, radio);
        printed++;
    }
    if (printed == 0)
    {
        write(out, "upstream unset");
        print_path_metrics(out, radio);
    }
}

void console::handle_line(const char* line, const out_s& out)
{
    char copy[128];
    strncpy(copy, line, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = '\0';

    char* save = nullptr;
    char* cmd = strtok_r(copy, " \t", &save);
    if (cmd == nullptr || *cmd == '\0' || cmd[0] == '#')
    {
        return;
    }

    if (cmd_is(cmd, "help", "?"))
    {
        print_help(out);
        return;
    }
    if (cmd_is(cmd, "status", "s"))
    {
        print_status(out);
        return;
    }

    char* arg1 = strtok_r(nullptr, " \t", &save);
    char* arg2 = strtok_r(nullptr, " \t", &save);
    char* arg3 = strtok_r(nullptr, " \t", &save);
    char* extra = strtok_r(nullptr, " \t", &save);

    if (cmd_is(cmd, "set_mode", "sm"))
    {
        WinjectMode mode = WINJECT_MODE_BFC_TUNNEL_DEVICE;
        if (!frameParseMode(arg1, &mode) || arg2 != nullptr)
        {
            write(out,
                  "error: usage set_mode <BFC_TUNNEL_DEVICE|STANDALONE>\n");
            return;
        }
        if (!frameSetMode(mode))
        {
            write(out, "error: failed to set mode\n");
            return;
        }
        write(out, "ok\n");
        return;
    }

    if (cmd_is(cmd, "unset_upstream_rx", "uur"))
    {
        uint8_t airport[6] = {};
        if (arg1 != nullptr)
        {
            if (!parse_airport(arg1, airport) || arg2 != nullptr || arg3 != nullptr ||
                extra != nullptr)
            {
                write(out, "error: usage unset_upstream_rx [airport]\n");
                return;
            }
        }
        else if (arg2 != nullptr || arg3 != nullptr || extra != nullptr)
        {
            write(out, "error: usage unset_upstream_rx [airport]\n");
            return;
        }

        if (!airport_allowed(airport))
        {
            write(out, airport_mode_error());
            return;
        }
        if (!rx_->unset(airport))
        {
            write(out, "error: failed to unset upstream rx\n");
            return;
        }
        write(out, "ok\n");
        return;
    }

    if (cmd_is(cmd, "unset_upstream_tx", "uut"))
    {
        uint8_t airport[6] = {};
        if (arg1 != nullptr)
        {
            if (!parse_airport(arg1, airport) || arg2 != nullptr || arg3 != nullptr ||
                extra != nullptr)
            {
                write(out, "error: usage unset_upstream_tx [airport]\n");
                return;
            }
        }
        else if (arg2 != nullptr || arg3 != nullptr || extra != nullptr)
        {
            write(out, "error: usage unset_upstream_tx [airport]\n");
            return;
        }

        if (!airport_allowed(airport))
        {
            write(out, airport_mode_error());
            return;
        }
        if (!tx_->unset(airport))
        {
            write(out, "error: failed to unset upstream tx\n");
            return;
        }
        write(out, "ok\n");
        return;
    }

    if (cmd_is(cmd, "set_upstream_rx", "sur"))
    {
        uint8_t airport[6] = {};
        uint16_t port = 0;
        if (arg2 == nullptr)
        {
            if (!parse_port(arg1, &port) || extra != nullptr)
            {
                write(out, "error: usage set_upstream_rx [airport] <port>\n");
                return;
            }
        }
        else if (!parse_airport(arg1, airport) || !parse_port(arg2, &port) ||
                 arg3 != nullptr)
        {
            write(out, "error: usage set_upstream_rx [airport] <port>\n");
            return;
        }
        if (!airport_allowed(airport))
        {
            write(out, airport_mode_error());
            return;
        }
        if (!netmgr_->connected())
        {
            write(out, "error: ethernet is down\n");
            return;
        }
        if (!rx_->set(airport, port))
        {
            write(out, "error: failed to bind udp port\n");
            return;
        }
        write(out, "ok\n");
        return;
    }

    if (cmd_is(cmd, "set_upstream_tx", "sut"))
    {
        uint8_t airport[6] = {};
        uint32_t host = 0;
        uint16_t port = 0;
        if (arg3 == nullptr)
        {
            if (!parse_host(arg1, &host) || !parse_port(arg2, &port) ||
                extra != nullptr)
            {
                write(out,
                      "error: usage set_upstream_tx [airport] <host> <port>\n");
                return;
            }
        }
        else if (!parse_airport(arg1, airport) || !parse_host(arg2, &host) ||
                 !parse_port(arg3, &port) || extra != nullptr)
        {
            write(out,
                  "error: usage set_upstream_tx [airport] <host> <port>\n");
            return;
        }
        if (!airport_allowed(airport))
        {
            write(out, airport_mode_error());
            return;
        }
        if (!netmgr_->connected())
        {
            write(out, "error: ethernet is down\n");
            return;
        }
        if (!tx_->set(airport, host, port))
        {
            write(out, "error: failed to set tx destination\n");
            return;
        }
        write(out, "ok\n");
        return;
    }

    if (cmd_is(cmd, "set_allow_failed_crc", "saf"))
    {
        bool allow = false;
        if (!parse_bool(arg1, &allow) || arg2 != nullptr)
        {
            write(out, "error: usage set_allow_failed_crc <0|1>\n");
            return;
        }
        if (!wifi::instance().set_allow_failed_crc(allow))
        {
            write(out, "error: failed to set allow_failed_crc\n");
            return;
        }
        write(out, "ok\n");
        return;
    }

    if (cmd_is(cmd, "set_channel", "sc"))
    {
        uint8_t channel = 0;
        if (!parse_channel(arg1, &channel) || arg2 != nullptr)
        {
            write(out, "error: usage set_channel <1-13>\n");
            return;
        }
        if (!wifi::instance().set_channel(channel))
        {
            write(out, "error: failed to set channel\n");
            return;
        }
        write(out, "ok\n");
        return;
    }

    if (cmd_is(cmd, "set_modulation", "sd"))
    {
        if (arg1 == nullptr || arg2 != nullptr)
        {
            write(out, "error: usage set_modulation <modulation>\n");
            return;
        }
        if (!wifi::instance().set_modulation(arg1))
        {
            write(out, "error: unknown modulation\n");
            return;
        }
        write(out, "ok\n");
        return;
    }

    if (cmd_is(cmd, "set_cca_enabled", "sce"))
    {
        bool enabled = false;
        if (!parse_bool(arg1, &enabled) || arg2 != nullptr)
        {
            write(out, "error: usage set_cca_enabled <0|1>\n");
            return;
        }
        if (!wifi::instance().set_cca_enabled(enabled))
        {
            write(out, "error: failed to set cca\n");
            return;
        }
        write(out, "ok\n");
        return;
    }

    if (cmd_is(cmd, "set_tx_power", "stp"))
    {
        int8_t dbm = 0;
        if (!parse_tx_power(arg1, &dbm) || arg2 != nullptr)
        {
            write(out, "error: usage set_tx_power <2-20>\n");
            return;
        }
        if (!wifi::instance().set_tx_power(dbm))
        {
            write(out, "error: failed to set tx power\n");
            return;
        }
        write(out, "ok\n");
        return;
    }

    if (cmd_is(cmd, "set_network", "sn"))
    {
        NetmgrMode mode = NETMGR_MODE_AUTO;
        if (!manager::parse_network_mode(arg1, &mode) || arg2 != nullptr)
        {
            write(out, "error: usage set_network <STATIC|AUTO>\n");
            return;
        }
        if (!netmgr_->set_network_mode(mode))
        {
            write(out, "error: failed to set network mode\n");
            return;
        }
        write(out, "ok\n");
        return;
    }

    if (cmd_is(cmd, "set_enable_dhcp_server", "sed"))
    {
        bool enabled = false;
        if (!parse_bool(arg1, &enabled) || arg2 != nullptr)
        {
            write(out, "error: usage set_enable_dhcp_server <0|1>\n");
            return;
        }
        if (!netmgr_->set_dhcp_server_enabled(enabled))
        {
            write(out, "error: failed to set dhcp server\n");
            return;
        }
        if (enabled && netmgr_->network_mode() == NETMGR_MODE_AUTO)
        {
            write(out, "ok (blocked in AUTO)\n");
            return;
        }
        write(out, "ok\n");
        return;
    }

    if (cmd_is(cmd, "set_ip", "sfi") || cmd_is(cmd, "set_fallback_ip", "sfi"))
    {
        struct in_addr addr = {};
        if (arg1 == nullptr || arg2 != nullptr ||
            inet_pton(AF_INET, arg1, &addr) != 1)
        {
            write(out, "error: usage set_ip <a.b.c.d>\n");
            return;
        }
        if (!netmgr_->set_ip(addr.s_addr))
        {
            write(out, "error: host must be 1-254 on a unicast /24\n");
            return;
        }
        write(out, "ok\n");
        return;
    }

    if (cmd_is(cmd, "save", "sv"))
    {
        uint8_t slot = settingsCurrentSlot();
        if (arg1 != nullptr)
        {
            char* end = nullptr;
            const long value = strtol(arg1, &end, 10);
            if (end == arg1 || *end != '\0' || value < 0 ||
                value >= SETTINGS_SLOT_COUNT || arg2 != nullptr)
            {
                write(out, "error: usage save [0-9]\n");
                return;
            }
            slot = static_cast<uint8_t>(value);
        }
        if (!settingsSave(slot, *rx_, *tx_, *netmgr_))
        {
            write(out, "error: failed to save slot\n");
            return;
        }
        write(out, "ok\n");
        return;
    }

    if (cmd_is(cmd, "use", "u"))
    {
        char* end = nullptr;
        const long value = arg1 == nullptr ? -1 : strtol(arg1, &end, 10);
        if (arg1 == nullptr || end == arg1 || *end != '\0' || value < 0 ||
            value >= SETTINGS_SLOT_COUNT || arg2 != nullptr)
        {
            write(out, "error: usage use <0-9>\n");
            return;
        }
        if (!settingsUse(static_cast<uint8_t>(value), *rx_, *tx_, *netmgr_))
        {
            write(out, "error: slot empty\n");
            return;
        }
        write(out, "ok\n");
        return;
    }

    write(out, "error: unknown command, type help\n");
}

void console::feed_char(char c, char* line, size_t* len, size_t max_len,
                        const out_s& out)
{
    if (c == '\r')
    {
        return;
    }
    if (c == '\n')
    {
        line[*len] = '\0';
        *len = 0;
        handle_line(line, out);
        return;
    }
    if (*len + 1 >= max_len)
    {
        *len = 0;
        write(out, "error: line too long\n");
        return;
    }
    line[(*len)++] = c;
}

void console::start_tcp()
{
    if (listen_fd_ >= 0)
    {
        return;
    }

    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0)
    {
        ESP_LOGE(TAG, "socket failed: %d", errno);
        return;
    }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(CONTROL_CONSOLE_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        ESP_LOGE(TAG, "bind *:%u failed: %d", CONTROL_CONSOLE_PORT, errno);
        close(fd);
        return;
    }
    if (listen(fd, static_cast<int>(k_max_tcp_clients)) < 0 ||
        !set_nonblock(fd))
    {
        ESP_LOGE(TAG, "listen failed: %d", errno);
        close(fd);
        return;
    }

    listen_fd_ = fd;
    if (reactor_ != nullptr)
    {
        reactor_->add_read_rdy(listen_fd_,
                               [this]()
                               {
                                   accept_clients();
                               });
    }

    uint32_t ip = 0;
    if (netmgr_->local_ipv4(&ip))
    {
        char ip_str[16];
        ipv4_to_string(ip, ip_str, sizeof(ip_str));
        ESP_LOGI(TAG, "control console on %s:%u", ip_str, CONTROL_CONSOLE_PORT);
    }
}

bool console::attach_client(int fd)
{
    if (fd < 0)
    {
        return false;
    }
    for (tcp_client_s& client : clients_)
    {
        if (client.fd < 0)
        {
            client.fd = fd;
            client.len = 0;
            return true;
        }
    }
    return false;
}

void console::accept_clients()
{
    for (;;)
    {
        struct sockaddr_in peer = {};
        socklen_t peer_len = sizeof(peer);
        const int fd = accept(
            listen_fd_, reinterpret_cast<struct sockaddr*>(&peer), &peer_len);
        if (fd < 0)
        {
            return;
        }

        if (client_count() >= k_max_tcp_clients)
        {
            close(fd);
            continue;
        }

        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        if (!set_nonblock(fd) || !attach_client(fd))
        {
            close(fd);
            continue;
        }
        watch_client(fd);
        print_banner(out_s{fd});
    }
}

void console::watch_client(int fd)
{
    if (reactor_ == nullptr || fd < 0)
    {
        return;
    }
    reactor_->add_read_rdy(fd,
                           [this, fd]()
                           {
                               poll_client(fd);
                           });
}

void console::poll_client(int fd)
{
    tcp_client_s* client = client_by_fd(fd);
    if (client == nullptr)
    {
        return;
    }

    char buf[64];
    const out_s out{fd};
    while (client_by_fd(fd) != nullptr)
    {
        const int n = recv(fd, buf, sizeof(buf), 0);
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            {
                return;
            }
            close_client(fd);
            return;
        }
        if (n == 0)
        {
            close_client(fd);
            return;
        }
        for (int i = 0; i < n && client_by_fd(fd) != nullptr; i++)
        {
            client = client_by_fd(fd);
            feed_char(buf[i], client->line, &client->len, sizeof(client->line),
                      out);
        }
    }
}

void console::sync_tcp()
{
    if (netmgr_ == nullptr)
    {
        return;
    }
    if (netmgr_->connected())
    {
        start_tcp();
    }
    else
    {
        stop_tcp();
    }
}

void console::schedule_sync()
{
    if (reactor_ == nullptr)
    {
        return;
    }
    reactor_->get_timer().wait_ms(k_sync_ms,
                                  [this]()
                                  {
                                      sync_tcp();
                                      schedule_sync();
                                  });
}

void console::attach_reactor()
{
    schedule_sync();
    sync_tcp();
}

bool console::init(upstream_rx& rx, upstream_tx& tx, manager& netmgr)
{
    if (ready_)
    {
        return true;
    }

    rx_ = &rx;
    tx_ = &tx;
    netmgr_ = &netmgr;
    reactor_ = &netmgr.reactor();
    reactor_->wake_up(
        [this]()
        {
            attach_reactor();
        });

    ready_ = true;
    return true;
}
