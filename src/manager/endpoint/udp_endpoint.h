#ifndef WINJECT_MANAGER_UDP_ENDPOINT_H_
#define WINJECT_MANAGER_UDP_ENDPOINT_H_

#include "config.h"

#include <deque>
#include <netinet/in.h>
#include <vector>

#include "frames/basic_fec.h"
#include "net_util.h"
#include "reactor.h"
#include "stream/stream.h"

class udp_endpoint : public stream
{
public:
    udp_endpoint() = default;
    ~udp_endpoint() override;
    bool open(::reactor& reactor, const upstream_config_s& cfg);
    void close();

    void on_radio_rx(const uint8_t* data, size_t len) override;
    bool has_tx() const override;
    size_t pull_tx(uint8_t* out, size_t max, bool* is_ack) override;
    void on_tick() override;
    void announce_down() override;
    uint64_t take_rx_bytes() override;
    stream_stats_s take_stats() override;

private:
    void on_app();
    void enqueue_air(std::vector<uint8_t> pkt);

    ::reactor* reactor = nullptr;
    bfc::socket sock;
    upstream_mode_e mode = upstream_mode_e::udp_generic;
    sockaddr_in dest{};
    bool dest_valid = false;
    std::deque<std::vector<uint8_t>> txq;
    uint8_t buf[2048]{};
    uint64_t radio_rx_pkt_interval = 0;
    uint64_t radio_rx_bytes_interval = 0;
    uint64_t air_tx_bytes_interval = 0;
    uint64_t air_rx_bytes_interval = 0;
    uint64_t app_rx_pkt_interval = 0;
    uint64_t app_rx_bytes_interval = 0;
    rs_block_erasure fec;
};

#endif  // WINJECT_MANAGER_UDP_ENDPOINT_H_
