#include "upstream.h"
#include "config.h"
#include "ethernet.h"
#include "frame.h"
#include "wifi_radio.h"

#include <Arduino.h>
#include <errno.h>
#include <fcntl.h>
#include <lwip/sockets.h>
#include <stdio.h>
#include <string.h>

static int g_udpFd = -1;
static uint16_t g_rxPort = 0;
static bool g_txSet = false;
static uint32_t g_txHost = 0;
static uint16_t g_txPort = 0;
static uint32_t g_udpRx = 0;
static uint32_t g_udpTx = 0;
static uint32_t g_udpRxDropped = 0;
static uint8_t g_buf[WIFI_RADIO_MAX_FRAME];
static uint8_t g_mpdu[WIFI_RADIO_INJECT_MAX];

static void closeUdp() {
    if (g_udpFd >= 0) {
        close(g_udpFd);
        g_udpFd = -1;
    }
}

static bool setNonblock(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static bool openSocket(uint16_t port) {
    closeUdp();

    uint32_t ip = 0;
    if (!ethernetLocalIpv4(&ip)) {
        Serial.println("upstream udp: ethernet has no ipv4");
        return false;
    }

    const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        Serial.printf("upstream udp socket failed: %d\n", errno);
        return false;
    }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = ip;
    if (bind(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
        Serial.printf("upstream udp bind %u failed: %d\n", port, errno);
        close(fd);
        return false;
    }
    if (!setNonblock(fd)) {
        Serial.println("upstream udp fcntl failed");
        close(fd);
        return false;
    }

    g_udpFd = fd;
    return true;
}

static void drainRadio() {
    size_t len = 0;
    while (wifiRadioPopRx(g_buf, &len, sizeof(g_buf))) {
    }
}

void upstreamBegin() {
}

bool upstreamSetRxPort(uint16_t port) {
    if (port == 0) {
        return false;
    }
    if (!openSocket(port)) {
        return false;
    }
    g_rxPort = port;
    Serial.printf("upstream rx listening on UDP %u\n", port);
    return true;
}

bool upstreamSetTx(uint32_t host, uint16_t port) {
    if (port == 0 || host == 0 || host == 0xFFFFFFFFu) {
        return false;
    }
    if (g_udpFd < 0 && !openSocket(g_rxPort)) {
        return false;
    }
    g_txHost = host;
    g_txPort = port;
    g_txSet = true;

    char ipStr[16];
    const uint8_t *b = reinterpret_cast<const uint8_t *>(&host);
    snprintf(ipStr, sizeof(ipStr), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    Serial.printf("upstream tx %s:%u\n", ipStr, port);
    return true;
}

void upstreamPoll() {
    if (!ethernetConnected()) {
        closeUdp();
        drainRadio();
        return;
    }

    if (g_udpFd < 0 && (g_rxPort != 0 || g_txSet) && !openSocket(g_rxPort)) {
        drainRadio();
        return;
    }

    if (g_udpFd >= 0) {
        for (;;) {
            const int n = recvfrom(g_udpFd, g_buf, sizeof(g_buf), 0, nullptr, nullptr);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                    break;
                }
                g_udpRxDropped++;
                break;
            }
            if (n == 0 || n > static_cast<int>(WIFI_PAYLOAD_MAX)) {
                g_udpRxDropped++;
                continue;
            }
            const size_t framed =
                frameWrap(g_buf, static_cast<size_t>(n), g_mpdu, sizeof(g_mpdu));
            if (framed == 0 || !wifiRadioInject(g_mpdu, framed)) {
                g_udpRxDropped++;
                continue;
            }
            g_udpRx++;
        }
    }

    if (!g_txSet || g_udpFd < 0) {
        drainRadio();
        return;
    }

    size_t len = 0;
    while (wifiRadioPopRx(g_buf, &len, sizeof(g_buf))) {
        const uint8_t *payload = nullptr;
        size_t payloadLen = 0;
        if (!frameUnwrap(g_buf, len, &payload, &payloadLen)) {
            continue;
        }

        struct sockaddr_in dest = {};
        dest.sin_family = AF_INET;
        dest.sin_port = htons(g_txPort);
        dest.sin_addr.s_addr = g_txHost;
        const int sent = sendto(g_udpFd, payload, payloadLen, 0,
                                reinterpret_cast<struct sockaddr *>(&dest), sizeof(dest));
        if (sent == static_cast<int>(payloadLen)) {
            g_udpTx++;
        }
    }
}

void upstreamGetStatus(UpstreamStatus *status) {
    if (status == nullptr) {
        return;
    }
    status->rxBound = g_udpFd >= 0 && g_rxPort != 0;
    status->rxPort = g_rxPort;
    status->txSet = g_txSet;
    status->txHost = g_txHost;
    status->txPort = g_txPort;
    status->udpRx = g_udpRx;
    status->udpTx = g_udpTx;
    status->udpRxDropped = g_udpRxDropped;
}
