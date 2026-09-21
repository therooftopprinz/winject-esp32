#ifndef WINJECT_ETHER_BENCH_H_
#define WINJECT_ETHER_BENCH_H_

#include "packet.h"

#include <stdint.h>

#include "bfc-esp32/select_reactor.hpp"

class manager;

struct ether_bench_status_s
{
    bool tx_running;
    bool rx_armed;
    uint16_t size;
    uint32_t tx_sent;
    uint32_t tx_fail;
    uint64_t tx_bytes;
    uint32_t rx_ok;
    uint32_t rx_bad;
    uint32_t rx_gap;
    uint64_t rx_bytes;
    uint32_t rx_last_sn;
    bool rx_have_sn;
    int64_t tx_elapsed_us;
};

class ether_bench
{
public:
    static ether_bench& instance();
    ether_bench(const ether_bench&) = delete;
    ether_bench& operator=(const ether_bench&) = delete;

    bool init(manager& netmgr);

    // Fire-and-forget UDP flood to dest. count=0 runs until stop().
    bool start_tx(ip_port_t dest, uint16_t size, uint32_t count);
    // Arm RX on ETHER_BENCH_PORT; clears RX counters. Uses a dedicated
    // drain task (not the select reactor) so host→device can keep up.
    bool arm_rx();
    void stop();
    void fill_status(ether_bench_status_s* out) const;

private:
    using reactor_t = bfc::select_reactor<>;

    ether_bench() = default;

    void attach();
    bool ensure_sock();
    void close_sock();
    void handle_rx_datagram(const uint8_t* buf, int n);
    static void tx_task(void* arg);
    static void rx_task(void* arg);

    manager* netmgr_ = nullptr;
    reactor_t* reactor_ = nullptr;
    int sock_ = -1;
    bool ready_ = false;
    volatile bool rx_armed_ = false;
    volatile bool rx_task_running_ = false;
    volatile bool tx_running_ = false;

    uint16_t size_ = 0;
    uint32_t tx_target_ = 0;
    ip_port_t tx_dest_{};

    uint32_t tx_sent_ = 0;
    uint32_t tx_fail_ = 0;
    uint64_t tx_bytes_ = 0;
    int64_t tx_elapsed_us_ = 0;

    uint32_t rx_ok_ = 0;
    uint32_t rx_bad_ = 0;
    uint32_t rx_gap_ = 0;
    uint64_t rx_bytes_ = 0;
    uint32_t rx_last_sn_ = 0;
    bool rx_have_sn_ = false;
};

#endif  // WINJECT_ETHER_BENCH_H_
