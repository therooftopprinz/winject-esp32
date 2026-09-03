#include "frame.h"
#include "winject-esp32/config.h"

#include <gtest/gtest.h>

#include <string.h>

namespace
{
const uint8_t kZeroMac[6] = {};
const uint8_t kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
const uint8_t kYouPeer[6] = {0x02, 0x02, 0x03, 0x04, 0x05, 0x06};
const uint8_t kPeerYou[6] = {0x04, 0x05, 0x06, 0x02, 0x02, 0x03};
const uint8_t kBcastAirport[6] = {0x00, 0x00, 0x00, 0xAA, 0xBB, 0xCC};
const uint8_t kTunnelBssid[6] = {WIFI_BSSID_TUNNEL};
const uint8_t kStandaloneBssid[6] = {WIFI_BSSID_STANDALONE};
const uint8_t kSta[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

class FrameTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(frameBegin());
        ASSERT_TRUE(frameSetMode(WINJECT_MODE_BFC_TUNNEL_DEVICE));
        frameSetPeerAirports(nullptr, 0);
        frameSetLocalAirports(nullptr, 0);
    }
};
}  // namespace

TEST_F(FrameTest, IdentityAndMode)
{
    uint8_t sta[6] = {};
    uint8_t bssid[6] = {};
    frameGetStaMac(sta);
    frameGetBssid(bssid);
    EXPECT_EQ(memcmp(sta, kSta, 6), 0);
    EXPECT_EQ(frameGetMode(), WINJECT_MODE_BFC_TUNNEL_DEVICE);
    EXPECT_EQ(memcmp(bssid, kTunnelBssid, 6), 0);
    EXPECT_STREQ(frameModeName(WINJECT_MODE_BFC_TUNNEL_DEVICE),
                 "BFC_TUNNEL_DEVICE");

    WinjectMode parsed = WINJECT_MODE_STANDALONE;
    EXPECT_TRUE(frameParseMode("bfc_tunnel_device", &parsed));
    EXPECT_EQ(parsed, WINJECT_MODE_BFC_TUNNEL_DEVICE);
    EXPECT_TRUE(frameParseMode("STANDALONE", &parsed));
    EXPECT_EQ(parsed, WINJECT_MODE_STANDALONE);
    EXPECT_FALSE(frameParseMode("nope", &parsed));
}

TEST_F(FrameTest, AirportHelpers)
{
    EXPECT_TRUE(frameAirportIsZero(kZeroMac));
    EXPECT_FALSE(frameAirportValidStandalone(kZeroMac));
    EXPECT_FALSE(frameAirportValidStandalone(kBroadcast));
    EXPECT_TRUE(frameAirportValidStandalone(kYouPeer));
    EXPECT_TRUE(frameAirportValidStandalone(kBcastAirport));
    const uint8_t kBadPeerHalf[6] = {0x02, 0x02, 0x03, 0x05, 0x05, 0x06};
    EXPECT_FALSE(frameAirportValidStandalone(kBadPeerHalf));
    EXPECT_TRUE(frameAirportIsBroadcastStandalone(kBcastAirport));
    EXPECT_FALSE(frameAirportIsBroadcastStandalone(kYouPeer));

    uint8_t swapped[6] = {};
    frameAirportSwap(kYouPeer, swapped);
    EXPECT_EQ(memcmp(swapped, kPeerYou, 6), 0);

    uint8_t filterSa[6] = {};
    frameStandaloneFilterSa(kYouPeer, filterSa);
    EXPECT_EQ(memcmp(filterSa, kPeerYou, 6), 0);
    frameStandaloneFilterSa(kBcastAirport, filterSa);
    EXPECT_EQ(memcmp(filterSa, kBcastAirport, 6), 0);
    EXPECT_TRUE(frameStandaloneSaMatchesAirport(kPeerYou, kYouPeer));
    EXPECT_FALSE(frameStandaloneSaMatchesAirport(kYouPeer, kYouPeer));
    EXPECT_TRUE(frameStandaloneSaMatchesAirport(kBcastAirport, kBcastAirport));
}

TEST_F(FrameTest, TunnelWrapClassify)
{
    const uint8_t payload[] = {'w', 'i', 'n', 'j'};
    uint8_t mpdu[WIFI_RADIO_INJECT_MAX] = {};
    const size_t n =
        frameWrap(payload, sizeof(payload), nullptr, mpdu, sizeof(mpdu));
    ASSERT_EQ(n, WIFI_HDR_LEN + sizeof(payload));
    EXPECT_EQ(mpdu[0], 0x08);
    EXPECT_EQ(mpdu[1], 0x00);
    EXPECT_EQ(memcmp(mpdu + 4, kBroadcast, 6), 0);
    EXPECT_EQ(memcmp(mpdu + 10, kSta, 6), 0);
    EXPECT_EQ(memcmp(mpdu + 16, kTunnelBssid, 6), 0);
    EXPECT_TRUE(frameBssidMatch(mpdu, n));
    EXPECT_FALSE(frameMatch(mpdu, n));
    EXPECT_EQ(frameClassify(mpdu, n), FRAME_RX_SELF);

    uint8_t foreign[WIFI_RADIO_INJECT_MAX] = {};
    memcpy(foreign, mpdu, n);
    foreign[10] ^= 0x01;
    EXPECT_TRUE(frameBssidMatch(foreign, n));
    EXPECT_EQ(frameClassify(foreign, n), FRAME_RX_NONE);
    uint8_t tunnelDest[1][6] = {};
    frameSetPeerAirports(tunnelDest, 1);
    EXPECT_EQ(frameClassify(foreign, n), FRAME_RX_MATCH);
    EXPECT_TRUE(frameHeard(mpdu, n));
    EXPECT_TRUE(frameHeard(foreign, n));
    foreign[16] ^= 0x01;
    EXPECT_FALSE(frameBssidMatch(foreign, n));
    EXPECT_EQ(frameClassify(foreign, n), FRAME_RX_NONE);
    frameSetPeerAirports(nullptr, 0);
    EXPECT_EQ(frameWrap(payload, sizeof(payload), nullptr, mpdu, WIFI_HDR_LEN),
              0u);
    EXPECT_EQ(frameWrap(payload, WIFI_PAYLOAD_MAX + 1, nullptr, mpdu,
                        sizeof(mpdu)),
              0u);
}

TEST_F(FrameTest, StandaloneWrapMatchUnwrap)
{
    ASSERT_TRUE(frameSetMode(WINJECT_MODE_STANDALONE));
    uint8_t bssid[6] = {};
    frameGetBssid(bssid);
    EXPECT_EQ(memcmp(bssid, kStandaloneBssid, 6), 0);

    const uint8_t payload[] = {'w', 'i', 'n', 'j'};
    uint8_t mpdu[WIFI_RADIO_INJECT_MAX] = {};
    EXPECT_EQ(
        frameWrap(payload, sizeof(payload), kZeroMac, mpdu, sizeof(mpdu)), 0u);
    const size_t sn =
        frameWrap(payload, sizeof(payload), kYouPeer, mpdu, sizeof(mpdu));
    ASSERT_EQ(sn, WIFI_HDR_LEN + sizeof(payload));
    EXPECT_EQ(memcmp(mpdu + 10, kYouPeer, 6), 0);
    EXPECT_TRUE(frameBssidMatch(mpdu, sn));
    EXPECT_FALSE(frameMatch(mpdu, sn));

    uint8_t peers[1][6] = {};
    memcpy(peers[0], kYouPeer, 6);
    frameSetPeerAirports(peers, 1);
    EXPECT_FALSE(frameMatch(mpdu, sn));

    uint8_t reply[WIFI_RADIO_INJECT_MAX] = {};
    const size_t rn =
        frameWrap(payload, sizeof(payload), kPeerYou, reply, sizeof(reply));
    ASSERT_EQ(rn, sn);
    EXPECT_TRUE(frameMatch(reply, rn));
    EXPECT_TRUE(frameHeard(reply, rn));
    uint8_t unknownSa[WIFI_RADIO_INJECT_MAX] = {};
    memcpy(unknownSa, reply, rn);
    unknownSa[15] ^= 0x02;
    EXPECT_FALSE(frameHeard(unknownSa, rn));

    const uint8_t* body = nullptr;
    size_t bodyLen = 0;
    EXPECT_TRUE(frameUnwrap(reply, rn, &body, &bodyLen));
    ASSERT_EQ(bodyLen, sizeof(payload));
    EXPECT_EQ(memcmp(body, payload, bodyLen), 0);

    uint8_t locals[1][6] = {};
    memcpy(locals[0], kYouPeer, 6);
    frameSetLocalAirports(locals, 1);
    EXPECT_EQ(frameClassify(mpdu, sn), FRAME_RX_SELF);
    frameSetLocalAirports(nullptr, 0);

    const size_t bn =
        frameWrap(payload, sizeof(payload), kBcastAirport, mpdu, sizeof(mpdu));
    ASSERT_EQ(bn, sn);
    memcpy(peers[0], kBcastAirport, 6);
    frameSetPeerAirports(peers, 1);
    EXPECT_TRUE(frameMatch(mpdu, bn));
    uint8_t bcastSwap[6] = {};
    frameAirportSwap(kBcastAirport, bcastSwap);
    const size_t bsn =
        frameWrap(payload, sizeof(payload), bcastSwap, reply, sizeof(reply));
    EXPECT_FALSE(frameMatch(reply, bsn));
}

TEST_F(FrameTest, StandaloneBroadcastSharedAirport)
{
    ASSERT_TRUE(frameSetMode(WINJECT_MODE_STANDALONE));
    uint8_t locals[1][6] = {};
    uint8_t peers[1][6] = {};
    memcpy(locals[0], kBcastAirport, 6);
    memcpy(peers[0], kBcastAirport, 6);
    frameSetLocalAirports(locals, 1);
    frameSetPeerAirports(peers, 1);

    const uint8_t payload[] = {'w', 'i', 'n', 'j'};
    uint8_t mpdu[WIFI_RADIO_INJECT_MAX] = {};
    const size_t n =
        frameWrap(payload, sizeof(payload), kBcastAirport, mpdu, sizeof(mpdu));
    ASSERT_EQ(n, WIFI_HDR_LEN + sizeof(payload));
    EXPECT_EQ(frameClassify(mpdu, n), FRAME_RX_MATCH);
    EXPECT_TRUE(frameHeard(mpdu, n));
}
