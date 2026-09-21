#include "config.h"
#include "console.h"
#include "ether_bench.h"
#include "frame.h"
#include "ota.h"
#include "settings.h"
#include "wifi.h"
#include "wifi_bench.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "channel_info_endpoint.h"
#include "eth_dma_burst.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lc_rx_endpoint.h"
#include "lc_tx_endpoint.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "packet.h"
#include "sdkconfig.h"
#include "udp_logger.h"

static const char* TAG = "console";

namespace
{
const char* reset_reason_name(esp_reset_reason_t reason)
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

const char* chip_model_name(esp_chip_model_t model)
{
    switch (model)
    {
        case CHIP_ESP32:
            return "ESP32";
        case CHIP_ESP32S2:
            return "ESP32-S2";
        case CHIP_ESP32S3:
            return "ESP32-S3";
        case CHIP_ESP32C2:
            return "ESP32-C2";
        case CHIP_ESP32C3:
            return "ESP32-C3";
        case CHIP_ESP32C6:
            return "ESP32-C6";
        case CHIP_ESP32H2:
            return "ESP32-H2";
        case CHIP_ESP32P4:
            return "ESP32-P4";
        default:
            return "unknown";
    }
}

void format_mac(const uint8_t mac[6], char* out, size_t out_n)
{
    if (out == nullptr || out_n < 18)
    {
        return;
    }
    snprintf(out, out_n, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1],
             mac[2], mac[3], mac[4], mac[5]);
}

void ipv4_to_string(uint32_t addr, char* out, size_t out_len)
{
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&addr);
    snprintf(out, out_len, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

bool cmd_is(const char* cmd, const char* name)
{
    return strcasecmp(cmd, name) == 0;
}

bool cmd_is(const char* cmd, const char* name, const char* alias)
{
    return cmd_is(cmd, name) || cmd_is(cmd, alias);
}

bool parse_long(const char* text, int base, long min_v, long max_v, long* value)
{
    if (text == nullptr || value == nullptr || *text == '\0')
    {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const long v = strtol(text, &end, base);
    if (errno != 0 || end == text || *end != '\0' || v < min_v || v > max_v)
    {
        return false;
    }
    *value = v;
    return true;
}

bool parse_bool(const char* text, bool* value)
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

bool parse_port(const char* text, uint16_t* port)
{
    long value = 0;
    if (!parse_long(text, 10, 1, 65535, &value))
    {
        return false;
    }
    *port = static_cast<uint16_t>(value);
    return true;
}

bool parse_channel(const char* text, uint8_t* channel)
{
    long value = 0;
    if (!parse_long(text, 10, WIFI_CHANNEL_MIN, WIFI_CHANNEL_MAX, &value))
    {
        return false;
    }
    *channel = static_cast<uint8_t>(value);
    return true;
}

bool parse_tx_power(const char* text, int8_t* dbm)
{
    long value = 0;
    if (!parse_long(text, 10, WIFI_TX_POWER_DBM_MIN, WIFI_TX_POWER_DBM_MAX,
                    &value))
    {
        return false;
    }
    *dbm = static_cast<int8_t>(value);
    return true;
}

bool parse_slot(const char* text, uint8_t* slot)
{
    long value = 0;
    if (!parse_long(text, 10, 0, SETTINGS_SLOT_COUNT - 1, &value))
    {
        return false;
    }
    *slot = static_cast<uint8_t>(value);
    return true;
}

bool parse_host(const char* text, uint32_t* ip)
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

bool parse_packed_hex8(const char* text, uint8_t* value)
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
    long v = 0;
    if (!parse_long(text, 16, 0, 255, &v))
    {
        return false;
    }
    *value = static_cast<uint8_t>(v);
    return true;
}

bool parse_kv(const char* text, const char* key, const char** value)
{
    if (text == nullptr || key == nullptr || value == nullptr)
    {
        return false;
    }
    const size_t n = strlen(key);
    if (strncasecmp(text, key, n) != 0)
    {
        return false;
    }
    *value = text + n;
    return true;
}

bool parse_bus_arg(const char* text, bus_t* bus)
{
    const char* value = nullptr;
    return parse_kv(text, "bus=", &value) && parse_packed_hex8(value, bus);
}

bool parse_domain(const char* text, uint16_t* domain)
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
    errno = 0;
    const unsigned long value = strtoul(text, &end, 16);
    if (errno != 0 || end == text || *end != '\0' || value < 1 || value > 65535)
    {
        return false;
    }
    *domain = static_cast<uint16_t>(value);
    return true;
}

bool parse_host_port(const char* text, ip_port_t* dest)
{
    if (text == nullptr || dest == nullptr)
    {
        return false;
    }
    const char* colon = strrchr(text, ':');
    if (colon == nullptr || colon == text)
    {
        return false;
    }
    char host[64];
    const size_t host_len = static_cast<size_t>(colon - text);
    if (host_len == 0 || host_len >= sizeof(host))
    {
        return false;
    }
    memcpy(host, text, host_len);
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

bool parse_to_arg(const char* text, ip_port_t* dest)
{
    const char* value = nullptr;
    return parse_kv(text, "to=", &value) && parse_host_port(value, dest);
}

bool parse_address_arg(const char* text, ip_port_t* dest)
{
    const char* value = nullptr;
    return parse_kv(text, "address=", &value) && parse_host_port(value, dest);
}

bool parse_level_arg(const char* text, log_level_e* level)
{
    const char* value = nullptr;
    return parse_kv(text, "level=", &value) &&
           udp_logger::parse_level(value, level);
}

bool parse_size_arg(const char* text, size_t* size)
{
    const char* value = nullptr;
    long v = 0;
    if (!parse_kv(text, "size=", &value) ||
        !parse_long(value, 10, 1, ETHER_TEST_MAX, &v))
    {
        return false;
    }
    *size = static_cast<size_t>(v);
    return true;
}

bool parse_bench_size_arg(const char* text, size_t* size)
{
    const char* value = nullptr;
    long v = 0;
    if (!parse_kv(text, "size=", &value) ||
        !parse_long(value, 10, ETHER_BENCH_HDR, ETHER_BENCH_MAX, &v))
    {
        return false;
    }
    *size = static_cast<size_t>(v);
    return true;
}

bool parse_sn_arg(const char* text, uint32_t* sn)
{
    const char* value = nullptr;
    long v = 0;
    if (!parse_kv(text, "sn=", &value) ||
        !parse_long(value, 10, 0, 0x7fffffffL, &v))
    {
        return false;
    }
    *sn = static_cast<uint32_t>(v);
    return true;
}

void fill_ether_test(uint8_t* buf, size_t len, uint32_t sn)
{
    for (size_t i = 0; i < len; ++i)
    {
        buf[i] = static_cast<uint8_t>((sn + i) & 0xff);
    }
}

bool ether_test_matches(const uint8_t* data, size_t len, uint32_t sn)
{
    for (size_t i = 0; i < len; ++i)
    {
        if (data[i] != static_cast<uint8_t>((sn + i) & 0xff))
        {
            return false;
        }
    }
    return true;
}

// Shared TX write buffer; RX verifies against the same pattern formula.
uint8_t g_ether_test_buf[ETHER_TEST_MAX];
size_t g_ether_test_len = 0;
uint32_t g_ether_test_tx_sn = 0;
uint32_t g_ether_test_rx_last_sn = 0;
bool g_ether_test_rx_have = false;

bool starts_with_cmd(const char* buf, size_t n, const char* name)
{
    const size_t name_len = strlen(name);
    if (n < name_len)
    {
        return false;
    }
    for (size_t i = 0; i < name_len; ++i)
    {
        const char a = buf[i];
        const char b = name[i];
        const char al = (a >= 'A' && a <= 'Z') ? static_cast<char>(a + 32) : a;
        const char bl = (b >= 'A' && b <= 'Z') ? static_cast<char>(b + 32) : b;
        if (al != bl)
        {
            return false;
        }
    }
    if (n == name_len)
    {
        return true;
    }
    const char c = buf[name_len];
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}
}  // namespace

bool console::radio_ready() const
{
    return tx_ep != nullptr && rx_ep != nullptr && ci != nullptr;
}

bool console::require_radio()
{
    if (radio_ready())
    {
        return true;
    }
    reply_nok("unavailable in OTA mode");
    return false;
}

void console::reboot_after_ok()
{
    reply_ok();
    flush_reply();
    ESP_LOGI(TAG, "persisted change reboot");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

void console::print_help()
{
    write("set_mode|sm <BFC_TUNNEL_DEVICE|STANDALONE|OTA>\n");
    write("  OTA reboots into network+console+HTTP update only\n");
    write("set_domain|sdom <domain>\n");
    write("unset_domain|udom\n");
    write("set_upstream_rx|sur host=<ip> port=<port>\n");
    write("unset_upstream_rx|uur\n");
    write("set_upstream_tx|sut port=<port>\n");
    write("unset_upstream_tx|uut\n");
    write("set_upstream_ci|suc to=<host>:<port>\n");
    write("unset_upstream_ci|usuc to=<host>:<port>\n");
    write(
        "set_logger|sl address=<host>:<port> level=<error|warn|info|debug>\n");
    write("unset_logger|ul\n");
    write("set_allow_failed_crc|saf <0|1>\n");
    write("reset_promisc_stats|rps\n");
    write("set_channel|sc <channel>\n");
    write("set_modulation|sd <modulation>\n");
    write("set_cca_enabled|sce <0|1>\n");
    write("set_inject_sink|sis <wifi|null|dry>\n");
    write("set_tx_power|stp <2-20>\n");
    write("set_network|sn <STATIC|AUTO>\n");
    write("set_ip|sfi <ip>\n");
    write("save|sv [0-9]  (network/ethernet/radio/upstreams)\n");
    write("use|u <0-9>\n");
    write("status|s\n");
    write("ping  (rsp: pong)\n");
    write("ether_test_tx|ett size=<1-1400>\n");
    write("ether_test_rx|etr sn=<n> data=<binary>\n");
    write("ether_bench_tx|ebt to=<host>:<port> size=<8-1472> count=<n>\n");
    write("ether_bench_rx|ebr\n");
    write("ether_bench_stop|ebs\n");
    write("ether_bench_status|ebst\n");
    write("set_inject_tune|sit flush_batch=<1-64> emac_gap_ticks=<0-50> "
          "max_in_flight=<1-32> staging_margin=<1-15>\n");
    write("set_eth_dma_burst_len|sedl <32|16|8|4|2|1>  (reboot)\n");
    write("set_wifi_bench_defaults|swbd size=<24-1500> count=<n> kbps=<0-100000>\n");
    write("wifi_bench_tx|wbt [size=<24-1500> count=<n> kbps=<0-100000>]\n");
    write("wifi_bench_stop|wbx\n");
    write("wifi_bench_status|wbs\n");
    write("tx_grant|tg  (cd-protocol: ok <frames> inject budget)\n");
    write("reset|r\n");
    write("help\n");
    write("modulations: ");
    write(wifi::modulation_list());
    write("\n");
    write("ota: HTTP POST /update\n");
}

void console::print_upstreams(const lc_tx_bind_s* sut, bool have_sut,
                              const lc_rx_bind_s* sur, bool have_sur)
{
    if (have_sut && sut != nullptr)
    {
        print("upstream_tx port=%u\n", sut->udp_port);
    }
    if (have_sur && sur != nullptr)
    {
        char host_str[16];
        ipv4_to_string(sur->dest.host, host_str, sizeof(host_str));
        print("upstream_rx host=%s port=%u\n", host_str, sur->dest.port);
    }
}

void console::print_channel_metrics(const wifi_status_s& radio,
                                    const lc_tx_bind_s* sut, bool have_sut,
                                    const lc_rx_bind_s* sur, bool have_sur)
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
    print(
        "channel_tx queue=%u queue_hwm=%u pool_free=%u pool_free_min=%u "
        "drop_nomem=%u retry_count=%u "
        "retry_nomem=%u retry_other=%u inject_ok=%u inject_fail=%u "
        "in_flight=%u tx_latency=%s inject_wait=%s udp_tx=%u sink=%s\n",
        (unsigned)radio.tx_queue, (unsigned)radio.tx_queue_hwm,
        (unsigned)radio.tx_pool_free, (unsigned)radio.tx_pool_free_min,
        (unsigned)radio.drop_tx_nomem,
        (unsigned)radio.tx_retry_count, (unsigned)radio.tx_retry_nomem,
        (unsigned)radio.tx_retry_other, (unsigned)radio.inject_ok,
        (unsigned)radio.inject_fail, (unsigned)radio.tx_in_flight, tx_latency,
        inject_wait, (unsigned)radio.udp_tx_pkt,
        radio.inject_null_sink ? "null"
                               : (radio.inject_dry_run ? "dry" : "wifi"));
    print(
        "channel_rx queue=%u drop_no_pkt_pool=%u drop_queue_full=%u "
        "drop_crc_error=%u wifi_accept=%u udp_fwd=%u\n",
        (unsigned)radio.rx_queue, (unsigned)radio.drop_rx_no_pkt_pool,
        (unsigned)radio.drop_rx_queue_full, (unsigned)radio.drop_crc_error,
        (unsigned)radio.udp_rx_pkt, (unsigned)radio.udp_fwd_pkt);
    print(
        "promisc data=%u misc=%u misc_len=%u ctrl=%u skip_type=%u "
        "sig_ht=%u sig_legacy=%u sig_other=%u drop_ampdu=%u drop_len=%u "
        "drop_addr3=%u ht_prefix=%u ht_ok=%u leg_prefix=%u leg_ok=%u "
        "domain_word=%u\n",
        (unsigned)radio.promisc_data, (unsigned)radio.promisc_misc,
        (unsigned)radio.promisc_misc_nonempty, (unsigned)radio.promisc_ctrl,
        (unsigned)radio.promisc_skip_type, (unsigned)radio.promisc_ht,
        (unsigned)radio.promisc_legacy, (unsigned)radio.promisc_other_sig,
        (unsigned)radio.promisc_drop_ampdu, (unsigned)radio.promisc_drop_len,
        (unsigned)radio.promisc_drop_addr3, (unsigned)radio.promisc_ht_prefix,
        (unsigned)radio.promisc_ht_addr3_ok,
        (unsigned)radio.promisc_legacy_prefix,
        (unsigned)radio.promisc_legacy_addr3_ok,
        (unsigned)radio.promisc_domain_word_ok);

    lc_tx_endpoint& lct = lc_tx_endpoint::instance();
    print("inject_tune flush_batch=%u emac_gap_ticks=%u staging_margin=%u "
          "max_in_flight=%u\n",
          (unsigned)lct.flush_batch(), (unsigned)lct.emac_gap_ticks(),
          (unsigned)lct.staging_pressure_margin(),
          (unsigned)wifi::instance().tx().max_in_flight_cap());

    if (have_sut && sut != nullptr)
    {
        print("channels_tx drop_no_pkt_pool=%u drop_queue_full=%u "
              "pool_free_min=%u sink=%s\n",
              (unsigned)sut->drop_no_pkt_pool, (unsigned)sut->drop_queue_full,
              (unsigned)sut->pool_free_min,
              sut->null_sink ? "null"
                             : (radio.inject_dry_run ? "dry" : "wifi"));
    }
    if (have_sur && sur != nullptr)
    {
        print("channels_rx drop_send_fail=%u\n",
              (unsigned)sur->drop_send_fail);
    }
}

void console::print_hardware()
{
    write("# hardware\n");

    esp_chip_info_t chip = {};
    esp_chip_info(&chip);

    char rev_str[16];
    if (chip.revision >= 100)
    {
        snprintf(rev_str, sizeof(rev_str), "%u.%02u",
                 static_cast<unsigned>(chip.revision / 100),
                 static_cast<unsigned>(chip.revision % 100));
    }
    else
    {
        snprintf(rev_str, sizeof(rev_str), "%u",
                 static_cast<unsigned>(chip.revision));
    }

    char features[48];
    size_t off = 0;
    features[0] = '\0';
    const auto append = [&](const char* name) {
        const size_t n = strlen(name);
        if (off > 0 && off + 1 < sizeof(features))
        {
            features[off++] = ',';
        }
        if (off + n < sizeof(features))
        {
            memcpy(features + off, name, n);
            off += n;
            features[off] = '\0';
        }
    };
    if (chip.features & CHIP_FEATURE_WIFI_BGN)
    {
        append("wifi");
    }
    if (chip.features & CHIP_FEATURE_BT)
    {
        append("bt");
    }
    if (chip.features & CHIP_FEATURE_BLE)
    {
        append("ble");
    }
    if (chip.features & CHIP_FEATURE_EMB_FLASH)
    {
        append("emb_flash");
    }
    if (chip.features & CHIP_FEATURE_EMB_PSRAM)
    {
        append("psram");
    }
    if (features[0] == '\0')
    {
        snprintf(features, sizeof(features), "-");
    }

    uint32_t flash_bytes = 0;
    unsigned flash_mb = 0;
    if (esp_flash_get_size(nullptr, &flash_bytes) == ESP_OK && flash_bytes > 0)
    {
        flash_mb = static_cast<unsigned>((flash_bytes + (1u << 19)) >> 20);
    }

    print("hw chip=%s rev=%s cores=%u features=%s flash_mb=%u cpu_mhz=%u\n",
          chip_model_name(chip.model), rev_str,
          static_cast<unsigned>(chip.cores), features, flash_mb,
          static_cast<unsigned>(CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ));

    uint8_t mac_wifi[6] = {};
    uint8_t mac_eth[6] = {};
    char wifi_str[18] = "-";
    char eth_str[18] = "-";
    if (esp_read_mac(mac_wifi, ESP_MAC_WIFI_STA) == ESP_OK)
    {
        format_mac(mac_wifi, wifi_str, sizeof(wifi_str));
    }
    if (esp_read_mac(mac_eth, ESP_MAC_ETH) == ESP_OK)
    {
        format_mac(mac_eth, eth_str, sizeof(eth_str));
    }
    print("hw mac_wifi=%s mac_eth=%s\n", wifi_str, eth_str);

    const esp_app_desc_t* app = esp_app_get_description();
    if (app != nullptr)
    {
        print("hw app=%s ver=%s idf=%s date=%s %s\n", app->project_name,
              app->version[0] != '\0' ? app->version : "-",
              app->idf_ver[0] != '\0' ? app->idf_ver : "-", app->date,
              app->time);
    }
}

void console::print_status()
{
    wifi_status_s radio = {};
    lc_tx_bind_s sut = {};
    lc_rx_bind_s sur = {};
    static ip_port_t ci_binds[WIFI_AIRPORT_MAX];
    bool have_sut = false;
    bool have_sur = false;
    uint8_t ci_count = 0;
    const bool have_radio = radio_ready();
    if (have_radio)
    {
        wifi::instance().get_status(&radio);
        have_sut = tx_ep->get_status(&sut);
        have_sur = rx_ep->get_status(&sur);
        ci->fill_status(ci_binds, &ci_count);
        if (have_sut)
        {
            radio.inject_null_sink = sut.null_sink;
            radio.tx_pool_free_min = sut.pool_free_min;
        }
        else
        {
            radio.inject_null_sink = false;
            radio.tx_pool_free_min = radio.tx_pool_free;
        }
    }

    write("# device\n");
    const size_t heap_total = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    const size_t heap_free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const size_t heap_usage =
        heap_total > heap_free ? heap_total - heap_free : 0;
    char domain_str[8] = "unset";
    if (have_radio && radio.domain != 0)
    {
        snprintf(domain_str, sizeof(domain_str), "%04X", radio.domain);
    }
    print(
        "device uptime=%lus reset_reason=%s heap_usage=%u heap_free=%u "
        "mode=%s domain=%s\n",
        (unsigned long)(esp_timer_get_time() / 1000000),
        reset_reason_name(esp_reset_reason()), (unsigned)heap_usage,
        (unsigned)heap_free, frameModeName(frameGetMode()), domain_str);

    print_hardware();

    write("# network\n");
    uint32_t eth_ip = 0;
    const bool have_ip = netmgr->local_ipv4(&eth_ip);
    char ip_str[16] = "-";
    if (have_ip)
    {
        ipv4_to_string(eth_ip, ip_str, sizeof(ip_str));
    }
    const char* net_mode =
        netmgr->network_mode() == NETMGR_MODE_STATIC ? "static" : "dhcp";
    if (have_ip)
    {
        print("network ip=%s/24 mode=%s console_port=%u eth_dma_burst=%u\n",
              ip_str, net_mode, CONTROL_CONSOLE_PORT,
              eth_dma_burst_beats_stored());
    }
    else
    {
        print("network ip=- mode=%s console_port=%u eth_dma_burst=%u\n",
              net_mode, CONTROL_CONSOLE_PORT, eth_dma_burst_beats_stored());
    }

    if (otaActive() && have_ip)
    {
        print("ota url=http://%s:%u/update\n", ip_str, OTA_HTTP_PORT);
    }
    else
    {
        write("ota waiting\n");
    }

    if (!have_radio)
    {
        write("# radio\n");
        write("radio disabled (OTA mode)\n");
        return;
    }

    write("# wifi\n");
    print("wifi_txrx channel=%u\n", radio.channel);
    print("wifi_tx modulation=%s cca=%s tx_power=%d\n",
          radio.modulation != nullptr ? radio.modulation : "-",
          radio.cca_enabled ? "enabled" : "disabled", radio.tx_power_dbm);
    if (radio.rx_air_valid)
    {
        print("wifi_rx radio rssi=%d snr=%d allow_failed_crc=%s\n",
              radio.rx_rssi, radio.rx_snr,
              radio.allow_failed_crc ? "true" : "false");
    }
    else
    {
        print("wifi_rx radio rssi=- snr=- allow_failed_crc=%s\n",
              radio.allow_failed_crc ? "true" : "false");
    }

    if (have_sut || have_sur)
    {
        write("# upstreams\n");
        print_upstreams(have_sut ? &sut : nullptr, have_sut,
                        have_sur ? &sur : nullptr, have_sur);
    }

    write("# channel\n");
    print_channel_metrics(radio, have_sut ? &sut : nullptr, have_sut,
                          have_sur ? &sur : nullptr, have_sur);

    {
        udp_logger_status_s log = {};
        udp_logger::instance().fill_status(&log);
        if (log.set)
        {
            write("# logger\n");
            char host_str[16];
            ipv4_to_string(log.dest.host, host_str, sizeof(host_str));
            print("logger address=%s:%u level=%s emitted=%u dropped=%u\n",
                  host_str, log.dest.port, udp_logger::level_name(log.level),
                  (unsigned)log.emitted, (unsigned)log.dropped);
        }
    }

    if (ci_count == 0)
    {
        return;
    }

    write("# channel_info\n");
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
        ipv4_to_string(ci_binds[i].host, host_str, sizeof(host_str));
        n = snprintf(line + used, sizeof(line) - used, "%s%s:%u",
                     i == 0 ? "" : ",", host_str, ci_binds[i].port);
        if (n < 0 || static_cast<size_t>(n) >= sizeof(line) - used)
        {
            break;
        }
        used += static_cast<size_t>(n);
    }
    snprintf(line + used, sizeof(line) - used, "\n");
    write(line);
}

void console::handle_datagram(char* buf, size_t n)
{
    size_t off = 0;
    while (off < n && (buf[off] == ' ' || buf[off] == '\t'))
    {
        ++off;
    }
    if (off >= n)
    {
        return;
    }
    char* body = buf + off;
    const size_t body_n = n - off;

    if (starts_with_cmd(body, body_n, "ether_test_rx") ||
        starts_with_cmd(body, body_n, "etr"))
    {
        handle_ether_test_rx(body, body_n);
        return;
    }

    buf[n] = '\0';
    size_t end = n;
    while (end > off)
    {
        const char c = buf[end - 1];
        if (c != '\n' && c != '\r' && c != ' ' && c != '\t')
        {
            break;
        }
        buf[--end] = '\0';
    }
    if (buf[off] == '\0')
    {
        return;
    }
    handle_line(buf + off);
}

void console::handle_ether_test_tx(char* arg1, char* extra)
{
    size_t size = 0;
    if (!parse_size_arg(arg1, &size) || extra != nullptr)
    {
        reply_usage("ether_test_tx size=<1-1400>");
        return;
    }

    ++g_ether_test_tx_sn;
    const uint32_t sn = g_ether_test_tx_sn;
    fill_ether_test(g_ether_test_buf, size, sn);
    g_ether_test_len = size;

    char hdr[48];
    snprintf(hdr, sizeof(hdr), "ok sn=%lu data=",
             static_cast<unsigned long>(sn));
    write(hdr);
    write_bytes(g_ether_test_buf, size);
}

void console::handle_ether_test_rx(char* buf, size_t n)
{
    // ether_test_rx sn=<sn> data=<binary...>
    size_t pos = 0;
    while (pos < n && buf[pos] != ' ' && buf[pos] != '\t')
    {
        ++pos;
    }
    while (pos < n && (buf[pos] == ' ' || buf[pos] == '\t'))
    {
        ++pos;
    }

    const char* sn_tok = buf + pos;
    while (pos < n && buf[pos] != ' ' && buf[pos] != '\t')
    {
        ++pos;
    }
    if (pos >= n || sn_tok == buf + pos)
    {
        reply_usage("ether_test_rx sn=<n> data=<binary>");
        return;
    }
    const char sn_end = buf[pos];
    buf[pos] = '\0';
    uint32_t sn = 0;
    if (!parse_sn_arg(sn_tok, &sn))
    {
        buf[pos] = sn_end;
        reply_usage("ether_test_rx sn=<n> data=<binary>");
        return;
    }
    buf[pos] = sn_end;
    while (pos < n && (buf[pos] == ' ' || buf[pos] == '\t'))
    {
        ++pos;
    }

    static const char k_data_prefix[] = "data=";
    const size_t prefix_len = sizeof(k_data_prefix) - 1;
    if (pos + prefix_len > n ||
        strncasecmp(buf + pos, k_data_prefix, prefix_len) != 0)
    {
        reply_usage("ether_test_rx sn=<n> data=<binary>");
        return;
    }
    pos += prefix_len;
    const uint8_t* data = reinterpret_cast<const uint8_t*>(buf + pos);
    const size_t data_len = n - pos;

    if (data_len == 0 || data_len > ETHER_TEST_MAX)
    {
        reply_nok("bad data length");
        return;
    }
    if (!ether_test_matches(data, data_len, sn))
    {
        reply_nok("data mismatch");
        return;
    }

    // Read into the shared test buffer (mirrors TX write).
    memcpy(g_ether_test_buf, data, data_len);
    g_ether_test_len = data_len;

    uint32_t gap = 0;
    if (g_ether_test_rx_have)
    {
        if (sn > g_ether_test_rx_last_sn)
        {
            gap = sn - g_ether_test_rx_last_sn - 1;
        }
        else if (sn < g_ether_test_rx_last_sn)
        {
            // wrap or reorder: report distance past last
            gap = (0xffffffffu - g_ether_test_rx_last_sn) + sn;
        }
    }
    g_ether_test_rx_last_sn = sn;
    g_ether_test_rx_have = true;

    char args[64];
    snprintf(args, sizeof(args), "last_sn=%lu gap=%lu",
             static_cast<unsigned long>(sn),
             static_cast<unsigned long>(gap));
    reply_ok_args(args);
}

void console::handle_line(char* line)
{
    char* save = nullptr;
    char* cmd = strtok_r(line, " \t", &save);
    if (cmd == nullptr || *cmd == '\0' || cmd[0] == '#')
    {
        return;
    }

    if (cmd_is(cmd, "help", "?"))
    {
        print_help();
        return;
    }
    if (cmd_is(cmd, "status", "s"))
    {
        write("ok\n");
        print_status();
        return;
    }
    if (cmd_is(cmd, "ping"))
    {
        if (strtok_r(nullptr, " \t", &save) != nullptr)
        {
            reply_usage("ping");
            return;
        }
        write("pong\n");
        return;
    }
    if (cmd_is(cmd, "ether_test_tx", "ett"))
    {
        char* arg1 = strtok_r(nullptr, " \t", &save);
        char* extra = strtok_r(nullptr, " \t", &save);
        handle_ether_test_tx(arg1, extra);
        return;
    }
    if (cmd_is(cmd, "ether_bench_tx", "ebt"))
    {
        char* a1 = strtok_r(nullptr, " \t", &save);
        char* a2 = strtok_r(nullptr, " \t", &save);
        char* a3 = strtok_r(nullptr, " \t", &save);
        char* extra = strtok_r(nullptr, " \t", &save);
        ip_port_t dest = {};
        size_t size = 0;
        long count = -1;
        const char* args[3] = {a1, a2, a3};
        for (const char* a : args)
        {
            if (a == nullptr)
            {
                continue;
            }
            if (parse_to_arg(a, &dest))
            {
                continue;
            }
            if (parse_bench_size_arg(a, &size))
            {
                continue;
            }
            const char* value = nullptr;
            long v = 0;
            if (parse_kv(a, "count=", &value) &&
                parse_long(value, 10, 0, 0x7fffffffL, &v))
            {
                count = v;
                continue;
            }
            reply_usage(
                "ether_bench_tx to=<host>:<port> size=<8-1472> count=<n>");
            return;
        }
        if (dest.host == 0 || dest.port == 0 || size < ETHER_BENCH_HDR ||
            size > ETHER_BENCH_MAX || count < 0 || extra != nullptr)
        {
            reply_usage(
                "ether_bench_tx to=<host>:<port> size=<8-1472> count=<n>");
            return;
        }
        if (!ether_bench::instance().start_tx(dest, static_cast<uint16_t>(size),
                                              static_cast<uint32_t>(count)))
        {
            reply_nok("bench tx busy or no mem");
            return;
        }
        reply_ok();
        return;
    }
    if (cmd_is(cmd, "ether_bench_rx", "ebr"))
    {
        if (strtok_r(nullptr, " \t", &save) != nullptr)
        {
            reply_usage("ether_bench_rx");
            return;
        }
        if (!ether_bench::instance().arm_rx())
        {
            reply_nok("bench rx arm failed (no mem?)");
            return;
        }
        reply_ok();
        return;
    }
    if (cmd_is(cmd, "ether_bench_stop", "ebs"))
    {
        if (strtok_r(nullptr, " \t", &save) != nullptr)
        {
            reply_usage("ether_bench_stop");
            return;
        }
        ether_bench::instance().stop();
        reply_ok();
        return;
    }
    if (cmd_is(cmd, "ether_bench_status", "ebst"))
    {
        if (strtok_r(nullptr, " \t", &save) != nullptr)
        {
            reply_usage("ether_bench_status");
            return;
        }
        ether_bench_status_s st = {};
        ether_bench::instance().fill_status(&st);
        char args[192];
        snprintf(
            args, sizeof(args),
            "tx_running=%u rx_armed=%u size=%u tx_sent=%lu tx_fail=%lu "
            "tx_bytes=%llu tx_us=%lld rx_ok=%lu rx_bad=%lu rx_gap=%lu "
            "rx_bytes=%llu rx_last_sn=%lu",
            st.tx_running ? 1u : 0u, st.rx_armed ? 1u : 0u, st.size,
            static_cast<unsigned long>(st.tx_sent),
            static_cast<unsigned long>(st.tx_fail),
            static_cast<unsigned long long>(st.tx_bytes),
            static_cast<long long>(st.tx_elapsed_us),
            static_cast<unsigned long>(st.rx_ok),
            static_cast<unsigned long>(st.rx_bad),
            static_cast<unsigned long>(st.rx_gap),
            static_cast<unsigned long long>(st.rx_bytes),
            static_cast<unsigned long>(st.rx_last_sn));
        reply_ok_args(args);
        return;
    }
    if (cmd_is(cmd, "set_inject_tune", "sit"))
    {
        bool any = false;
        bool ok = true;
        for (char* a = strtok_r(nullptr, " \t", &save); a != nullptr;
             a = strtok_r(nullptr, " \t", &save))
        {
            const char* value = nullptr;
            long v = 0;
            lc_tx_endpoint& lct = lc_tx_endpoint::instance();
            if (parse_kv(a, "flush_batch=", &value) &&
                parse_long(value, 10, 1, 64, &v))
            {
                any = true;
                ok = ok && lct.set_flush_batch(static_cast<uint8_t>(v));
                continue;
            }
            if (parse_kv(a, "emac_gap_ticks=", &value) &&
                parse_long(value, 10, 0, 50, &v))
            {
                any = true;
                ok = ok && lct.set_emac_gap_ticks(static_cast<uint8_t>(v));
                continue;
            }
            if (parse_kv(a, "staging_margin=", &value) &&
                parse_long(value, 10, 1, LC_TX_STAGING_DEPTH - 1, &v))
            {
                any = true;
                ok = ok && lct.set_staging_pressure_margin(static_cast<uint8_t>(v));
                continue;
            }
            if (parse_kv(a, "max_in_flight=", &value) &&
                parse_long(value, 10, 1, 32, &v))
            {
                any = true;
                ok = ok &&
                     wifi::instance().tx().set_max_in_flight(
                         static_cast<uint32_t>(v));
                continue;
            }
            reply_usage("set_inject_tune flush_batch=<1-64> emac_gap_ticks=<0-50> "
                        "max_in_flight=<1-32> staging_margin=<1-15>");
            return;
        }
        if (!any)
        {
            reply_usage("set_inject_tune flush_batch=<1-64> emac_gap_ticks=<0-50> "
                        "max_in_flight=<1-32> staging_margin=<1-15>");
            return;
        }
        if (!ok)
        {
            reply_nok("inject tune out of range");
            return;
        }
        reply_ok();
        return;
    }
    if (cmd_is(cmd, "set_wifi_bench_defaults", "swbd"))
    {
        char* a1 = strtok_r(nullptr, " \t", &save);
        char* a2 = strtok_r(nullptr, " \t", &save);
        char* a3 = strtok_r(nullptr, " \t", &save);
        char* extra = strtok_r(nullptr, " \t", &save);
        long size = -1;
        long count = -1;
        long kbps = -1;
        const char* args[3] = {a1, a2, a3};
        for (const char* a : args)
        {
            if (a == nullptr)
            {
                continue;
            }
            const char* value = nullptr;
            long v = 0;
            if (parse_kv(a, "size=", &value) &&
                parse_long(value, 10, WIFI_RADIO_INJECT_MIN,
                           WIFI_RADIO_INJECT_MAX, &v))
            {
                size = v;
                continue;
            }
            if (parse_kv(a, "count=", &value) &&
                parse_long(value, 10, 1, 0x7fffffffL, &v))
            {
                count = v;
                continue;
            }
            if (parse_kv(a, "kbps=", &value) &&
                parse_long(value, 10, 0, 100000L, &v))
            {
                kbps = v;
                continue;
            }
            reply_usage("set_wifi_bench_defaults size=<24-1500> count=<n> kbps=<0-100000>");
            return;
        }
        if (size < 0 || count < 0 || kbps < 0 || extra != nullptr)
        {
            reply_usage("set_wifi_bench_defaults size=<24-1500> count=<n> kbps=<0-100000>");
            return;
        }
        if (!wifi_bench::instance().set_defaults(static_cast<uint16_t>(size),
                                               static_cast<uint32_t>(count),
                                               static_cast<uint32_t>(kbps)))
        {
            reply_nok("wifi bench defaults out of range");
            return;
        }
        reply_ok();
        return;
    }
    if (cmd_is(cmd, "wifi_bench_tx", "wbt"))
    {
        if (!require_radio())
        {
            return;
        }
        long size = -1;
        long count = -1;
        long kbps = -1;
        for (char* a = strtok_r(nullptr, " \t", &save); a != nullptr;
             a = strtok_r(nullptr, " \t", &save))
        {
            const char* value = nullptr;
            long v = 0;
            if (parse_kv(a, "size=", &value) &&
                parse_long(value, 10, WIFI_RADIO_INJECT_MIN,
                           WIFI_RADIO_INJECT_MAX, &v))
            {
                size = v;
                continue;
            }
            if (parse_kv(a, "count=", &value) &&
                parse_long(value, 10, 1, 0x7fffffffL, &v))
            {
                count = v;
                continue;
            }
            if (parse_kv(a, "kbps=", &value) &&
                parse_long(value, 10, 0, 100000L, &v))
            {
                kbps = v;
                continue;
            }
            reply_usage("wifi_bench_tx [size=<24-1500> count=<n> kbps=<0-100000>]");
            return;
        }
        uint16_t def_size = 0;
        uint32_t def_count = 0;
        uint32_t def_kbps = 0;
        wifi_bench::instance().get_defaults(&def_size, &def_count, &def_kbps);
        if (size < 0)
        {
            size = def_size;
        }
        if (count < 0)
        {
            count = static_cast<long>(def_count);
        }
        if (kbps < 0)
        {
            kbps = static_cast<long>(def_kbps);
        }
        if (!wifi_bench::instance().start_tx(static_cast<uint16_t>(size),
                                             static_cast<uint32_t>(count),
                                             static_cast<uint32_t>(kbps)))
        {
            reply_nok("wifi bench busy, no domain, or wifi down");
            return;
        }
        reply_ok();
        return;
    }
    if (cmd_is(cmd, "wifi_bench_stop", "wbx"))
    {
        if (strtok_r(nullptr, " \t", &save) != nullptr)
        {
            reply_usage("wifi_bench_stop");
            return;
        }
        wifi_bench::instance().stop();
        reply_ok();
        return;
    }
    if (cmd_is(cmd, "wifi_bench_status", "wbs"))
    {
        if (strtok_r(nullptr, " \t", &save) != nullptr)
        {
            reply_usage("wifi_bench_status");
            return;
        }
        wifi_bench_status_s st = {};
        wifi_bench::instance().fill_status(&st);
        uint16_t def_size = 0;
        uint32_t def_count = 0;
        uint32_t def_kbps = 0;
        wifi_bench::instance().get_defaults(&def_size, &def_count, &def_kbps);
        char args[220];
        snprintf(args, sizeof(args),
                 "running=%u size=%u kbps=%lu count=%lu enq_ok=%lu "
                 "enq_fail=%lu elapsed_us=%lld default_size=%u "
                 "default_count=%lu default_kbps=%lu",
                 st.running ? 1u : 0u, st.size,
                 static_cast<unsigned long>(st.kbps),
                 static_cast<unsigned long>(st.target),
                 static_cast<unsigned long>(st.enq_ok),
                 static_cast<unsigned long>(st.enq_fail),
                 static_cast<long long>(st.elapsed_us),
                 static_cast<unsigned>(def_size),
                 static_cast<unsigned long>(def_count),
                 static_cast<unsigned long>(def_kbps));
        reply_ok_args(args);
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
            reply_usage("set_mode <BFC_TUNNEL_DEVICE|STANDALONE|OTA>");
            return;
        }
        const WinjectMode prev = settings::instance().configured_mode();
        if (prev == WINJECT_MODE_OTA || mode == WINJECT_MODE_OTA)
        {
            if (!settings::instance().persist_mode(mode))
            {
                reply_nok("failed to set mode");
                return;
            }
            reboot_after_ok();
            return;
        }
        reply_done(frameSetMode(mode), "failed to set mode");
        return;
    }

    if (cmd_is(cmd, "set_domain", "sdom"))
    {
        uint16_t domain = 0;
        if (!require_radio())
        {
            return;
        }
        if (!parse_domain(arg1, &domain) || arg2 != nullptr)
        {
            reply_usage("set_domain <domain>");
            return;
        }
        reply_done(wifi::instance().set_domain(domain), "failed to set domain");
        return;
    }

    if (cmd_is(cmd, "unset_domain", "udom"))
    {
        if (!require_radio())
        {
            return;
        }
        if (arg1 != nullptr)
        {
            reply_usage("unset_domain");
            return;
        }
        reply_done(wifi::instance().set_domain(0), "failed to unset domain");
        return;
    }

    if (cmd_is(cmd, "unset_upstream_rx", "uur"))
    {
        if (!require_radio())
        {
            return;
        }
        if (arg1 != nullptr)
        {
            reply_usage("unset_upstream_rx");
            return;
        }
        reply_done(rx_ep->clear(), "failed to unset upstream rx");
        return;
    }

    if (cmd_is(cmd, "unset_upstream_tx", "uut"))
    {
        if (!require_radio())
        {
            return;
        }
        if (arg1 != nullptr)
        {
            reply_usage("unset_upstream_tx");
            return;
        }
        reply_done(tx_ep->clear(), "failed to unset upstream tx");
        return;
    }

    if (cmd_is(cmd, "set_upstream_rx", "sur"))
    {
        const char* host_v = nullptr;
        const char* port_v = nullptr;
        uint32_t host = 0;
        uint16_t port = 0;
        if (!require_radio())
        {
            return;
        }
        if (!parse_kv(arg1, "host=", &host_v) || !parse_host(host_v, &host) ||
            !parse_kv(arg2, "port=", &port_v) || !parse_port(port_v, &port) ||
            arg3 != nullptr)
        {
            reply_usage("set_upstream_rx host=<ip> port=<port>");
            return;
        }
        ip_port_t dest = {host, port};
        reply_done(rx_ep->set_endpoint(dest), "failed to set upstream rx");
        return;
    }

    if (cmd_is(cmd, "set_upstream_tx", "sut"))
    {
        const char* port_v = nullptr;
        uint16_t port = 0;
        if (!require_radio())
        {
            return;
        }
        if (!parse_kv(arg1, "port=", &port_v) || !parse_port(port_v, &port) ||
            arg2 != nullptr)
        {
            reply_usage("set_upstream_tx port=<port>");
            return;
        }
        reply_done(tx_ep->set_endpoint(port), "failed to bind udp port");
        return;
    }

    if (cmd_is(cmd, "set_upstream_ci", "suc"))
    {
        ip_port_t dest = {};
        if (!require_radio())
        {
            return;
        }
        if (!parse_to_arg(arg1, &dest) || arg2 != nullptr)
        {
            reply_usage("set_upstream_ci to=<host>:<port>");
            return;
        }
        reply_done(ci->add_subscriber(dest), "failed to add ci subscriber");
        return;
    }

    if (cmd_is(cmd, "unset_upstream_ci", "usuc"))
    {
        ip_port_t dest = {};
        if (!require_radio())
        {
            return;
        }
        if (!parse_to_arg(arg1, &dest) || arg2 != nullptr)
        {
            reply_usage("unset_upstream_ci to=<host>:<port>");
            return;
        }
        if (!ci->rem_subscriber(dest))
        {
            reply_nok("ci subscriber not found");
            return;
        }
        reply_ok();
        return;
    }

    if (cmd_is(cmd, "set_logger", "sl"))
    {
        ip_port_t dest = {};
        log_level_e level = log_level_e::warn;
        bool have_addr = false;
        bool have_level = false;
        const char* args[3] = {arg1, arg2, arg3};
        const char* logger_usage =
            "set_logger address=<host>:<port> level=<error|warn|info|debug>";
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
            reply_usage(logger_usage);
            return;
        }
        if (!have_addr || !have_level || extra != nullptr)
        {
            reply_usage(logger_usage);
            return;
        }
        reply_done(udp_logger::instance().set(dest, level),
                   "set_logger failed");
        return;
    }

    if (cmd_is(cmd, "unset_logger", "ul"))
    {
        if (arg1 != nullptr)
        {
            reply_usage("unset_logger");
            return;
        }
        reply_done(udp_logger::instance().unset(), "unset_logger failed");
        return;
    }

    if (cmd_is(cmd, "set_allow_failed_crc", "saf"))
    {
        bool allow = false;
        if (!require_radio())
        {
            return;
        }
        if (!parse_bool(arg1, &allow) || arg2 != nullptr)
        {
            reply_usage("set_allow_failed_crc <0|1>");
            return;
        }
        reply_done(wifi::instance().set_allow_failed_crc(allow),
                   "failed to set allow_failed_crc");
        return;
    }

    if (cmd_is(cmd, "reset_promisc_stats", "rps"))
    {
        if (arg1 != nullptr)
        {
            reply_usage("reset_promisc_stats");
            return;
        }
        if (!require_radio())
        {
            return;
        }
        wifi::instance().reset_promisc_stats();
        reply_ok();
        return;
    }

    if (cmd_is(cmd, "set_channel", "sc"))
    {
        uint8_t channel = 0;
        if (!require_radio())
        {
            return;
        }
        if (!parse_channel(arg1, &channel) || arg2 != nullptr)
        {
            reply_usage("set_channel <1-14>");
            return;
        }
        reply_done(wifi::instance().set_channel(channel),
                   "failed to set channel");
        return;
    }

    if (cmd_is(cmd, "set_modulation", "sd"))
    {
        if (!require_radio())
        {
            return;
        }
        if (arg1 == nullptr || arg2 != nullptr)
        {
            reply_usage("set_modulation <modulation>");
            return;
        }
        if (!wifi::instance().set_modulation(arg1))
        {
            reply_nok("unknown modulation");
            return;
        }
        reply_ok();
        return;
    }

    if (cmd_is(cmd, "set_cca_enabled", "sce"))
    {
        bool enabled = false;
        if (!require_radio())
        {
            return;
        }
        if (!parse_bool(arg1, &enabled) || arg2 != nullptr)
        {
            reply_usage("set_cca_enabled <0|1>");
            return;
        }
        reply_done(wifi::instance().set_cca_enabled(enabled),
                   "failed to set cca");
        return;
    }

    if (cmd_is(cmd, "set_inject_sink", "sis"))
    {
        if (!require_radio())
        {
            return;
        }
        if (arg1 == nullptr || arg2 != nullptr)
        {
            reply_usage("set_inject_sink <wifi|null|dry>");
            return;
        }
        bool null_sink = false;
        bool dry_run = false;
        if (strcasecmp(arg1, "null") == 0)
        {
            null_sink = true;
        }
        else if (strcasecmp(arg1, "dry") == 0)
        {
            dry_run = true;
        }
        else if (strcasecmp(arg1, "wifi") != 0)
        {
            reply_usage("set_inject_sink <wifi|null|dry>");
            return;
        }
        tx_ep->set_null_sink(null_sink);
        wifi::instance().set_tx_dry_run(dry_run);
        tx_ep->reset_pool_free_min();
        reply_ok();
        return;
    }

    if (cmd_is(cmd, "set_tx_power", "stp"))
    {
        int8_t dbm = 0;
        if (!require_radio())
        {
            return;
        }
        if (!parse_tx_power(arg1, &dbm) || arg2 != nullptr)
        {
            reply_usage("set_tx_power <2-20>");
            return;
        }
        reply_done(wifi::instance().set_tx_power(dbm),
                   "failed to set tx power");
        return;
    }

    if (cmd_is(cmd, "set_network", "sn"))
    {
        NetmgrMode mode = NETMGR_MODE_AUTO;
        if (!manager::parse_network_mode(arg1, &mode) || arg2 != nullptr)
        {
            reply_usage("set_network <STATIC|AUTO>");
            return;
        }
        reply_done(netmgr->set_network_mode(mode),
                   "failed to set network mode");
        return;
    }

    if (cmd_is(cmd, "set_ip", "sfi") || cmd_is(cmd, "set_fallback_ip", "sfi"))
    {
        struct in_addr addr = {};
        if (arg1 == nullptr || arg2 != nullptr ||
            inet_pton(AF_INET, arg1, &addr) != 1)
        {
            reply_usage("set_ip <a.b.c.d>");
            return;
        }
        reply_done(netmgr->set_ip(addr.s_addr),
                   "host must be 1-254 on a unicast /24");
        return;
    }

    if (cmd_is(cmd, "save", "sv"))
    {
        uint8_t slot = settings::instance().current_slot();
        if (arg1 != nullptr)
        {
            if (!parse_slot(arg1, &slot) || arg2 != nullptr)
            {
                reply_usage("save [0-9]");
                return;
            }
        }
        reply_done(settings::instance().save(slot), "failed to save slot");
        return;
    }

    if (cmd_is(cmd, "use", "u"))
    {
        uint8_t slot = 0;
        if (!parse_slot(arg1, &slot) || arg2 != nullptr)
        {
            reply_usage("use <0-9>");
            return;
        }
        const WinjectMode prev = settings::instance().configured_mode();
        if (!settings::instance().use(slot))
        {
            reply_nok("slot empty");
            return;
        }
        const WinjectMode next = settings::instance().configured_mode();
        if (prev == WINJECT_MODE_OTA || next == WINJECT_MODE_OTA)
        {
            reboot_after_ok();
            return;
        }
        reply_ok();
        return;
    }

    if (cmd_is(cmd, "set_eth_dma_burst_len", "sedl"))
    {
        if (arg1 == nullptr || arg2 != nullptr)
        {
            reply_usage("set_eth_dma_burst_len <32|16|8|4|2|1>");
            return;
        }
        char* end = nullptr;
        const unsigned long beats = strtoul(arg1, &end, 10);
        if (end == arg1 || *end != '\0' || beats > 255u)
        {
            reply_usage("set_eth_dma_burst_len <32|16|8|4|2|1>");
            return;
        }
        if (!eth_dma_burst_set_beats(static_cast<uint8_t>(beats)))
        {
            reply_nok("invalid burst or nvs failed");
            return;
        }
        reply_ok();
        reboot_after_ok();
        return;
    }

    if (cmd_is(cmd, "tx_grant", "tg"))
    {
        if (!require_radio())
        {
            return;
        }
        if (arg1 != nullptr)
        {
            reply_usage("tx_grant");
            return;
        }
        lc_tx_endpoint& lct = lc_tx_endpoint::instance();
        const uint8_t slots = wifi::instance().tx().compute_tx_slots();
        const uint16_t grant = slots;
        lct.issue_inject_grant(grant);
        char buf[16];
        snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(grant));
        reply_ok_args(buf);
        return;
    }

    if (cmd_is(cmd, "reset", "r"))
    {
        if (arg1 != nullptr)
        {
            reply_usage("reset");
            return;
        }
        reply_ok();
        flush_reply();
        ESP_LOGI(TAG, "reset requested");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
        return;
    }

    reply_nok("unknown command, type help");
}
