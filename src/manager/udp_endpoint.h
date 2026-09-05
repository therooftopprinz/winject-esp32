#ifndef WINJECT_MANAGER_UDP_ENDPOINT_H_
#define WINJECT_MANAGER_UDP_ENDPOINT_H_

#include "config.h"

#include <deque>
#include <netinet/in.h>
#include <vector>

#include "fec.h"
#include "net_util.h"
#include "reactor.h"
#include "upstream.h"

class UdpEndpoint : public Upstream
{
public:
    UdpEndpoint() = default;
    ~UdpEndpoint() override;
    bool open(Reactor& reactor, const UpstreamConfig& cfg);
    void close();

    void on_radio_rx(const uint8_t* data, size_t len) override;
    bool has_tx() const override;
    size_t pull_tx(uint8_t* out, size_t max, bool* is_ack) override;
    void on_tick() override;
    void announce_down() override;
    uint64_t take_rx_bytes() override;
    StreamStats take_stats() override;

private:
    void on_app();
    void enqueue_air(std::vector<uint8_t> pkt);

    Reactor* reactor_ = nullptr;
    bfc::socket sock_;
    UpstreamMode mode_ = UpstreamMode::UdpGeneric;
    sockaddr_in dest_{};
    bool dest_valid_ = false;
    std::deque<std::vector<uint8_t>> txq_;
    uint8_t buf_[2048]{};
    uint64_t radio_rx_pkt_interval_ = 0;
    uint64_t radio_rx_bytes_interval_ = 0;
    uint64_t air_tx_bytes_interval_ = 0;
    uint64_t air_rx_bytes_interval_ = 0;
    uint64_t app_rx_pkt_interval_ = 0;
    uint64_t app_rx_bytes_interval_ = 0;
    RsBlockErasure fec_;
};

#endif  // WINJECT_MANAGER_UDP_ENDPOINT_H_
