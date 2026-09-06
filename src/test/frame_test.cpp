#include "frame.h"

#include <gtest/gtest.h>

#include <string.h>

namespace
{
const uint8_t kPrefixTunnel[4] = {WIFI_BSSID_PREFIX_TUNNEL};
const uint8_t kPrefixStandalone[4] = {WIFI_BSSID_PREFIX_STANDALONE};
const uint8_t kSta[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

class FrameTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(frameBegin());
        ASSERT_TRUE(frameSetMode(WINJECT_MODE_BFC_TUNNEL_DEVICE));
    }
};

static void expect_mac6(const uint8_t got[6], const uint8_t want[6])
{
    EXPECT_EQ(memcmp(got, want, 6), 0);
}
}  // namespace

TEST_F(FrameTest, IdentityAndMode)
{
    uint8_t sta[6] = {};
    uint8_t prefix[4] = {};
    frameGetStaMac(sta);
    frameGetBssidPrefix(prefix);
    EXPECT_EQ(memcmp(sta, kSta, 6), 0);
    EXPECT_EQ(frameGetMode(), WINJECT_MODE_BFC_TUNNEL_DEVICE);
    EXPECT_EQ(memcmp(prefix, kPrefixTunnel, 4), 0);
    EXPECT_STREQ(frameModeName(WINJECT_MODE_BFC_TUNNEL_DEVICE),
                 "BFC_TUNNEL_DEVICE");

    WinjectMode parsed = WINJECT_MODE_STANDALONE;
    EXPECT_TRUE(frameParseMode("bfc_tunnel_device", &parsed));
    EXPECT_EQ(parsed, WINJECT_MODE_BFC_TUNNEL_DEVICE);
    EXPECT_TRUE(frameParseMode("STANDALONE", &parsed));
    EXPECT_EQ(parsed, WINJECT_MODE_STANDALONE);
    EXPECT_TRUE(frameParseMode("OTA", &parsed));
    EXPECT_EQ(parsed, WINJECT_MODE_OTA);
    EXPECT_FALSE(frameParseMode("nope", &parsed));

    ASSERT_TRUE(frameSetMode(WINJECT_MODE_OTA));
    EXPECT_EQ(frameGetMode(), WINJECT_MODE_OTA);
    EXPECT_STREQ(frameModeName(WINJECT_MODE_OTA), "OTA");

    ASSERT_TRUE(frameSetMode(WINJECT_MODE_STANDALONE));
    frameGetBssidPrefix(prefix);
    EXPECT_EQ(memcmp(prefix, kPrefixStandalone, 4), 0);
}

TEST_F(FrameTest, PackOnePduGolden)
{
    pdu_slot_t slots[WIFI_PDU_SLOTS] = {};
    slots[0] = {0xB2, 3};
    uint8_t addr1[6] = {};
    uint8_t addr2[6] = {};
    framePackSlots(addr1, addr2, slots);
    const uint8_t want1[6] = {0x65, 0x07, 0x00, 0x00, 0x00, 0x00};
    const uint8_t want2[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    expect_mac6(addr1, want1);
    expect_mac6(addr2, want2);
    EXPECT_EQ(addr1[0] & 0x01, 0x01);  // 802.11 I/G group

    pdu_slot_t round[WIFI_PDU_SLOTS] = {};
    frameUnpackSlots(addr1, addr2, round);
    EXPECT_EQ(round[0].bus, 0xB2);
    EXPECT_EQ(round[0].size, 3);
    for (int i = 1; i < WIFI_PDU_SLOTS; i++)
    {
        EXPECT_EQ(round[i].size, 0);
    }
    EXPECT_EQ(frameSlotPayloadBytes(slots), 3u);
}

TEST_F(FrameTest, PackTwoPduGolden)
{
    pdu_slot_t slots[WIFI_PDU_SLOTS] = {};
    slots[0] = {0xB2, 2};
    slots[1] = {0xC3, 3};
    uint8_t addr1[6] = {};
    uint8_t addr2[6] = {};
    framePackSlots(addr1, addr2, slots);
    const uint8_t want1[6] = {0x65, 0x05, 0x30, 0x3C, 0x00, 0x00};
    const uint8_t want2[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    expect_mac6(addr1, want1);
    expect_mac6(addr2, want2);
    EXPECT_EQ(addr1[0] & 0x01, 0x01);

    pdu_slot_t round[WIFI_PDU_SLOTS] = {};
    frameUnpackSlots(addr1, addr2, round);
    EXPECT_EQ(round[0].bus, 0xB2);
    EXPECT_EQ(round[0].size, 2);
    EXPECT_EQ(round[1].bus, 0xC3);
    EXPECT_EQ(round[1].size, 3);
    EXPECT_EQ(round[2].size, 0);
    EXPECT_EQ(frameSlotPayloadBytes(slots), 5u);
}

TEST_F(FrameTest, StampAndAddr3)
{
    pdu_slot_t slots[WIFI_PDU_SLOTS] = {};
    slots[0] = {0xB2, 3};
    uint8_t hdr[WIFI_HDR_LEN] = {};
    frameStampHeader(hdr, slots, 0x1234);
    EXPECT_EQ(hdr[0], 0x08);
    EXPECT_EQ(hdr[1], 0x00);
    const uint8_t want1[6] = {0x65, 0x07, 0x00, 0x00, 0x00, 0x00};
    const uint8_t want3[6] = {0xBA, 0xDD, 0xCA, 0xFE, 0x12, 0x34};
    expect_mac6(hdr + 4, want1);
    expect_mac6(hdr + 16, want3);
    EXPECT_TRUE(frameAddr3Accept(hdr, sizeof(hdr), 0x1234));
    EXPECT_FALSE(frameAddr3Accept(hdr, sizeof(hdr), 0));
    EXPECT_FALSE(frameAddr3Accept(hdr, sizeof(hdr), 0x0001));

    ASSERT_TRUE(frameSetMode(WINJECT_MODE_STANDALONE));
    uint8_t addr3[6] = {};
    frameBuildAddr3(addr3, 0x0001);
    const uint8_t want_sa[6] = {0xCA, 0xFE, 0xBA, 0xBE, 0x00, 0x01};
    expect_mac6(addr3, want_sa);

    uint8_t mpdu[WIFI_HDR_LEN] = {};
    memcpy(mpdu + 16, addr3, 6);
    EXPECT_TRUE(frameAddr3Accept(mpdu, sizeof(mpdu), 0x0001));
    EXPECT_FALSE(frameAddr3Accept(mpdu, sizeof(mpdu), 0x1234));
}

TEST_F(FrameTest, Size11RoundTrip)
{
    pdu_slot_t slots[WIFI_PDU_SLOTS] = {};
    slots[0] = {0x01, 0x7FF};
    slots[4] = {0xFF, 1};
    uint8_t addr1[6] = {};
    uint8_t addr2[6] = {};
    framePackSlots(addr1, addr2, slots);
    pdu_slot_t round[WIFI_PDU_SLOTS] = {};
    frameUnpackSlots(addr1, addr2, round);
    EXPECT_EQ(round[0].bus, 0x01);
    EXPECT_EQ(round[0].size, 0x7FF);
    EXPECT_EQ(round[4].bus, 0xFF);
    EXPECT_EQ(round[4].size, 1);
    EXPECT_EQ(round[1].size, 0);
}
