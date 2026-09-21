#ifndef WINJECT_WIFI_BENCH_H_
#define WINJECT_WIFI_BENCH_H_

#include <stdint.h>

class wifi_tx;

struct wifi_bench_status_s
{
    bool running;
    uint16_t size;
    uint32_t kbps;
    uint32_t target;
    uint32_t enq_ok;
    uint32_t enq_fail;
    int64_t elapsed_us;
};

// On-device wifi_tx load generator (no Ethernet UDP inject).
class wifi_bench
{
public:
    static wifi_bench& instance();
    wifi_bench(const wifi_bench&) = delete;
    wifi_bench& operator=(const wifi_bench&) = delete;

    // count frames of `size` bytes (full MPDU). kbps>0: paced offer; kbps==0:
    // flood (enqueue as fast as wifi_tx accepts). size includes 802.11 header.
    bool start_tx(uint16_t size, uint32_t count, uint32_t kbps);
    void stop();
    void fill_status(wifi_bench_status_s* out) const;
    bool set_defaults(uint16_t size, uint32_t count, uint32_t kbps);
    void get_defaults(uint16_t* size, uint32_t* count, uint32_t* kbps) const;

private:
    wifi_bench() = default;
    static void task(void* arg);
    void run();
    bool try_enqueue_one(wifi_tx& tx, uint16_t domain, uint16_t size);
    void wait_until(int64_t target_t);

    volatile bool running_ = false;
    uint16_t size_ = 0;
    uint32_t kbps_ = 0;
    uint32_t target_ = 0;
    uint32_t enq_ok_ = 0;
    uint32_t enq_fail_ = 0;
    int64_t elapsed_us_ = 0;
    uint16_t default_size_ = 1424;
    uint32_t default_count_ = 10000;
    uint32_t default_kbps_ = 24000;
};

#endif  // WINJECT_WIFI_BENCH_H_
