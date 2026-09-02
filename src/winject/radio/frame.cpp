#include "frame.h"

#include "config.h"

#include <atomic>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_wifi.h"

static const char* TAG = "frame";
static const uint8_t kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static const uint8_t kZeroMac[6] = {};
static const uint8_t kBssidTunnel[6] = {WIFI_BSSID_TUNNEL};
static const uint8_t kBssidStandalone[6] = {WIFI_BSSID_STANDALONE};

static constexpr int kSeqlockTries = 16;

struct FrameRules
{
    WinjectMode mode;
    uint8_t bssid[6];
    uint8_t staMac[6];
    uint8_t localSas[WIFI_AIRPORT_MAX][6];
    size_t localSaCount;
    uint8_t peerSas[WIFI_AIRPORT_MAX][6];
    size_t peerSaCount;
};

static FrameRules g_rules = {
    WINJECT_MODE_BFC_TUNNEL_DEVICE,
    {WIFI_BSSID_TUNNEL},
};
static std::atomic<uint32_t> g_seqlock{0};
static std::atomic<uint16_t> g_txSeq{0};
static semaphore g_writeLock;

class RulesWriter
{
public:
    RulesWriter()
    {
        g_writeLock.init();
        owned_ = g_writeLock.take();
        g_seqlock.fetch_add(1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
    }

    ~RulesWriter()
    {
        g_seqlock.fetch_add(1, std::memory_order_release);
        if (owned_)
        {
            g_writeLock.give();
        }
    }

    RulesWriter(const RulesWriter&) = delete;
    RulesWriter& operator=(const RulesWriter&) = delete;

private:
    bool owned_ = false;
};

static size_t headerLen(const uint8_t* mpdu, size_t len)
{
    if (mpdu == nullptr || len < WIFI_HDR_LEN)
    {
        return 0;
    }
    if ((mpdu[0] & 0x0C) != 0x08)
    {
        return 0;
    }
    if ((mpdu[0] & 0xF0) == 0x80)
    {
        return len >= WIFI_QOS_HDR_LEN ? WIFI_QOS_HDR_LEN : 0;
    }
    if ((mpdu[0] & 0xF0) == 0x00)
    {
        return WIFI_HDR_LEN;
    }
    return 0;
}

static bool macHalfZero(const uint8_t mac[3])
{
    return mac[0] == 0 && mac[1] == 0 && mac[2] == 0;
}

static bool macInList(const uint8_t mac[6], const uint8_t list[][6], size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        if (memcmp(mac, list[i], 6) == 0)
        {
            return true;
        }
    }
    return false;
}

static bool saMatchesPeerList(const uint8_t sa[6], const uint8_t list[][6],
                              size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        if (frameStandaloneSaMatchesAirport(sa, list[i]))
        {
            return true;
        }
    }
    return false;
}

static void copyAirportList(uint8_t dst[][6], size_t* dstCount,
                            const uint8_t macs[][6], size_t count)
{
    if (count > WIFI_AIRPORT_MAX)
    {
        count = WIFI_AIRPORT_MAX;
    }
    if (count == 0 || macs == nullptr)
    {
        *dstCount = 0;
        return;
    }
    memcpy(dst, macs, count * 6);
    *dstCount = count;
}

static bool bssidEq(const uint8_t* mpdu, size_t len, const uint8_t bssid[6])
{
    return mpdu != nullptr && len >= 22 && memcmp(mpdu + 16, bssid, 6) == 0;
}

static bool heardWith(const FrameRules& r, const uint8_t* mpdu, size_t len)
{
    if (!bssidEq(mpdu, len, r.bssid))
    {
        return false;
    }
    if (r.mode == WINJECT_MODE_BFC_TUNNEL_DEVICE)
    {
        return true;
    }
    const uint8_t* sa = mpdu + 10;
    return macInList(sa, r.localSas, r.localSaCount) ||
           saMatchesPeerList(sa, r.peerSas, r.peerSaCount);
}

static FrameRxClass classifyWith(const FrameRules& r, const uint8_t* mpdu,
                                 size_t len)
{
    const size_t hdr = headerLen(mpdu, len);
    if (hdr == 0)
    {
        return FRAME_RX_NONE;
    }
    if (memcmp(mpdu + 4, kBroadcast, 6) != 0)
    {
        return FRAME_RX_NONE;
    }
    if (!bssidEq(mpdu, len, r.bssid))
    {
        return FRAME_RX_NONE;
    }

    const uint8_t* sa = mpdu + 10;
    if (r.mode == WINJECT_MODE_BFC_TUNNEL_DEVICE)
    {
        if (memcmp(sa, r.staMac, 6) == 0)
        {
            return FRAME_RX_SELF;
        }
        if (r.peerSaCount == 0)
        {
            return FRAME_RX_NONE;
        }
        return FRAME_RX_MATCH;
    }
    if (macInList(sa, r.localSas, r.localSaCount))
    {
        return FRAME_RX_SELF;
    }
    if (saMatchesPeerList(sa, r.peerSas, r.peerSaCount))
    {
        return FRAME_RX_MATCH;
    }
    return FRAME_RX_NONE;
}

bool frameAirportIsZero(const uint8_t mac[6])
{
    return mac != nullptr && memcmp(mac, kZeroMac, 6) == 0;
}

bool frameAirportValidStandalone(const uint8_t mac[6])
{
    if (mac == nullptr || frameAirportIsZero(mac))
    {
        return false;
    }
    if ((mac[0] & 0x01) != 0)
    {
        return false;
    }
    if (!macHalfZero(mac) && (mac[3] & 0x01) != 0)
    {
        return false;
    }
    return true;
}

bool frameAirportIsBroadcastStandalone(const uint8_t mac[6])
{
    return mac != nullptr && macHalfZero(mac) && !macHalfZero(mac + 3);
}

void frameAirportSwap(const uint8_t in[6], uint8_t out[6])
{
    if (in == nullptr || out == nullptr)
    {
        return;
    }
    uint8_t tmp[6];
    memcpy(tmp, in + 3, 3);
    memcpy(tmp + 3, in, 3);
    memcpy(out, tmp, 6);
}

void frameStandaloneFilterSa(const uint8_t airport[6], uint8_t sa[6])
{
    if (airport == nullptr || sa == nullptr)
    {
        return;
    }
    if (frameAirportIsBroadcastStandalone(airport))
    {
        memcpy(sa, airport, 6);
        return;
    }
    frameAirportSwap(airport, sa);
}

bool frameStandaloneSaMatchesAirport(const uint8_t sa[6],
                                     const uint8_t airport[6])
{
    if (sa == nullptr || airport == nullptr)
    {
        return false;
    }
    uint8_t want[6] = {};
    frameStandaloneFilterSa(airport, want);
    return memcmp(sa, want, 6) == 0;
}

const char* frameModeName(WinjectMode mode)
{
    switch (mode)
    {
        case WINJECT_MODE_BFC_TUNNEL_DEVICE:
            return "BFC_TUNNEL_DEVICE";
        case WINJECT_MODE_STANDALONE:
            return "STANDALONE";
        default:
            return "UNKNOWN";
    }
}

bool frameParseMode(const char* text, WinjectMode* mode)
{
    if (text == nullptr || mode == nullptr)
    {
        return false;
    }
    if (strcasecmp(text, "BFC_TUNNEL_DEVICE") == 0)
    {
        *mode = WINJECT_MODE_BFC_TUNNEL_DEVICE;
        return true;
    }
    if (strcasecmp(text, "STANDALONE") == 0)
    {
        *mode = WINJECT_MODE_STANDALONE;
        return true;
    }
    return false;
}

bool frameSetMode(WinjectMode mode)
{
    const uint8_t* bssid = nullptr;
    if (mode == WINJECT_MODE_BFC_TUNNEL_DEVICE)
    {
        bssid = kBssidTunnel;
    }
    else if (mode == WINJECT_MODE_STANDALONE)
    {
        bssid = kBssidStandalone;
    }
    else
    {
        return false;
    }

    RulesWriter writer;
    g_rules.mode = mode;
    memcpy(g_rules.bssid, bssid, 6);

    ESP_LOGI(TAG, "mode %s  BSSID %02X:%02X:%02X:%02X:%02X:%02X",
             frameModeName(mode), bssid[0], bssid[1], bssid[2], bssid[3],
             bssid[4], bssid[5]);
    return true;
}

WinjectMode frameGetMode()
{
    for (int i = 0; i < kSeqlockTries; i++)
    {
        const uint32_t s1 = g_seqlock.load(std::memory_order_acquire);
        if ((s1 & 1u) != 0)
        {
            continue;
        }
        const WinjectMode mode = g_rules.mode;
        if (s1 == g_seqlock.load(std::memory_order_acquire))
        {
            return mode;
        }
    }
    return g_rules.mode;
}

void frameSetLocalAirports(const uint8_t macs[][6], size_t count)
{
    RulesWriter writer;
    copyAirportList(g_rules.localSas, &g_rules.localSaCount, macs, count);
}

void frameSetPeerAirports(const uint8_t macs[][6], size_t count)
{
    RulesWriter writer;
    copyAirportList(g_rules.peerSas, &g_rules.peerSaCount, macs, count);
}

bool frameBegin()
{
    uint8_t mac[6] = {};
    const esp_err_t err = esp_wifi_get_mac(WIFI_IF_STA, mac);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "frame sta mac failed: %s", esp_err_to_name(err));
        return false;
    }

    RulesWriter writer;
    memcpy(g_rules.staMac, mac, 6);

    ESP_LOGI(TAG,
             "STA %02X:%02X:%02X:%02X:%02X:%02X  BSSID "
             "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], g_rules.bssid[0],
             g_rules.bssid[1], g_rules.bssid[2], g_rules.bssid[3],
             g_rules.bssid[4], g_rules.bssid[5]);
    return true;
}

size_t frameWrap(const uint8_t* payload, size_t payloadLen, const uint8_t sa[6],
                 uint8_t* out, size_t outMax)
{
    if (payload == nullptr || out == nullptr)
    {
        return 0;
    }
    if (payloadLen > WIFI_PAYLOAD_MAX)
    {
        return 0;
    }
    const size_t total = WIFI_HDR_LEN + payloadLen;
    if (outMax < total)
    {
        return 0;
    }

    WinjectMode mode = WINJECT_MODE_BFC_TUNNEL_DEVICE;
    uint8_t sta[6] = {};
    uint8_t bssid[6] = {};
    bool got = false;
    for (int i = 0; i < kSeqlockTries; i++)
    {
        const uint32_t s1 = g_seqlock.load(std::memory_order_acquire);
        if ((s1 & 1u) != 0)
        {
            continue;
        }
        mode = g_rules.mode;
        memcpy(sta, g_rules.staMac, 6);
        memcpy(bssid, g_rules.bssid, 6);
        if (s1 == g_seqlock.load(std::memory_order_acquire))
        {
            got = true;
            break;
        }
    }
    if (!got)
    {
        return 0;
    }

    const uint8_t* src = sta;
    if (mode == WINJECT_MODE_STANDALONE)
    {
        if (!frameAirportValidStandalone(sa))
        {
            return 0;
        }
        src = sa;
    }

    out[0] = 0x08;
    out[1] = 0x00;
    out[2] = 0x00;
    out[3] = 0x00;
    memcpy(out + 4, kBroadcast, 6);
    memcpy(out + 10, src, 6);
    memcpy(out + 16, bssid, 6);

    const uint16_t seq =
        g_txSeq.fetch_add(1, std::memory_order_relaxed) & 0x0FFF;
    const uint16_t seqCtl = static_cast<uint16_t>(seq << 4);
    out[22] = static_cast<uint8_t>(seqCtl);
    out[23] = static_cast<uint8_t>(seqCtl >> 8);

    if (payloadLen > 0)
    {
        memcpy(out + WIFI_HDR_LEN, payload, payloadLen);
    }
    return total;
}

bool frameBssidMatch(const uint8_t* mpdu, size_t len)
{
    for (int i = 0; i < kSeqlockTries; i++)
    {
        const uint32_t s1 = g_seqlock.load(std::memory_order_acquire);
        if ((s1 & 1u) != 0)
        {
            continue;
        }
        const bool ok = bssidEq(mpdu, len, g_rules.bssid);
        if (s1 == g_seqlock.load(std::memory_order_acquire))
        {
            return ok;
        }
    }
    return false;
}

bool frameHeard(const uint8_t* mpdu, size_t len)
{
    for (int i = 0; i < kSeqlockTries; i++)
    {
        const uint32_t s1 = g_seqlock.load(std::memory_order_acquire);
        if ((s1 & 1u) != 0)
        {
            continue;
        }
        const bool ok = heardWith(g_rules, mpdu, len);
        if (s1 == g_seqlock.load(std::memory_order_acquire))
        {
            return ok;
        }
    }
    return false;
}

bool frameMatch(const uint8_t* mpdu, size_t len)
{
    return frameClassify(mpdu, len) == FRAME_RX_MATCH;
}

FrameRxClass frameClassify(const uint8_t* mpdu, size_t len)
{
    for (int i = 0; i < kSeqlockTries; i++)
    {
        const uint32_t s1 = g_seqlock.load(std::memory_order_acquire);
        if ((s1 & 1u) != 0)
        {
            continue;
        }
        const FrameRxClass kind = classifyWith(g_rules, mpdu, len);
        if (s1 == g_seqlock.load(std::memory_order_acquire))
        {
            return kind;
        }
    }
    return FRAME_RX_NONE;
}

bool frameUnwrap(const uint8_t* mpdu, size_t len, const uint8_t** payload,
                 size_t* payloadLen)
{
    if (!frameMatch(mpdu, len) || payload == nullptr || payloadLen == nullptr)
    {
        return false;
    }
    const size_t hdr = headerLen(mpdu, len);
    *payload = mpdu + hdr;
    *payloadLen = len - hdr;
    return true;
}

void frameGetStaMac(uint8_t mac[6])
{
    if (mac == nullptr)
    {
        return;
    }
    for (int i = 0; i < kSeqlockTries; i++)
    {
        const uint32_t s1 = g_seqlock.load(std::memory_order_acquire);
        if ((s1 & 1u) != 0)
        {
            continue;
        }
        memcpy(mac, g_rules.staMac, 6);
        if (s1 == g_seqlock.load(std::memory_order_acquire))
        {
            return;
        }
    }
    memcpy(mac, g_rules.staMac, 6);
}

void frameGetBssid(uint8_t bssid[6])
{
    if (bssid == nullptr)
    {
        return;
    }
    for (int i = 0; i < kSeqlockTries; i++)
    {
        const uint32_t s1 = g_seqlock.load(std::memory_order_acquire);
        if ((s1 & 1u) != 0)
        {
            continue;
        }
        memcpy(bssid, g_rules.bssid, 6);
        if (s1 == g_seqlock.load(std::memory_order_acquire))
        {
            return;
        }
    }
    memcpy(bssid, g_rules.bssid, 6);
}
