#ifndef WINJECT_CONSOLE_H_
#define WINJECT_CONSOLE_H_

#include <stddef.h>
#include <stdint.h>

#include "bfc-esp32/select_reactor.hpp"
#include "lwip/sockets.h"

class lc_tx_endpoint;
class lc_rx_endpoint;
class channel_info_endpoint;
class manager;
struct wifi_status_s;
struct lc_tx_bind_s;
struct lc_rx_bind_s;

class console
{
public:
    console() = default;
    console(const console&) = delete;
    console& operator=(const console&) = delete;

    bool init(manager& netmgr);
    bool init(lc_tx_endpoint& tx_ep, lc_rx_endpoint& rx_ep,
              channel_info_endpoint& ci, manager& netmgr);

private:
    using reactor_t = bfc::select_reactor<>;

    static constexpr int k_sync_ms = 250;
    // One Ethernet MTU UDP datagram (IP+UDP headers leave ~1472).
    static constexpr size_t k_datagram_max = 1500;
    static constexpr size_t k_reply_max = 16384;
    static constexpr int64_t k_send_timeout_us = 2000000;

    bool init_common(manager& netmgr);
    void attach_reactor();
    void schedule_sync();
    void sync_udp();
    void start_udp();
    void stop_udp();
    void unwatch_fd(int fd);
    void on_datagram();

    void write(const char* text);
    void write_bytes(const void* data, size_t n);
    void flush_reply();
    void print(const char* fmt, ...);
    void reply_ok();
    void reply_ok_args(const char* args);
    void reply_nok(const char* msg);
    void reply_usage(const char* usage);
    void reply_done(bool ok, const char* fail);

    void print_help();
    void print_upstreams(const lc_tx_bind_s* sut, bool have_sut,
                         const lc_rx_bind_s* sur, bool have_sur);
    void print_channel_metrics(const wifi_status_s& radio,
                               const lc_tx_bind_s* sut, bool have_sut,
                               const lc_rx_bind_s* sur, bool have_sur);
    void print_hardware();
    void print_status();
    void handle_datagram(char* buf, size_t n);
    void handle_line(char* line);
    void handle_ether_test_tx(char* arg1, char* extra);
    void handle_ether_test_rx(char* buf, size_t n);

    bool radio_ready() const;
    bool require_radio();
    void reboot_after_ok();

    lc_tx_endpoint* tx_ep = nullptr;
    lc_rx_endpoint* rx_ep = nullptr;
    channel_info_endpoint* ci = nullptr;
    manager* netmgr = nullptr;
    reactor_t* reactor = nullptr;
    bool ready = false;
    int sock_fd = -1;
    sockaddr_in reply_to{};
    bool reply_to_set = false;
    char reply[k_reply_max]{};
    size_t reply_len = 0;
    char recv_buf[k_datagram_max]{};
};

#endif  // WINJECT_CONSOLE_H_
