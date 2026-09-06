#ifndef WINJECT_UDP_LOGGER_H_
#define WINJECT_UDP_LOGGER_H_

#include "packet.h"

#include <stdarg.h>
#include <stdint.h>

#include "bfc-esp32/semaphore.hpp"
#include "bfc-esp32/socket.hpp"

enum class log_level_e : uint8_t
{
    error = 0,
    warn = 1,
    info = 2,
    debug = 3,
};

struct udp_logger_status_s
{
    bool set;
    ip_port_t dest;
    log_level_e level;
    uint32_t emitted;
    uint32_t dropped;
};

class udp_logger
{
public:
    static udp_logger& instance();
    udp_logger(const udp_logger&) = delete;
    udp_logger& operator=(const udp_logger&) = delete;

    bool init();
    bool set(ip_port_t dest, log_level_e level);
    bool unset();
    void fill_status(udp_logger_status_s* out) const;

    bool enabled(log_level_e level) const;
    void log(log_level_e level, const char* fmt, ...)
        __attribute__((format(printf, 3, 4)));
    void vlog(log_level_e level, const char* fmt, va_list args);

    static const char* level_name(log_level_e level);
    static bool parse_level(const char* text, log_level_e* out);

private:
    udp_logger() = default;

    bool ensure_socket();
    bool should_emit(log_level_e level);

    static constexpr uint32_t k_min_interval_us = 20000;  // 50 Hz cap
    static constexpr size_t k_line_max = 192;

    bfc::semaphore lock_;
    bfc::socket sock_;
    ip_port_t dest_{};
    log_level_e level_ = log_level_e::warn;
    bool set_ = false;
    uint32_t emitted_ = 0;
    uint32_t dropped_ = 0;
    uint64_t last_emit_us_ = 0;
};

#endif  // WINJECT_UDP_LOGGER_H_
