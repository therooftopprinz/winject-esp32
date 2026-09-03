#include "net_util.h"

#include <gtest/gtest.h>

TEST(NetUtilTest, ParseAirport)
{
    uint8_t mac[6] = {1, 1, 1, 1, 1, 1};
    ASSERT_TRUE(parse_airport("0", mac));
    EXPECT_TRUE(airport_is_zero(mac));
    ASSERT_TRUE(parse_airport("02:02:03:04:05:06", mac));
    EXPECT_EQ(airport_to_string(mac), "02:02:03:04:05:06");
    ASSERT_TRUE(parse_airport("AABBCCDDEEFF", mac));
    EXPECT_EQ(mac[0], 0xAA);
    EXPECT_EQ(mac[5], 0xFF);
    EXPECT_FALSE(parse_airport("", mac));
    EXPECT_FALSE(parse_airport("zz", mac));
}

TEST(NetUtilTest, ParseHostPort)
{
    sockaddr_in addr = {};
    ASSERT_TRUE(parse_host_port("127.0.0.1:22081", &addr));
    EXPECT_EQ(ntohs(addr.sin_port), 22081);
    EXPECT_EQ(ipv4_to_string(addr.sin_addr), "127.0.0.1");
    EXPECT_FALSE(parse_host_port("127.0.0.1", &addr));
    EXPECT_FALSE(parse_host_port(":80", &addr));
}
