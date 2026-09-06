#include "net_util.h"

#include <gtest/gtest.h>

TEST(NetUtilTest, ParseBus)
{
    uint8_t bus = 0;
    ASSERT_TRUE(parse_bus("b2", &bus));
    EXPECT_EQ(bus, 0xb2);
    ASSERT_TRUE(parse_bus("0xA1", &bus));
    EXPECT_EQ(bus, 0xa1);
    ASSERT_TRUE(parse_bus("0", &bus));
    EXPECT_EQ(bus, 0);
    EXPECT_EQ(bus_to_string(0xb2), "b2");
    EXPECT_FALSE(parse_bus("", &bus));
    EXPECT_FALSE(parse_bus("zzz", &bus));
    EXPECT_FALSE(parse_bus("100", &bus));
}

TEST(NetUtilTest, ParseDomain)
{
    uint16_t domain = 0;
    ASSERT_TRUE(parse_domain("1234", &domain));
    EXPECT_EQ(domain, 0x1234);
    ASSERT_TRUE(parse_domain("0xab", &domain));
    EXPECT_EQ(domain, 0xab);
    EXPECT_EQ(domain_to_string(0x1234), "1234");
    EXPECT_FALSE(parse_domain("0", &domain));
    EXPECT_FALSE(parse_domain("", &domain));
    EXPECT_FALSE(parse_domain("10000", &domain));
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
