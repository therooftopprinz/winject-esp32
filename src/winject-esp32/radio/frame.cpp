#include "frame.h"

#include "config.h"

#include <atomic>
#include <string.h>
#include <strings.h>

#include "bfc-esp32/semaphore.hpp"

#ifdef WINJECT_HOST_TEST
#include "host_idf_stub.h"
#else
#include "esp_log.h"
#include "esp_wifi.h"
#endif

static const char* TAG = "frame";
static const uint8_t kPrefixTunnel[4] = {WIFI_BSSID_PREFIX_TUNNEL};
static const uint8_t kPrefixStandalone[4] = {WIFI_BSSID_PREFIX_STANDALONE};

static constexpr int kSeqlockTries = 16;

struct FrameRules
{
    WinjectMode mode;
    uint8_t prefix[4];
    uint8_t staMac[6];
};

static FrameRules g_rules = {
    WINJECT_MODE_BFC_TUNNEL_DEVICE,
    {WIFI_BSSID_PREFIX_TUNNEL},
};
static std::atomic<uint32_t> g_seqlock{0};
static std::atomic<uint16_t> g_txSeq{0};
static bfc::semaphore g_writeLock;

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

static bool snapshot_rules(FrameRules* out)
{
    if (out == nullptr)
    {
        return false;
    }
    for (int i = 0; i < kSeqlockTries; i++)
    {
        const uint32_t s1 = g_seqlock.load(std::memory_order_acquire);
        if ((s1 & 1u) != 0)
        {
            continue;
        }
        *out = g_rules;
        if (s1 == g_seqlock.load(std::memory_order_acquire))
        {
            return true;
        }
    }
    *out = g_rules;
    return true;
}

static void emit_bits(uint8_t* packed, size_t* bit, uint16_t value, int nbits)
{
    for (int i = 0; i < nbits; i++)
    {
        const size_t b = (*bit)++;
        if ((value & 1u) != 0)
        {
            packed[b / 8] |= static_cast<uint8_t>(1u << (b % 8));
        }
        value = static_cast<uint16_t>(value >> 1);
    }
}

static uint16_t take_bits(const uint8_t* packed, size_t* bit, int nbits)
{
    uint16_t value = 0;
    for (int i = 0; i < nbits; i++)
    {
        const size_t b = (*bit)++;
        if ((packed[b / 8] & (1u << (b % 8))) != 0)
        {
            value |= static_cast<uint16_t>(1u << i);
        }
    }
    return value;
}

void framePackSlots(uint8_t addr1[6], uint8_t addr2[6],
                    const pdu_slot_t slots[WIFI_PDU_SLOTS])
{
    if (addr1 == nullptr || addr2 == nullptr || slots == nullptr)
    {
        return;
    }
    uint8_t packed[12] = {};
    size_t bit = 0;
    // Bit 0 of Addr1[0] is 802.11 I/G. Force group (1) so raw inject does not
    // wait for an ACK. The former trailing spare bit pays for this.
    emit_bits(packed, &bit, 1, 1);
    for (int i = 0; i < WIFI_PDU_SLOTS; i++)
    {
        const uint16_t size = static_cast<uint16_t>(slots[i].size & 0x7FFu);
        emit_bits(packed, &bit, slots[i].bus, 8);
        emit_bits(packed, &bit, size, 11);
    }
    memcpy(addr1, packed, 6);
    memcpy(addr2, packed + 6, 6);
}

void frameUnpackSlots(const uint8_t addr1[6], const uint8_t addr2[6],
                      pdu_slot_t slots[WIFI_PDU_SLOTS])
{
    if (addr1 == nullptr || addr2 == nullptr || slots == nullptr)
    {
        return;
    }
    uint8_t packed[12];
    memcpy(packed, addr1, 6);
    memcpy(packed + 6, addr2, 6);
    size_t bit = 0;
    (void)take_bits(packed, &bit, 1);  // I/G (ignore)
    for (int i = 0; i < WIFI_PDU_SLOTS; i++)
    {
        slots[i].bus = static_cast<bus_t>(take_bits(packed, &bit, 8));
        slots[i].size = take_bits(packed, &bit, 11);
    }
}

size_t frameSlotPayloadBytes(const pdu_slot_t slots[WIFI_PDU_SLOTS])
{
    if (slots == nullptr)
    {
        return 0;
    }
    size_t sum = 0;
    for (int i = 0; i < WIFI_PDU_SLOTS; i++)
    {
        sum += slots[i].size;
    }
    return sum;
}

void frameBuildAddr3(uint8_t addr3[6], uint16_t domain)
{
    if (addr3 == nullptr)
    {
        return;
    }
    FrameRules rules = {};
    snapshot_rules(&rules);
    memcpy(addr3, rules.prefix, 4);
    addr3[4] = static_cast<uint8_t>(domain >> 8);
    addr3[5] = static_cast<uint8_t>(domain);
}

bool frameAddr3Accept(const uint8_t* mpdu, size_t len, uint16_t domain)
{
    if (mpdu == nullptr || len < 22 || domain == 0)
    {
        return false;
    }
    FrameRules rules = {};
    snapshot_rules(&rules);
    if (memcmp(mpdu + 16, rules.prefix, 4) != 0)
    {
        return false;
    }
    const uint16_t got = static_cast<uint16_t>((mpdu[20] << 8) | mpdu[21]);
    return got == domain;
}

void frameStampHeader(uint8_t* hdr, const pdu_slot_t slots[WIFI_PDU_SLOTS],
                      uint16_t domain)
{
    if (hdr == nullptr || slots == nullptr)
    {
        return;
    }
    hdr[0] = 0x08;
    hdr[1] = 0x00;
    hdr[2] = 0x00;
    hdr[3] = 0x00;
    framePackSlots(hdr + 4, hdr + 10, slots);
    frameBuildAddr3(hdr + 16, domain);
    const uint16_t seq =
        g_txSeq.fetch_add(1, std::memory_order_relaxed) & 0x0FFF;
    const uint16_t seqCtl = static_cast<uint16_t>(seq << 4);
    hdr[22] = static_cast<uint8_t>(seqCtl);
    hdr[23] = static_cast<uint8_t>(seqCtl >> 8);
}

const char* frameModeName(WinjectMode mode)
{
    switch (mode)
    {
        case WINJECT_MODE_BFC_TUNNEL_DEVICE:
            return "BFC_TUNNEL_DEVICE";
        case WINJECT_MODE_STANDALONE:
            return "STANDALONE";
        case WINJECT_MODE_OTA:
            return "OTA";
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
    if (strcasecmp(text, "OTA") == 0)
    {
        *mode = WINJECT_MODE_OTA;
        return true;
    }
    return false;
}

bool frameSetMode(WinjectMode mode)
{
    if (mode == WINJECT_MODE_OTA)
    {
        RulesWriter writer;
        g_rules.mode = mode;
        ESP_LOGI(TAG, "mode OTA");
        return true;
    }

    const uint8_t* prefix = nullptr;
    if (mode == WINJECT_MODE_BFC_TUNNEL_DEVICE)
    {
        prefix = kPrefixTunnel;
    }
    else if (mode == WINJECT_MODE_STANDALONE)
    {
        prefix = kPrefixStandalone;
    }
    else
    {
        return false;
    }

    RulesWriter writer;
    g_rules.mode = mode;
    memcpy(g_rules.prefix, prefix, 4);

    ESP_LOGI(TAG, "mode %s  BSSID %02X:%02X:%02X:%02X:DH:DL",
             frameModeName(mode), prefix[0], prefix[1], prefix[2], prefix[3]);
    return true;
}

WinjectMode frameGetMode()
{
    FrameRules rules = {};
    snapshot_rules(&rules);
    return rules.mode;
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
             "%02X:%02X:%02X:%02X:DH:DL",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], g_rules.prefix[0],
             g_rules.prefix[1], g_rules.prefix[2], g_rules.prefix[3]);
    return true;
}

void frameGetStaMac(uint8_t mac[6])
{
    if (mac == nullptr)
    {
        return;
    }
    FrameRules rules = {};
    snapshot_rules(&rules);
    memcpy(mac, rules.staMac, 6);
}

void frameGetBssidPrefix(uint8_t prefix[4])
{
    if (prefix == nullptr)
    {
        return;
    }
    FrameRules rules = {};
    snapshot_rules(&rules);
    memcpy(prefix, rules.prefix, 4);
}
