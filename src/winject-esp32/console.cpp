#include "console.h"

#include "channel_info_endpoint.h"
#include "config.h"
#include "frame.h"
#include "lc_rx_endpoint.h"
#include "lc_tx_endpoint.h"
#include "ota.h"
#include "packet.h"
#include "settings.h"
#include "udp_logger.h"
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

static bool parse_packed_hex8(const char* text, uint8_t* value)
{
    if (text == nullptr || value == nullptr || *text == '\0')
    {
        return false;
    }
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
    {
        text += 2;
    }
    const size_t n = strlen(text);
    if (n < 1 || n > 2)
    {
        return false;
    }
    char* end = nullptr;
    const long v = strtol(text, &end, 16);
    if (end != text + n || v < 0 || v > 255)
    {
        return false;
    }
    *value = static_cast<uint8_t>(v);
    return true;
}

static bool parse_bus_arg(const char* text, bus_t* bus)
{
    if (text == nullptr || bus == nullptr)
    {
        return false;
    }
    if (strncasecmp(text, "bus=", 4) != 0)
    {
        return false;
    }
    return parse_packed_hex8(text + 4, bus);
}

static bool parse_domain(const char* text, uint16_t* domain)
{
    if (text == nullptr || domain == nullptr || *text == '\0')
    {
        return false;
    }
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
    {
        text += 2;
    }
    char* end = nullptr;
    const unsigned long value = strtoul(text, &end, 16);
    if (end == text || *end != '\0' || value < 1 || value > 65535)
    {
        return false;
    }
    *domain = static_cast<uint16_t>(value);
    return true;
}

static bool parse_to_arg(const char* text, ip_port_t* dest)
{
    if (text == nullptr || dest == nullptr)
    {
        return false;
    }
    if (strncasecmp(text, "to=", 3) != 0)
    {
        return false;
    }
    const char* p = text + 3;
    const char* colon = strrchr(p, ':');
    if (colon == nullptr || colon == p)
    {
        return false;
    }
    char host[64];
    const size_t host_len = static_cast<size_t>(colon - p);
    if (host_len == 0 || host_len >= sizeof(host))
    {
        return false;
    }
    memcpy(host, p, host_len);
    host[host_len] = '\0';
    uint32_t ip = 0;
    uint16_t port = 0;
    if (!parse_host(host, &ip) || !parse_port(colon + 1, &port))
    {
        return false;
    }
    dest->host = ip;
    dest->port = port;
    return true;
}

static bool parse_address_arg(const char* text, ip_port_t* dest)
{
    if (text == nullptr || dest == nullptr)
    {
        return false;
    }
    if (strncasecmp(text, "address=", 8) != 0)
    {
        return false;
    }
    const char* p = text + 8;
    const char* colon = strrchr(p, ':');
    if (colon == nullptr || colon == p)
    {
        return false;
    }
    char host[64];
    const size_t host_len = static_cast<size_t>(colon - p);
    if (host_len == 0 || host_len >= sizeof(host))
    {
        return false;
    }
    memcpy(host, p, host_len);
    host[host_len] = '\0';
    uint32_t ip = 0;
    uint16_t port = 0;
    if (!parse_host(host, &ip) || !parse_port(colon + 1, &port))
    {
        return false;
    }
    dest->host = ip;
    dest->port = port;
    return true;
}

static bool parse_level_arg(const char* text, log_level_e* level)
{
    if (text == nullptr || level == nullptr)
    {
        return false;
    }
    if (strncasecmp(text, "level=", 6) != 0)
    {
        return false;
    }
    return udp_logger::parse_level(text + 6, level);
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
    const int64_t deadline_us = esp_timer_get_time() + 2000000;
    while (off < len)
    {
        if (client_by_fd(out.fd) == nullptr)
        {
            return;
        }
        const int n = send(out.fd, text + off, len - off, 0);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                if (esp_timer_get_time() >= deadline_us)
                {
                    close_client(out.fd);
                    return;
                }
                vTaskDelay(1);
                continue;
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

void console::print_banner(const out_s& out)
{
    write(out, "WInject-ESP32  bus/domain radio\n");
    write(out, "type help\n");
}

void console::print_help(const out_s& out)
{
    write(out, "set_mode|sm <BFC_TUNNEL_DEVICE|STANDALONE|OTA>\n");
    write(out, "  OTA reboots into network+console+HTTP update only\n");
    write(out, "set_domain|sdom <domain>\n");
    write(out, "unset_domain|udom\n");
    write(out, "set_upstream_rx|sur bus=<lcid> <host> <udp_port>\n");
    write(out, "unset_upstream_rx|uur bus=<lcid>\n");
    write(out, "set_upstream_tx|sut bus=<lcid> <udp_port>\n");
    write(out, "unset_upstream_tx|uut bus=<lcid>\n");
    write(out, "set_upstream_ci|suc to=<host>:<port>\n");
    write(out, "unset_upstream_ci|usuc to=<host>:<port>\n");
    write(out, "set_logger|sl address=<host>:<port> level=<error|warn|info|debug>\n");
    write(out, "unset_logger|ul\n");
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
    write(out, "reset|r\n");
    write(out, "help\n");
    write(out, "modulations: ");
    write(out, wifi::modulation_list());
    write(out, "\n");
    write(out, "ota: HTTP POST /update\n");
}

void console::print_upstreams(const out_s& out, const lc_tx_bind_s* sut,
                              uint8_t sut_count, const lc_rx_bind_s* sur,
                              uint8_t sur_count)
{
    for (uint8_t i = 0; i < sut_count; i++)
    {
        print(out, "upstream_tx bus=%02X address=%u\n", sut[i].bus,
              sut[i].udp_port);
    }
    for (uint8_t i = 0; i < sur_count; i++)
    {
        char host_str[16];
        ipv4_to_string(sur[i].dest.host, host_str, sizeof(host_str));
        print(out, "upstream_rx bus=%02X address=%s:%u\n", sur[i].bus,
              host_str, sur[i].dest.port);
    }
    if (sut_count == 0 && sur_count == 0)
    {
        write(out, "upstream unset\n");
    }
}

void console::print_channel_metrics(const out_s& out,
                                    const wifi_status_s& radio,
                                    const lc_tx_bind_s* sut, uint8_t sut_count,
                                    const lc_rx_bind_s* sur, uint8_t sur_count)
{
    char tx_latency[16] = "-";
    if (radio.tx_latency_valid)
    {
        snprintf(tx_latency, sizeof(tx_latency), "%uus",
                 (unsigned)radio.tx_latency_us);
    }
    char inject_wait[16] = "-";
    if (radio.inject_wait_valid)
    {
        snprintf(inject_wait, sizeof(inject_wait), "%uus",
                 (unsigned)radio.inject_wait_us);
    }
    print(out,
          "channel_tx queue=%u drop_nomem=%u retry_count=%u "
          "retry_nomem=%u retry_other=%u inject_ok=%u inject_fail=%u "
          "in_flight=%u tx_latency=%s inject_wait=%s\n",
          (unsigned)radio.tx_queue, (unsigned)radio.drop_tx_nomem,
          (unsigned)radio.tx_retry_count, (unsigned)radio.tx_retry_nomem,
          (unsigned)radio.tx_retry_other, (unsigned)radio.inject_ok,
          (unsigned)radio.inject_fail, (unsigned)radio.tx_in_flight,
          tx_latency, inject_wait);
    print(out,
          "channel_rx queue=%u drop_no_pkt_pool=%u drop_queue_full=%u "
          "drop_crc_error=%u\n",
          (unsigned)radio.rx_queue, (unsigned)radio.drop_rx_no_pkt_pool,
          (unsigned)radio.drop_rx_queue_full, (unsigned)radio.drop_crc_error);

    for (uint8_t i = 0; i < sut_count; i++)
    {
        print(out,
              "channels_tx bus=%02X drop_no_pkt_pool=%u drop_queue_full=%u\n",
              sut[i].bus, (unsigned)sut[i].drop_no_pkt_pool,
              (unsigned)sut[i].drop_queue_full);
    }
    for (uint8_t i = 0; i < sur_count; i++)
    {
        print(out, "channels_rx bus=%02X drop_send_fail=%u\n", sur[i].bus,
              (unsigned)sur[i].drop_send_fail);
    }
}

bool console::radio_ready() const
{
    return tx_ep_ != nullptr && rx_ep_ != nullptr && ci_ != nullptr;
}

bool console::require_radio(const out_s& out)
{
    if (radio_ready())
    {
        return true;
    }
    write(out, "error: unavailable in OTA mode\n");
    return false;
}

void console::reboot_after_ok(const out_s& out)
{
    write(out, "ok\n");
    ESP_LOGI(TAG, "mode change reboot");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

void console::print_status(const out_s& out)
{
    wifi_status_s radio = {};
    static lc_tx_bind_s sut[WIFI_AIRPORT_MAX];
    static lc_rx_bind_s sur[WIFI_AIRPORT_MAX];
    static ip_port_t ci[WIFI_AIRPORT_MAX];
    uint8_t sut_count = 0;
    uint8_t sur_count = 0;
    uint8_t ci_count = 0;
    const bool have_radio = radio_ready();
    if (have_radio)
    {
        sut_count = WIFI_AIRPORT_MAX;
        wifi::instance().get_status(&radio);
        tx_ep_->get_status(sut, &sut_count);
        rx_ep_->fill_status(sur, &sur_count);
        ci_->fill_status(ci, &ci_count);
    }

    write(out, "# device\n");
    const size_t heap_total = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    const size_t heap_free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const size_t heap_usage =
        heap_total > heap_free ? heap_total - heap_free : 0;
    char domain_str[8] = "unset";
    if (have_radio && radio.domain != 0)
    {
        snprintf(domain_str, sizeof(domain_str), "%04X", radio.domain);
    }
    print(out,
          "device uptime=%lus reset_reason=%s heap_usage=%u heap_free=%u "
          "mode=%s domain=%s\n",
          (unsigned long)(esp_timer_get_time() / 1000000),
          reset_reason_name(esp_reset_reason()), (unsigned)heap_usage,
          (unsigned)heap_free, frameModeName(frameGetMode()), domain_str);

    write(out, "# network\n");
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

    if (!have_radio)
    {
        write(out, "# radio\n");
        write(out, "radio disabled (OTA mode)\n");
        return;
    }

    write(out, "# wifi\n");
    print(out,
          "wifi channel=%u modulation=%s cca=%s tx_power=%d "
          "allow_failed_crc=%s\n",
          radio.channel,
          radio.modulation != nullptr ? radio.modulation : "-",
          radio.cca_enabled ? "enabled" : "disabled", radio.tx_power_dbm,
          radio.allow_failed_crc ? "true" : "false");

    write(out, "# radio\n");
    if (radio.rx_air_valid)
    {
        print(out, "radio rssi=%d snr=%d\n", radio.rx_rssi, radio.rx_snr);
    }
    else
    {
        write(out, "radio rssi=- snr=-\n");
    }

    write(out, "# upstreams\n");
    print_upstreams(out, sut, sut_count, sur, sur_count);

    write(out, "# channel\n");
    print_channel_metrics(out, radio, sut, sut_count, sur, sur_count);

    write(out, "# logger\n");
    {
        udp_logger_status_s log = {};
        udp_logger::instance().fill_status(&log);
        if (!log.set)
        {
            write(out, "logger unset\n");
        }
        else
        {
            char host_str[16];
            ipv4_to_string(log.dest.host, host_str, sizeof(host_str));
            print(out,
                  "logger address=%s:%u level=%s emitted=%u dropped=%u\n",
                  host_str, log.dest.port, udp_logger::level_name(log.level),
                  (unsigned)log.emitted, (unsigned)log.dropped);
        }
    }

    write(out, "# channel_info\n");
    if (ci_count == 0)
    {
        write(out, "channel_info subscribers=-\n");
    }
    else
    {
        char line[256];
        size_t used = 0;
        int n = snprintf(line, sizeof(line), "channel_info subscribers=");
        if (n > 0)
        {
            used = static_cast<size_t>(n);
        }
        for (uint8_t i = 0; i < ci_count; i++)
        {
            char host_str[16];
            ipv4_to_string(ci[i].host, host_str, sizeof(host_str));
            n = snprintf(line + used, sizeof(line) - used, "%s%s:%u",
                         i == 0 ? "" : ",", host_str, ci[i].port);
            if (n < 0 || static_cast<size_t>(n) >= sizeof(line) - used)
            {
                break;
            }
            used += static_cast<size_t>(n);
        }
        snprintf(line + used, sizeof(line) - used, "\n");
        write(out, line);
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
                  "error: usage set_mode <BFC_TUNNEL_DEVICE|STANDALONE|OTA>\n");
            return;
        }
        const WinjectMode prev = settings::instance().configured_mode();
        if (prev == WINJECT_MODE_OTA || mode == WINJECT_MODE_OTA)
        {
            if (!settings::instance().persist_mode(mode))
            {
                write(out, "error: failed to set mode\n");
                return;
            }
            reboot_after_ok(out);
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

    if (cmd_is(cmd, "set_domain", "sdom"))
    {
        if (!require_radio(out))
        {
            return;
        }
        uint16_t domain = 0;
        if (!parse_domain(arg1, &domain) || arg2 != nullptr)
        {
            write(out, "error: usage set_domain <domain>\n");
            return;
        }
        if (!wifi::instance().set_domain(domain))
        {
            write(out, "error: failed to set domain\n");
            return;
        }
        write(out, "ok\n");
        return;
    }

    if (cmd_is(cmd, "unset_domain", "udom"))
    {
        if (!require_radio(out))
        {
            return;
        }
        if (arg1 != nullptr)
        {
            write(out, "error: usage unset_domain\n");
            return;
        }
        if (!wifi::instance().set_domain(0))
        {
            write(out, "error: failed to unset domain\n");
            return;
        }
        write(out, "ok\n");
        return;
    }

    if (cmd_is(cmd, "unset_upstream_rx", "uur"))
    {
        if (!require_radio(out))
        {
            return;
        }
        bus_t bus = 0;
        if (!parse_bus_arg(arg1, &bus) || arg2 != nullptr)
        {
            write(out, "error: usage unset_upstream_rx bus=<lcid>\n");
            return;
        }
        if (!rx_ep_->rem_endpoint(bus))
        {
            write(out, "error: failed to unset upstream rx\n");
            return;
        }
        write(out, "ok\n");
        return;
    }

    if (cmd_is(cmd, "unset_upstream_tx", "uut"))
    {
        if (!require_radio(out))
        {
            return;
        }
        bus_t bus = 0;
        if (!parse_bus_arg(arg1, &bus) || arg2 != nullptr)
        {
            write(out, "error: usage unset_upstream_tx bus=<lcid>\n");
            return;
        }
        if (!tx_ep_->rem_endpoint(bus))
        {
            write(out, "error: failed to unset upstream tx\n");
            return;
        }
        write(out, "ok\n");
        return;
    }

    if (cmd_is(cmd, "set_upstream_rx", "sur"))
    {
        if (!require_radio(out))
        {
            return;
        }
        bus_t bus = 0;
        uint32_t host = 0;
        uint16_t port = 0;
        if (!parse_bus_arg(arg1, &bus) || !parse_host(arg2, &host) ||
            !parse_port(arg3, &port) || extra != nullptr)
        {
            write(out,
                  "error: usage set_upstream_rx bus=<lcid> <host> <udp_port>\n");
            return;
        }
        ip_port_t dest = {host, port};
        if (!rx_ep_->add_endpoint(bus, dest))
        {
            write(out, "error: failed to set upstream rx\n");
            return;
        }
        write(out, "ok\n");
        return;
    }

    if (cmd_is(cmd, "set_upstream_tx", "sut"))
    {
        if (!require_radio(out))
        {
            return;
        }
        bus_t bus = 0;
        uint16_t port = 0;
        if (!parse_bus_arg(arg1, &bus) || !parse_port(arg2, &port) ||
            arg3 != nullptr)
        {
            write(out, "error: usage set_upstream_tx bus=<lcid> <udp_port>\n");
            return;
        }
        if (!tx_ep_->add_endpoint(bus, port))
        {
            write(out, "error: failed to bind udp port\n");
            return;
        }
        write(out, "ok\n");
        return;
    }

    if (cmd_is(cmd, "set_upstream_ci", "suc"))
    {
        if (!require_radio(out))
        {
            return;
        }
        ip_port_t dest = {};
        if (!parse_to_arg(arg1, &dest) || arg2 != nullptr)
        {
            write(out, "error: usage set_upstream_ci to=<host>:<port>\n");
            return;
        }
        if (!ci_->add_subscriber(dest))
        {
            write(out, "error: failed to add ci subscriber\n");
            return;
        }
        write(out, "ok\n");
        return;
    }

    if (cmd_is(cmd, "unset_upstream_ci", "usuc"))
    {
        if (!require_radio(out))
        {
            return;
        }
        ip_port_t dest = {};
        if (!parse_to_arg(arg1, &dest) || arg2 != nullptr)
        {
            write(out, "error: usage unset_upstream_ci to=<host>:<port>\n");
            return;
        }
        if (!ci_->rem_subscriber(dest))
        {
            write(out, "error: ci subscriber not found\n");
            return;
        }
        write(out, "ok\n");
        return;
    }

    if (cmd_is(cmd, "set_logger", "sl"))
    {
        ip_port_t dest = {};
        log_level_e level = log_level_e::warn;
        bool have_addr = false;
        bool have_level = false;
        const char* args[3] = {arg1, arg2, arg3};
        for (const char* a : args)
        {
            if (a == nullptr)
            {
                continue;
            }
            if (parse_address_arg(a, &dest))
            {
                have_addr = true;
                continue;
            }
            if (parse_level_arg(a, &level))
            {
                have_level = true;
                continue;
            }
            write(out,
                  "error: usage set_logger address=<host>:<port> "
                  "level=<error|warn|info|debug>\n");
            return;
        }
        if (!have_addr || !have_level || extra != nullptr)
        {
            write(out,
                  "error: usage set_logger address=<host>:<port> "
                  "level=<error|warn|info|debug>\n");
            return;
        }
        if (!udp_logger::instance().set(dest, level))
        {
            write(out, "error: set_logger failed\n");
            return;
        }
        write(out, "ok\n");
        return;
    }

    if (cmd_is(cmd, "unset_logger", "ul"))
    {
        if (arg1 != nullptr)
        {
            write(out, "error: usage unset_logger\n");
            return;
        }
        if (!udp_logger::instance().unset())
        {
            write(out, "error: unset_logger failed\n");
            return;
        }
        write(out, "ok\n");
        return;
    }

    if (cmd_is(cmd, "set_allow_failed_crc", "saf"))
    {
        if (!require_radio(out))
        {
            return;
        }
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
        if (!require_radio(out))
        {
            return;
        }
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
        if (!require_radio(out))
        {
            return;
        }
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
        if (!require_radio(out))
        {
            return;
        }
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
        if (!require_radio(out))
        {
            return;
        }
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
        uint8_t slot = settings::instance().current_slot();
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
        if (!settings::instance().save(slot))
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
        const WinjectMode prev = settings::instance().configured_mode();
        if (!settings::instance().use(static_cast<uint8_t>(value)))
        {
            write(out, "error: slot empty\n");
            return;
        }
        const WinjectMode next = settings::instance().configured_mode();
        if (prev == WINJECT_MODE_OTA || next == WINJECT_MODE_OTA)
        {
            reboot_after_ok(out);
            return;
        }
        write(out, "ok\n");
        return;
    }

    if (cmd_is(cmd, "reset", "r"))
    {
        if (arg1 != nullptr)
        {
            write(out, "error: usage reset\n");
            return;
        }
        write(out, "ok\n");
        ESP_LOGI(TAG, "reset requested");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
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
        // Keep enough room for a full status dump even when heap is tight.
        const int sndbuf = 4096;
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
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

bool console::init(manager& netmgr)
{
    if (ready_)
    {
        return true;
    }

    tx_ep_ = nullptr;
    rx_ep_ = nullptr;
    ci_ = nullptr;
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

bool console::init(lc_tx_endpoint& tx_ep, lc_rx_endpoint& rx_ep,
                   channel_info_endpoint& ci, manager& netmgr)
{
    if (ready_)
    {
        return true;
    }

    tx_ep_ = &tx_ep;
    rx_ep_ = &rx_ep;
    ci_ = &ci;
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
