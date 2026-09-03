#ifndef WINJECT_CONSOLE_H_
#define WINJECT_CONSOLE_H_

#include <stddef.h>
#include <stdint.h>

#include "bfc-esp32/select_reactor.hpp"
#include "bfc-esp32/timer.hpp"

class upstream_rx;
class upstream_tx;
class manager;
struct wifi_status_s;

class console
{
public:
    console() = default;
    console(const console&) = delete;
    console& operator=(const console&) = delete;

    bool init(upstream_rx& rx, upstream_tx& tx, manager& netmgr);

private:
    using reactor_t = bfc::select_reactor<>;
    using timer_id_t = bfc::timer<>::timer_id_t;

    struct out_s
    {
        int fd;
    };

    struct tcp_client_s
    {
        int fd = -1;
        char line[128]{};
        size_t len = 0;
    };

    static constexpr size_t k_max_tcp_clients = 4;
    static constexpr int k_sync_ms = 250;

    void attach_reactor();
    void schedule_sync();
    void sync_tcp();
    void watch_client(int fd);

    tcp_client_s* client_by_fd(int fd);
    size_t client_count() const;
    bool attach_client(int fd);
    void close_client(int fd);
    void close_all_clients();
    void write(const out_s& out, const char* text);
    void print(const out_s& out, const char* fmt, ...);
    void print_mac(const out_s& out, const uint8_t mac[6]);
    void print_banner(const out_s& out);
    void print_help(const out_s& out);
    void print_path_metrics(const out_s& out, const wifi_status_s& radio);
    void print_status(const out_s& out);
    void handle_line(const char* line, const out_s& out);
    void feed_char(char c, char* line, size_t* len, size_t max_len,
                   const out_s& out);
    void start_tcp();
    void stop_tcp();
    void accept_clients();
    void poll_client(int fd);

    upstream_rx* rx_ = nullptr;
    upstream_tx* tx_ = nullptr;
    manager* netmgr_ = nullptr;
    reactor_t* reactor_ = nullptr;
    bool ready_ = false;
    int listen_fd_ = -1;
    tcp_client_s clients_[k_max_tcp_clients];
};

#endif  // WINJECT_CONSOLE_H_
