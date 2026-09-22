#ifndef WINJECT_MANAGER_CONSOLE_SERVICE_H_
#define WINJECT_MANAGER_CONSOLE_SERVICE_H_

#include "config.h"

#include <functional>
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

#include "net_util.h"
#include "reactor.h"

// Local UDP console for runtime manager controls (not the radio console).
// Binds console_in for commands; always sends replies to console_out.
// One command datagram = one line. Responses: "ok\n", "ok <args>\n", or
// "nok <msg>\n". gci args may span multiple lines in one reply datagram.
struct stream_rate_view_s
{
    size_t index = 0;
    const char* type = nullptr;
    uint64_t tx_byte = 0; // lifetime unfecced / decoded payload
    uint64_t rx_byte = 0;
    uint64_t tx_pkt = 0;
    uint64_t rx_pkt = 0;
    uint64_t rx_pkt_loss = 0;
    uint64_t air_tx_pkt = 0;
    uint64_t drop_txq = 0;
    char fec[32] = "none";
    bool tcp = false;
    size_t queue = 0;
    size_t unacked = 0;
    uint64_t fec_recovered = 0;
    uint64_t fec_fail = 0;
};

struct channel_info_view_s
{
    bool flow_valid = false;
    uint8_t tx_queue_size = 0;
    uint8_t tx_queue_capacity = 0;
    char flow_t[32] = "-";
    bool air_valid = false;
    int8_t rssi = 0;
    int8_t snr = 0;
    char rssi_t[32] = "-";
    uint64_t tx_byte = 0; // sum of radio on-air bytes (FEC shards + seq)
    uint64_t rx_byte = 0;
    uint64_t tx_pkt = 0;
    uint64_t rx_pkt = 0;
    uint64_t rx_pkt_loss = 0;
    std::vector<stream_rate_view_s> streams;
};

class console_service
{
public:
    using set_fec_fn = std::function<bool(size_t index, fec_type_e type, int k,
                                          int n, std::string* error)>;
    using set_budget_fn =
        std::function<bool(size_t index, size_t budget, std::string* error)>;
    using get_fec_fn = std::function<bool(size_t index, fec_type_e* type,
                                          int* k, int* n, std::string* error)>;
    using get_budget_fn =
        std::function<bool(size_t index, size_t* budget, std::string* error)>;
    using get_ci_fn =
        std::function<bool(channel_info_view_s* out, std::string* error)>;
    using set_modulation_fn =
        std::function<bool(const std::string& name, std::string* error)>;
    using get_modulation_fn =
        std::function<bool(std::string* name, std::string* error)>;
    using set_tx_pacing_fn = std::function<bool(
        const uint32_t* max_rate_kbps, const size_t* max_data_per_tick,
        const size_t* tx_burst_size, const uint32_t* tx_burst_interval_us,
        std::string* error)>;
    using get_tx_pacing_fn = std::function<bool(
        uint32_t* max_rate_kbps, size_t* max_data_per_tick,
        size_t* tx_burst_size, uint32_t* tx_burst_interval_us,
        std::string* error)>;

    ~console_service();

    bool start(::reactor& reactor, const sockaddr_in& console_in,
               const sockaddr_in& console_out, set_fec_fn set_fec,
               set_budget_fn set_budget, get_fec_fn get_fec,
               get_budget_fn get_budget, get_ci_fn get_ci,
               set_modulation_fn set_modulation,
               get_modulation_fn get_modulation,
               set_tx_pacing_fn set_tx_pacing, get_tx_pacing_fn get_tx_pacing,
               std::string* error);
    void stop();

private:
    static constexpr size_t k_line_max = 256;

    void on_datagram();
    void reply(const char* text);
    void reply_ok();
    void reply_ok_args(const char* args);
    void reply_nok(const char* msg);
    void handle_line(const char* line);
    static bool cmd_is(const char* cmd, const char* full, const char* abbrev);

    ::reactor* reactor = nullptr;
    bfc::socket sock;
    sockaddr_in out_addr{};
    set_fec_fn set_fec;
    set_budget_fn set_budget;
    get_fec_fn get_fec;
    get_budget_fn get_budget;
    get_ci_fn get_ci;
    set_modulation_fn set_modulation;
    get_modulation_fn get_modulation;
    set_tx_pacing_fn set_tx_pacing;
    get_tx_pacing_fn get_tx_pacing;
};

#endif  // WINJECT_MANAGER_CONSOLE_SERVICE_H_
