#ifndef SIM_WINJECT_RADIO_SIM_HPP_
#define SIM_WINJECT_RADIO_SIM_HPP_

#include "winject-esp32/config.h"
#include "winject-esp32/radio/frame.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <netinet/in.h>

struct ip_port_s
{
    uint32_t host = 0;  // network byte order
    uint16_t port = 0;
};

struct mpdu_buf_s
{
    std::array<uint8_t, WIFI_RADIO_INJECT_MAX> data{};
    size_t len = 0;
};

// Behavioral WT32 radio: UDP console contract + inject/forward + air UDP.
class radio_sim
{
public:
    static constexpr size_t k_tx_queue = WIFI_RADIO_TX_QUEUE;
    static constexpr size_t k_rx_queue = WIFI_RADIO_RX_QUEUE;
    static constexpr size_t k_ci_max = WIFI_AIRPORT_MAX;

    radio_sim();
    ~radio_sim();

    radio_sim(const radio_sim&) = delete;
    radio_sim& operator=(const radio_sim&) = delete;

    bool start(const sockaddr_in& console_bind, const sockaddr_in& air_bind,
               const std::vector<sockaddr_in>& air_peers, uint32_t display_ip);
    void stop();

    int console_fd() const
    {
        return console_fd_;
    }
    int inject_fd() const
    {
        return inject_fd_;
    }
    int air_fd() const
    {
        return air_fd_;
    }

    void on_console_readable();
    void on_inject_readable();
    void on_air_readable();

    // Console command surface (one line, no trailing newline required).
    std::string handle_command(const char* line);

    uint32_t display_ip() const
    {
        return display_ip_;
    }

private:
    void tx_worker();
    void rx_forward_worker();
    void maybe_emit_flow_ctrl(size_t qsize);
    void emit_ci(const void* data, size_t len);
    bool radio_active() const;
    std::string status_text();
    std::string help_text() const;

    bool set_upstream_tx(uint16_t port);
    bool clear_upstream_tx();
    bool set_upstream_rx(ip_port_s dest);
    bool clear_upstream_rx();
    bool add_ci(ip_port_s dest);
    bool rem_ci(ip_port_s dest);

    static int open_udp_bound(const sockaddr_in& addr, bool reuse);
    static bool parse_bool(const char* text, bool* out);
    static bool parse_host(const char* text, uint32_t* host_nbo);
    static bool parse_port(const char* text, uint16_t* port);
    static bool parse_domain(const char* text, uint16_t* domain);
    static void ipv4_to_string(uint32_t host_nbo, char* out, size_t n);

    int console_fd_ = -1;
    int inject_fd_ = -1;
    int air_fd_ = -1;
    int forward_fd_ = -1;
    int ci_fd_ = -1;

    sockaddr_in console_bind_{};
    std::vector<sockaddr_in> air_peers_;
    uint32_t display_ip_ = 0;  // network byte order, for status

    std::mutex state_mu_;
    WinjectMode mode_ = WINJECT_MODE_BFC_TUNNEL_DEVICE;
    uint16_t domain_ = 0;
    uint8_t channel_ = WIFI_DEFAULT_CHANNEL;
    std::string modulation_ = WIFI_DEFAULT_MODULATION;
    bool cca_enabled_ = true;
    int tx_power_dbm_ = WIFI_DEFAULT_TX_POWER_DBM;
    bool allow_failed_crc_ = false;
    bool net_static_ = false;
    uint32_t static_ip_ = 0;  // nbo; 0 → display_ip_

    bool have_sut_ = false;
    uint16_t sut_port_ = 0;
    bool have_sur_ = false;
    ip_port_s sur_{};
    std::vector<ip_port_s> ci_subs_;

    // Logger (runtime only).
    bool have_logger_ = false;
    ip_port_s logger_{};
    std::string logger_level_ = "info";

    // Queues + stats.
    std::mutex tx_mu_;
    std::condition_variable tx_cv_;
    std::deque<mpdu_buf_s> tx_q_;
    std::atomic<bool> stop_{false};
    std::thread tx_thread_;

    std::mutex rx_mu_;
    std::condition_variable rx_cv_;
    std::deque<mpdu_buf_s> rx_q_;
    std::thread rx_thread_;

    std::atomic<uint32_t> drop_tx_queue_full_{0};
    std::atomic<uint32_t> drop_rx_queue_full_{0};
    std::atomic<uint32_t> drop_inject_size_{0};
    std::atomic<uint32_t> inject_ok_{0};
    std::atomic<uint32_t> inject_fail_{0};
    std::atomic<uint32_t> udp_tx_pkt_{0};
    std::atomic<uint32_t> udp_fwd_pkt_{0};
    std::atomic<uint32_t> drop_send_fail_{0};
    std::atomic<uint32_t> air_rx_pkt_{0};
    std::atomic<uint32_t> air_tx_pkt_{0};
    std::atomic<uint64_t> last_tx_latency_us_{0};
    std::atomic<bool> tx_latency_valid_{false};

    std::chrono::steady_clock::time_point started_at_{};
};

#endif  // SIM_WINJECT_RADIO_SIM_HPP_
