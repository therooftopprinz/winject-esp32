#include "config.h"
#include "console.h"
#include "ethernet.h"
#include "frame.h"
#include "ota.h"
#include "upstream_rx.h"
#include "upstream_tx.h"
#include "wifi.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "bfc/select_reactor.hpp"
#include "bfc/task_queue.hpp"
#include "bfc/task_reactor.hpp"
#include "esp_chip_info.h"
#include "esp_event.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "manager.h"
#include "nvs_flash.h"
#include "settings.h"

static const char* TAG = "test";
static ethernet& g_ethernet = ethernet::instance();
static manager& g_netmgr = manager::instance();
static wifi& g_wifi = wifi::instance();
static upstream_rx g_upstream_rx;
static upstream_tx g_upstream_tx;
static console g_console;

static int g_pass = 0;
static int g_fail = 0;

static const uint8_t kZeroMac[6] = {};
static const uint8_t kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static const uint8_t kYouPeer[6] = {0x02, 0x02, 0x03, 0x04, 0x05, 0x06};
static const uint8_t kPeerYou[6] = {0x04, 0x05, 0x06, 0x02, 0x02, 0x03};
static const uint8_t kBcastAirport[6] = {0x00, 0x00, 0x00, 0xAA, 0xBB, 0xCC};
static const uint8_t kTunnelBssid[6] = {WIFI_BSSID_TUNNEL};
static const uint8_t kStandaloneBssid[6] = {WIFI_BSSID_STANDALONE};

static void check(bool ok, const char* name)
{
    if (ok)
    {
        g_pass++;
        ESP_LOGI(TAG, "PASS  %s", name);
        return;
    }
    g_fail++;
    ESP_LOGE(TAG, "FAIL  %s", name);
}

static void logSummary()
{
    ESP_LOGI(TAG, "summary %d pass  %d fail", g_pass, g_fail);
}

static const char* resetReasonName(esp_reset_reason_t reason)
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
        case ESP_RST_DEEPSLEEP:
            return "deep-sleep";
        case ESP_RST_BROWNOUT:
            return "brownout";
        case ESP_RST_SDIO:
            return "sdio";
        default:
            return "unknown";
    }
}

static const char* chipModelName(esp_chip_model_t model)
{
    switch (model)
    {
        case CHIP_ESP32:
            return "ESP32";
        default:
            return "unknown";
    }
}

static void ipv4ToString(uint32_t addr, char* out, size_t outLen)
{
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&addr);
    snprintf(out, outLen, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

static bool setNonblock(int fd)
{
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
    {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static void drainWifiRx()
{
    uint8_t buf[WIFI_RADIO_MAX_FRAME];
    size_t len = 0;
    while (g_wifi.pop_rx(buf, &len, sizeof(buf)))
    {
    }
}

static bool waitReadable(int fd, int timeoutMs)
{
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    struct timeval tv = {};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    return select(fd + 1, &rfds, nullptr, nullptr, &tv) > 0;
}

static int tcpConnect(uint32_t ip, uint16_t port, int timeoutMs)
{
    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0)
    {
        return -1;
    }
    if (!setNonblock(fd))
    {
        close(fd);
        return -1;
    }

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = ip;
    const int err =
        connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (err == 0)
    {
        return fd;
    }
    if (errno != EINPROGRESS)
    {
        close(fd);
        return -1;
    }

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(fd, &wfds);
    struct timeval tv = {};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    if (select(fd + 1, nullptr, &wfds, nullptr, &tv) <= 0)
    {
        close(fd);
        return -1;
    }
    int soErr = 0;
    socklen_t soLen = sizeof(soErr);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soErr, &soLen) != 0 || soErr != 0)
    {
        close(fd);
        return -1;
    }
    return fd;
}

static bool recvContains(int fd, const char* needle, int timeoutMs)
{
    char acc[512] = {};
    size_t used = 0;
    const int64_t deadline =
        esp_timer_get_time() + static_cast<int64_t>(timeoutMs) * 1000;
    while (esp_timer_get_time() < deadline)
    {
        const int remainMs =
            static_cast<int>((deadline - esp_timer_get_time()) / 1000);
        if (remainMs <= 0)
        {
            break;
        }
        if (!waitReadable(fd, remainMs > 200 ? 200 : remainMs))
        {
            continue;
        }
        char chunk[128];
        const int n = recv(fd, chunk, sizeof(chunk), 0);
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            {
                continue;
            }
            break;
        }
        if (n == 0)
        {
            break;
        }
        const size_t copy = static_cast<size_t>(n) < sizeof(acc) - 1 - used
                                ? static_cast<size_t>(n)
                                : sizeof(acc) - 1 - used;
        memcpy(acc + used, chunk, copy);
        used += copy;
        acc[used] = '\0';
        if (strstr(acc, needle) != nullptr)
        {
            return true;
        }
        if (used + 1 >= sizeof(acc))
        {
            break;
        }
    }
    return false;
}

static bool udpSend(uint32_t ip, uint16_t port, const void* data, size_t len)
{
    const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0)
    {
        return false;
    }
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = ip;
    const int n =
        sendto(fd, data, len, 0, reinterpret_cast<struct sockaddr*>(&addr),
               sizeof(addr));
    close(fd);
    return n == static_cast<int>(len);
}

static int udpBind(uint32_t ip, uint16_t port)
{
    const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0)
    {
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (!setNonblock(fd))
    {
        close(fd);
        return -1;
    }
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = ip;
    if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        close(fd);
        return -1;
    }
    return fd;
}

static void testEthernetMac()
{
    uint8_t mac[6] = {};
    check(g_ethernet.mac(mac), "ethernet mac");
    check(memcmp(mac, kZeroMac, 6) != 0 && memcmp(mac, kBroadcast, 6) != 0,
          "ethernet mac not empty");
}

static void testWifiRadio()
{
    check(g_wifi.ready(), "wifi radio ready");

    wifi_status_s status = {};
    g_wifi.get_status(&status);
    check(status.channel == WIFI_DEFAULT_CHANNEL, "wifi default channel");
    check(status.modulation != nullptr &&
              strcasecmp(status.modulation, WIFI_DEFAULT_MODULATION) == 0,
          "wifi default modulation");
    check(status.cca_enabled, "wifi default cca");
    check(!status.allow_failed_crc, "wifi default allow_failed_crc");
    check(wifi::modulation_list() != nullptr &&
              strstr(wifi::modulation_list(), "DSS_1M_L") != nullptr,
          "wifi modulation list");

    check(!g_wifi.set_channel(0), "wifi reject channel 0");
    check(!g_wifi.set_channel(14), "wifi reject channel 14");
    check(g_wifi.set_channel(6), "wifi set channel 6");
    g_wifi.get_status(&status);
    check(status.channel == 6, "wifi channel 6 applied");
    check(g_wifi.set_channel(WIFI_DEFAULT_CHANNEL), "wifi restore channel");

    check(!g_wifi.set_modulation("NOPE"), "wifi reject modulation");
    check(g_wifi.set_modulation("ofdm_6m"), "wifi set OFDM_6M");
    g_wifi.get_status(&status);
    check(status.modulation != nullptr &&
              strcasecmp(status.modulation, "OFDM_6M") == 0,
          "wifi OFDM_6M applied");
    check(g_wifi.set_modulation(WIFI_DEFAULT_MODULATION),
          "wifi restore modulation");

    check(g_wifi.set_cca_enabled(false), "wifi cca off");
    g_wifi.get_status(&status);
    check(!status.cca_enabled, "wifi cca off applied");
    check(g_wifi.set_cca_enabled(true), "wifi restore cca");

    check(g_wifi.set_allow_failed_crc(true), "wifi allow_failed_crc on");
    g_wifi.get_status(&status);
    check(status.allow_failed_crc, "wifi allow_failed_crc on applied");
    check(g_wifi.set_allow_failed_crc(false), "wifi restore allow_failed_crc");

    check(!g_wifi.set_tx_power(1), "wifi reject tx power 1");
    check(!g_wifi.set_tx_power(21), "wifi reject tx power 21");
    check(g_wifi.set_tx_power(8), "wifi set tx power 8");
    g_wifi.get_status(&status);
    check(status.tx_power_dbm == 8, "wifi tx power 8 applied");
    check(g_wifi.set_tx_power(WIFI_DEFAULT_TX_POWER_DBM),
          "wifi restore tx power");

    check(!g_wifi.inject(nullptr, 64), "wifi inject null");
}

static void testSettings()
{
    check(!settingsSave(SETTINGS_SLOT_COUNT, g_upstream_rx, g_upstream_tx,
                        g_netmgr),
          "settings reject slot 10");
    check(!settingsUse(SETTINGS_SLOT_COUNT, g_upstream_rx, g_upstream_tx,
                       g_netmgr),
          "settings use reject slot 10");
    check(!settingsUse(9, g_upstream_rx, g_upstream_tx, g_netmgr),
          "settings empty slot 9");

    wifi_status_s before = {};
    g_wifi.get_status(&before);
    check(settingsSave(1, g_upstream_rx, g_upstream_tx, g_netmgr),
          "settings save slot 1");
    check(settingsCurrentSlot() == 1, "settings current slot 1");
    check(g_wifi.set_channel(6), "settings change channel");
    wifi_status_s mid = {};
    g_wifi.get_status(&mid);
    check(mid.channel == 6, "settings channel 6 before use");
    check(settingsUse(1, g_upstream_rx, g_upstream_tx, g_netmgr),
          "settings use slot 1");
    wifi_status_s after = {};
    g_wifi.get_status(&after);
    check(after.channel == before.channel, "settings restore channel");
    check(g_wifi.set_channel(WIFI_DEFAULT_CHANNEL),
          "settings restore default channel");
}

static void testFrame()
{
    uint8_t sta[6] = {};
    uint8_t bssid[6] = {};
    frameGetStaMac(sta);
    frameGetBssid(bssid);
    check(memcmp(sta, kZeroMac, 6) != 0, "frame sta mac");
    check(frameGetMode() == WINJECT_MODE_BFC_TUNNEL_DEVICE,
          "frame default mode");
    check(memcmp(bssid, kTunnelBssid, 6) == 0, "frame tunnel bssid");
    check(strcmp(frameModeName(WINJECT_MODE_BFC_TUNNEL_DEVICE),
                 "BFC_TUNNEL_DEVICE") == 0,
          "frame mode name tunnel");

    WinjectMode parsed = WINJECT_MODE_STANDALONE;
    check(frameParseMode("bfc_tunnel_device", &parsed) &&
              parsed == WINJECT_MODE_BFC_TUNNEL_DEVICE,
          "frame parse tunnel");
    check(frameParseMode("STANDALONE", &parsed) &&
              parsed == WINJECT_MODE_STANDALONE,
          "frame parse standalone");
    check(!frameParseMode("nope", &parsed), "frame parse reject");
    check(frameAirportIsZero(kZeroMac), "frame zero airport");
    check(!frameAirportValidStandalone(kZeroMac), "frame standalone reject 0");
    check(!frameAirportValidStandalone(kBroadcast),
          "frame standalone reject broadcast");
    check(frameAirportValidStandalone(kYouPeer), "frame standalone p2p ok");
    check(frameAirportValidStandalone(kBcastAirport),
          "frame standalone broadcast airport ok");
    const uint8_t kBadPeerHalf[6] = {0x02, 0x02, 0x03, 0x05, 0x05, 0x06};
    check(!frameAirportValidStandalone(kBadPeerHalf),
          "frame p2p reject multicast peer half");
    check(frameAirportIsBroadcastStandalone(kBcastAirport),
          "frame broadcast airport");
    check(!frameAirportIsBroadcastStandalone(kYouPeer),
          "frame p2p not broadcast");
    uint8_t swapped[6] = {};
    frameAirportSwap(kYouPeer, swapped);
    check(memcmp(swapped, kPeerYou, 6) == 0, "frame airport swap");
    uint8_t filterSa[6] = {};
    frameStandaloneFilterSa(kYouPeer, filterSa);
    check(memcmp(filterSa, kPeerYou, 6) == 0, "frame p2p filter sa");
    frameStandaloneFilterSa(kBcastAirport, filterSa);
    check(memcmp(filterSa, kBcastAirport, 6) == 0, "frame broadcast filter sa");
    check(frameStandaloneSaMatchesAirport(kPeerYou, kYouPeer),
          "frame p2p response match");
    check(!frameStandaloneSaMatchesAirport(kYouPeer, kYouPeer),
          "frame p2p no self match");
    check(frameStandaloneSaMatchesAirport(kBcastAirport, kBcastAirport),
          "frame broadcast exact match");

    const uint8_t payload[] = {'w', 'i', 'n', 'j'};
    uint8_t mpdu[WIFI_RADIO_INJECT_MAX] = {};
    const size_t n =
        frameWrap(payload, sizeof(payload), nullptr, mpdu, sizeof(mpdu));
    check(n == WIFI_HDR_LEN + sizeof(payload), "frame wrap size");
    check(mpdu[0] == 0x08 && mpdu[1] == 0x00, "frame wrap fc");
    check(memcmp(mpdu + 4, kBroadcast, 6) == 0, "frame wrap da");
    check(memcmp(mpdu + 10, sta, 6) == 0, "frame wrap sa");
    check(memcmp(mpdu + 16, kTunnelBssid, 6) == 0, "frame wrap bssid");
    check(frameBssidMatch(mpdu, n), "frame tunnel bssid match");
    check(!frameMatch(mpdu, n), "frame drop own sa");
    check(frameClassify(mpdu, n) == FRAME_RX_SELF, "frame tunnel self");

    uint8_t foreign[WIFI_RADIO_INJECT_MAX] = {};
    memcpy(foreign, mpdu, n);
    foreign[10] ^= 0x01;
    check(frameBssidMatch(foreign, n), "frame foreign bssid match");
    check(frameClassify(foreign, n) == FRAME_RX_NONE,
          "frame tunnel no dest not match");
    uint8_t tunnelDest[1][6] = {};
    frameSetPeerAirports(tunnelDest, 1);
    check(frameClassify(foreign, n) == FRAME_RX_MATCH,
          "frame tunnel dest match");
    check(frameHeard(mpdu, n), "frame tunnel heard own sa");
    check(frameHeard(foreign, n), "frame tunnel heard foreign sa");
    foreign[16] ^= 0x01;
    check(!frameBssidMatch(foreign, n), "frame wrong bssid");
    check(frameClassify(foreign, n) == FRAME_RX_NONE, "frame wrong bssid none");
    frameSetPeerAirports(nullptr, 0);
    check(frameWrap(payload, sizeof(payload), nullptr, mpdu, WIFI_HDR_LEN) == 0,
          "frame wrap short out");
    check(frameWrap(payload, WIFI_PAYLOAD_MAX + 1, nullptr, mpdu,
                    sizeof(mpdu)) == 0,
          "frame wrap too long");

    check(frameSetMode(WINJECT_MODE_STANDALONE), "frame set standalone");
    frameGetBssid(bssid);
    check(memcmp(bssid, kStandaloneBssid, 6) == 0, "frame standalone bssid");
    check(
        frameWrap(payload, sizeof(payload), kZeroMac, mpdu, sizeof(mpdu)) == 0,
        "frame wrap standalone zero sa");
    const size_t sn =
        frameWrap(payload, sizeof(payload), kYouPeer, mpdu, sizeof(mpdu));
    check(sn == WIFI_HDR_LEN + sizeof(payload), "frame wrap standalone");
    check(memcmp(mpdu + 10, kYouPeer, 6) == 0, "frame wrap standalone sa");
    check(frameBssidMatch(mpdu, sn), "frame standalone bssid match");
    check(!frameMatch(mpdu, sn), "frame standalone no peer yet");
    uint8_t peers[1][6] = {};
    memcpy(peers[0], kYouPeer, 6);
    frameSetPeerAirports(peers, 1);
    check(!frameMatch(mpdu, sn), "frame standalone p2p ignores unswapped sa");
    uint8_t reply[WIFI_RADIO_INJECT_MAX] = {};
    const size_t rn =
        frameWrap(payload, sizeof(payload), kPeerYou, reply, sizeof(reply));
    check(rn == sn, "frame wrap p2p response");
    check(frameMatch(reply, rn), "frame standalone match swapped peer");
    check(frameHeard(reply, rn), "frame standalone heard swapped peer");
    uint8_t unknownSa[WIFI_RADIO_INJECT_MAX] = {};
    memcpy(unknownSa, reply, rn);
    unknownSa[15] ^= 0x02;
    check(!frameHeard(unknownSa, rn), "frame standalone unknown sa not heard");
    const uint8_t* body = nullptr;
    size_t bodyLen = 0;
    check(frameUnwrap(reply, rn, &body, &bodyLen), "frame unwrap");
    check(bodyLen == sizeof(payload) && memcmp(body, payload, bodyLen) == 0,
          "frame unwrap payload");
    uint8_t locals[1][6] = {};
    memcpy(locals[0], kYouPeer, 6);
    frameSetLocalAirports(locals, 1);
    check(frameClassify(mpdu, sn) == FRAME_RX_SELF, "frame standalone self");
    frameSetLocalAirports(nullptr, 0);

    const size_t bn =
        frameWrap(payload, sizeof(payload), kBcastAirport, mpdu, sizeof(mpdu));
    check(bn == sn, "frame wrap broadcast airport");
    memcpy(peers[0], kBcastAirport, 6);
    frameSetPeerAirports(peers, 1);
    check(frameMatch(mpdu, bn), "frame standalone match broadcast airport");
    uint8_t bcastSwap[6] = {};
    frameAirportSwap(kBcastAirport, bcastSwap);
    const size_t bsn =
        frameWrap(payload, sizeof(payload), bcastSwap, reply, sizeof(reply));
    check(!frameMatch(reply, bsn), "frame broadcast does not match swap");

    check(frameSetMode(WINJECT_MODE_BFC_TUNNEL_DEVICE), "frame restore tunnel");
    frameSetPeerAirports(nullptr, 0);
}

static void testWifiLoopback()
{
    check(frameSetMode(WINJECT_MODE_STANDALONE), "loopback mode standalone");
    uint8_t peers[1][6] = {};
    memcpy(peers[0], kYouPeer, 6);
    frameSetPeerAirports(peers, 1);
    frameSetLocalAirports(nullptr, 0);
    drainWifiRx();

    const uint8_t payload[] = {'l', 'o', 'o', 'p'};
    uint8_t mpdu[WIFI_RADIO_INJECT_MAX] = {};
    const size_t n =
        frameWrap(payload, sizeof(payload), kPeerYou, mpdu, sizeof(mpdu));
    check(n > 0 && g_wifi.inject(mpdu, n), "loopback inject");

    uint8_t rx[WIFI_RADIO_MAX_FRAME] = {};
    size_t rxLen = 0;
    bool got = false;
    const int64_t deadline = esp_timer_get_time() + 500000;
    while (esp_timer_get_time() < deadline)
    {
        if (g_wifi.pop_rx(rx, &rxLen, sizeof(rx)))
        {
            got = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    check(got, "loopback rx");
    const uint8_t* body = nullptr;
    size_t bodyLen = 0;
    check(got && frameUnwrap(rx, rxLen, &body, &bodyLen) &&
              bodyLen == sizeof(payload) && memcmp(body, payload, bodyLen) == 0,
          "loopback payload");

    check(frameSetMode(WINJECT_MODE_BFC_TUNNEL_DEVICE),
          "loopback restore mode");
    frameSetPeerAirports(nullptr, 0);
}

static void testUpstreamApi()
{
    uint8_t bad[6] = {1, 2, 3, 4, 5, 6};
    check(!g_upstream_rx.set(kZeroMac, 0), "upstream rx port 0");
    check(!g_upstream_rx.set(bad, 19000), "upstream rx airport in tunnel");
    check(!g_upstream_tx.set(kZeroMac, 0, 19001), "upstream tx host 0");
    check(!g_upstream_tx.set(kZeroMac, 0xFFFFFFFFu, 19001),
          "upstream tx broadcast host");

    static upstream_bind_s rx[WIFI_AIRPORT_MAX] = {};
    static upstream_dest_s tx[WIFI_AIRPORT_MAX] = {};
    uint8_t rx_count = 0;
    uint8_t tx_count = 0;
    g_upstream_rx.fill_status(rx, &rx_count);
    g_upstream_tx.fill_status(tx, &tx_count);
    check(rx_count == 0 && tx_count == 0, "upstream unset");
}

static void testBfc()
{
    bfc::task_queue<uint32_t> q(false);
    check(q.push(7u) == 1, "bfc task_queue push");
    const auto got = q.pop();
    check(got.size() == 1 && got[0] == 7, "bfc task_queue pop");

    bfc::reactive_task_queue<uint32_t, bfc::light_function<void()>> rq;
    check(rq.push(1u) == 1, "bfc reactive_task_queue push");
    check(rq.has_data(), "bfc reactive_task_queue has_data");

    bfc::task_reactor<> reactor;
    check(!reactor.is_reactor_thread(), "bfc task_reactor idle");
    bfc::select_reactor<> select;
    check(!select.is_reactor_thread(), "bfc select_reactor idle");
}

static void testOtaBegin()
{
    check(otaActive(), "ota httpd");
}

static void testEthernetLink()
{
    uint32_t ip = 0;
    uint32_t mbps = 0;
    check(g_ethernet.connected(), "ethernet dhcp");
    check(g_ethernet.local_ipv4(&ip) && ip != 0, "ethernet ipv4");
    check(g_ethernet.link_speed_mbps(&mbps) && (mbps == 10 || mbps == 100),
          "ethernet speed");
}

static void testUpstreamNet(uint32_t ip)
{
    check(g_upstream_rx.set(kZeroMac, 19000), "upstream bind rx");
    const uint8_t payload[] = {'u', 'd', 'p'};
    check(udpSend(ip, 19000, payload, sizeof(payload)), "upstream udp send");
    vTaskDelay(pdMS_TO_TICKS(50));

    static upstream_bind_s rx[WIFI_AIRPORT_MAX] = {};
    uint8_t rx_count = 0;
    g_upstream_rx.fill_status(rx, &rx_count);
    check(rx_count == 1 && rx[0].port == 19000 && rx[0].socket_open,
          "upstream rx status");
}

static void testUpstreamAirToUdp(uint32_t ip)
{
    const uint16_t listenPort = 19001;
    const int fd = udpBind(ip, listenPort);
    check(fd >= 0, "upstream listen bind");
    if (fd < 0)
    {
        return;
    }

    check(frameSetMode(WINJECT_MODE_STANDALONE), "upstream air mode");
    check(g_upstream_tx.set(kYouPeer, ip, listenPort), "upstream set tx peer");
    drainWifiRx();

    const uint8_t payload[] = {'a', 'i', 'r'};
    uint8_t mpdu[WIFI_RADIO_INJECT_MAX] = {};
    const size_t n =
        frameWrap(payload, sizeof(payload), kPeerYou, mpdu, sizeof(mpdu));
    check(n > 0 && g_wifi.inject(mpdu, n), "upstream air inject");

    bool got = false;
    uint8_t rx[64] = {};
    const int64_t deadline = esp_timer_get_time() + 500000;
    while (esp_timer_get_time() < deadline)
    {
        struct sockaddr_in from = {};
        socklen_t fromLen = sizeof(from);
        const int nRx =
            recvfrom(fd, rx, sizeof(rx), 0,
                     reinterpret_cast<struct sockaddr*>(&from), &fromLen);
        if (nRx == static_cast<int>(sizeof(payload)) &&
            memcmp(rx, payload, sizeof(payload)) == 0)
        {
            got = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    check(got, "upstream air to udp");
    close(fd);

    check(frameSetMode(WINJECT_MODE_BFC_TUNNEL_DEVICE),
          "upstream restore tunnel");
}

static void testConsoleTcp(uint32_t ip)
{
    const int fd = tcpConnect(ip, CONTROL_CONSOLE_PORT, 1000);
    check(fd >= 0, "console connect");
    if (fd < 0)
    {
        return;
    }
    check(recvContains(fd, "type help", 1000), "console banner");
    const char help[] = "help\n";
    check(send(fd, help, sizeof(help) - 1, 0) ==
              static_cast<int>(sizeof(help) - 1),
          "console help send");
    check(recvContains(fd, "set_mode", 1000), "console help");
    close(fd);
}

static void testOtaHttp(uint32_t ip)
{
    const int fd = tcpConnect(ip, OTA_HTTP_PORT, 1000);
    check(fd >= 0, "ota connect");
    if (fd < 0)
    {
        return;
    }
    const char req[] = "GET / HTTP/1.0\r\nHost: winject\r\n\r\n";
    check(
        send(fd, req, sizeof(req) - 1, 0) == static_cast<int>(sizeof(req) - 1),
        "ota get send");
    check(recvContains(fd, "WInject-ESP32", 1000), "ota get form");
    close(fd);
}

static void runImmediateTests()
{
    ESP_LOGI(TAG, "--- immediate component tests ---");
    testEthernetMac();
    testWifiRadio();
    testSettings();
    testFrame();
    testBfc();
    testWifiLoopback();
    testOtaBegin();
    logSummary();
}

static void runNetTests(uint32_t ip)
{
    char ipStr[16];
    ipv4ToString(ip, ipStr, sizeof(ipStr));
    ESP_LOGI(TAG, "--- network tests on %s ---", ipStr);
    vTaskDelay(pdMS_TO_TICKS(200));
    testEthernetLink();
    testUpstreamNet(ip);
    testUpstreamAirToUdp(ip);
    testConsoleTcp(ip);
    testOtaHttp(ip);
    logSummary();
}

extern "C" void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_chip_info_t chip = {};
    esp_chip_info(&chip);
    uint32_t flashSize = 0;
    esp_flash_get_size(nullptr, &flashSize);

    ESP_LOGI(TAG, "winject-esp32 hardware test");
    ESP_LOGI(TAG, "chip  %s rev%d, %u core(s) @ %d MHz",
             chipModelName(chip.model), chip.revision, chip.cores,
             CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    ESP_LOGI(TAG, "flash %lu bytes, reset %s", (unsigned long)flashSize,
             resetReasonName(esp_reset_reason()));
    ESP_LOGI(TAG, "ETH   LAN8720 addr %d  MDC %d  MDIO %d  power %d",
             ETH_PHY_ADDR, ETH_PHY_MDC, ETH_PHY_MDIO, ETH_PHY_POWER);

    settingsLoadCurrent(g_netmgr);
    check(g_netmgr.start(), "netmgr start");
    check(g_wifi.initialize(), "wifi radio initialize");
    check(frameBegin(), "frame begin");
    check(g_upstream_rx.init(g_ethernet) && g_upstream_tx.init(g_ethernet),
          "upstream init");
    check(g_console.init(g_upstream_rx, g_upstream_tx, g_netmgr),
          "console init");
    otaBegin(g_netmgr);

    runImmediateTests();

    // Upstream TX drains the WiFi RX queue, so start it after loopback.
    check(g_upstream_rx.start_task() && g_upstream_tx.start_task(),
          "upstream tasks");
    testUpstreamApi();
    logSummary();

    bool netDone = false;
    uint32_t lastSec = 0;
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
        const uint32_t sec =
            static_cast<uint32_t>(esp_timer_get_time() / 1000000);
        if (sec == lastSec)
        {
            continue;
        }
        lastSec = sec;

        if (!netDone && g_ethernet.connected())
        {
            uint32_t ip = 0;
            if (g_ethernet.local_ipv4(&ip))
            {
                runNetTests(ip);
                netDone = true;
            }
        }

        wifi_status_s radio = {};
        g_wifi.get_status(&radio);
        if (g_ethernet.connected())
        {
            uint8_t mac[6] = {};
            uint32_t ip = 0;
            uint32_t mbps = 0;
            g_ethernet.mac(mac);
            g_ethernet.local_ipv4(&ip);
            g_ethernet.link_speed_mbps(&mbps);
            const uint8_t* b = reinterpret_cast<const uint8_t*>(&ip);
            ESP_LOGI(TAG,
                     "up %lus  heap %u  ETH %02X:%02X:%02X:%02X:%02X:%02X  "
                     "%u.%u.%u.%u  %luMbps  wifi ch%u %s  "
                     "%d pass %d fail",
                     (unsigned long)sec, (unsigned)esp_get_free_heap_size(),
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], b[0], b[1],
                     b[2], b[3], (unsigned long)mbps, radio.channel,
                     radio.modulation != nullptr ? radio.modulation : "-",
                     g_pass, g_fail);
        }
        else
        {
            ESP_LOGI(TAG,
                     "up %lus  heap %u  ETH waiting for link/DHCP  wifi ch%u "
                     "%s  %d pass %d fail",
                     (unsigned long)sec, (unsigned)esp_get_free_heap_size(),
                     radio.channel,
                     radio.modulation != nullptr ? radio.modulation : "-",
                     g_pass, g_fail);
        }
    }
}
