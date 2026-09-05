#include "config.h"

#include <cstdio>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <unistd.h>

namespace
{
std::string write_conf(const std::string& body)
{
    char path[] = "/tmp/winject-conf-XXXXXX";
    const int fd = mkstemp(path);
    EXPECT_GE(fd, 0);
    if (fd >= 0)
    {
        close(fd);
        std::ofstream out(path);
        out << body;
    }
    return path;
}
}  // namespace

TEST(ConfigTest, LoadsStandaloneUdp)
{
    const std::string path = write_conf(R"(
winject.device        = 192.168.32.1
winject.console       = 2323
winject.channel       = 1
winject.modulation    = OFDM_24M
winject.power         = 20
winject.mode          = STANDALONE
winject.max_rate_kbps = 10000
upstream.size = 1
upstream-0.mode             = UDP_GENERIC_FORWARDING
upstream-0.airport          = 00:00:00:AA:BB:CC
upstream-0.scheduler_budget = 100
upstream-0.rx               = 0.0.0.0:22081
upstream-0.tx               = 127.0.0.1:21082
)");
    config cfg;
    std::string err;
    ASSERT_TRUE(cfg.load(path, &err)) << err;
    EXPECT_EQ(cfg.device, "192.168.32.1");
    EXPECT_EQ(cfg.console_port, 2323);
    EXPECT_EQ(cfg.channel, 1);
    EXPECT_EQ(cfg.modulation, "OFDM_24M");
    EXPECT_EQ(cfg.power_dbm, 20);
    EXPECT_EQ(cfg.radio_mode, radio_mode_e::standalone);
    ASSERT_EQ(cfg.upstreams.size(), 1u);
    EXPECT_EQ(cfg.upstreams[0].mode, upstream_mode_e::udp_generic);
    EXPECT_EQ(cfg.upstreams[0].fec_type, fec_type_e::none);
    std::remove(path.c_str());
}

TEST(ConfigTest, DefaultMaxRateWhenOmitted)
{
    EXPECT_EQ(config::phy_rate_kbps("OFDM_24M"), 24000u);
    // Formula helper still exists; omitted config uses a fixed 10000 default.

    const std::string path = write_conf(R"(
winject.device        = 192.168.32.1
winject.console       = 2323
winject.channel       = 1
winject.modulation    = OFDM_24M
winject.power         = 20
winject.mode          = STANDALONE
upstream.size = 1
upstream-0.mode             = UDP_GENERIC_FORWARDING
upstream-0.airport          = 00:00:00:AA:BB:CC
upstream-0.scheduler_budget = 100
upstream-0.rx               = 0.0.0.0:22081
upstream-0.tx               = 127.0.0.1:21082
)");
    config cfg;
    std::string err;
    ASSERT_TRUE(cfg.load(path, &err)) << err;
    EXPECT_EQ(cfg.max_rate_kbps, 10000u);
    std::remove(path.c_str());
}

TEST(ConfigTest, TunnelRequiresAirportZero)
{
    const std::string path = write_conf(R"(
winject.device        = 192.168.32.1
winject.console       = 2323
winject.channel       = 1
winject.modulation    = OFDM_24M
winject.power         = 20
winject.mode          = BFC_TUNNEL_DEVICE
winject.max_rate_kbps = 10000
upstream.size = 1
upstream-0.mode             = UDP_CLIENT_FORWARDING
upstream-0.airport          = 00:00:00:AA:BB:CC
upstream-0.scheduler_budget = 100
upstream-0.connect_address  = 127.0.0.1:9
)");
    config cfg;
    std::string err;
    EXPECT_FALSE(cfg.load(path, &err));
    EXPECT_NE(err.find("airport"), std::string::npos);
    std::remove(path.c_str());
}

TEST(ConfigTest, LoadsRsBlockErasure)
{
    const std::string path = write_conf(R"(
winject.device        = 192.168.32.1
winject.console       = 2323
winject.channel       = 1
winject.modulation    = OFDM_24M
winject.power         = 20
winject.mode          = STANDALONE
upstream.size = 1
upstream-0.mode             = UDP_SERVER_FORWARDING
upstream-0.airport          = 00:00:00:AA:BB:CC
upstream-0.scheduler_budget = 4096
upstream-0.bind_address     = 127.0.0.1:22081
upstream-0.fec.type         = RS_BLOCK_ERASURE
upstream-0.fec.k            = 10
upstream-0.fec.n            = 15
upstream-0.fec.timeout_ms   = 25
)");
    config cfg;
    std::string err;
    ASSERT_TRUE(cfg.load(path, &err)) << err;
    ASSERT_EQ(cfg.upstreams.size(), 1u);
    EXPECT_EQ(cfg.upstreams[0].fec_type, fec_type_e::rs_block_erasure);
    EXPECT_EQ(cfg.upstreams[0].fec_k, 10);
    EXPECT_EQ(cfg.upstreams[0].fec_n, 15);
    EXPECT_EQ(cfg.upstreams[0].fec_timeout_ms, 25);
    std::remove(path.c_str());
}

TEST(ConfigTest, FecRejectsBadKn)
{
    const std::string path = write_conf(R"(
winject.device        = 192.168.32.1
winject.console       = 2323
winject.channel       = 1
winject.modulation    = OFDM_24M
winject.power         = 20
winject.mode          = STANDALONE
upstream.size = 1
upstream-0.mode             = UDP_SERVER_FORWARDING
upstream-0.airport          = 00:00:00:AA:BB:CC
upstream-0.scheduler_budget = 100
upstream-0.bind_address     = 127.0.0.1:22081
upstream-0.fec.type         = RS_BLOCK_ERASURE
upstream-0.fec.k            = 15
upstream-0.fec.n            = 10
)");
    config cfg;
    std::string err;
    EXPECT_FALSE(cfg.load(path, &err));
    EXPECT_NE(err.find("fec.k"), std::string::npos);
    std::remove(path.c_str());
}

TEST(ConfigTest, FecRejectedOnTcp)
{
    const std::string path = write_conf(R"(
winject.device        = 192.168.32.1
winject.console       = 2323
winject.channel       = 1
winject.modulation    = OFDM_24M
winject.power         = 20
winject.mode          = STANDALONE
upstream.size = 1
upstream-0.mode             = TCP_SERVER_FORWARDING
upstream-0.airport          = 02:02:03:04:05:06
upstream-0.scheduler_budget = 1024
upstream-0.bind_address     = 127.0.0.1:22022
upstream-0.fec.type         = RS_BLOCK_ERASURE
upstream-0.fec.k            = 10
upstream-0.fec.n            = 15
)");
    config cfg;
    std::string err;
    EXPECT_FALSE(cfg.load(path, &err));
    EXPECT_NE(err.find("UDP"), std::string::npos);
    std::remove(path.c_str());
}
