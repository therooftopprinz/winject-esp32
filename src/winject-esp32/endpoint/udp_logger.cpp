#include "udp_logger.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"

static const char* TAG = "udp_log";

udp_logger& udp_logger::instance()
{
    static udp_logger inst;
    return inst;
}

const char* udp_logger::level_name(log_level_e level)
{
    switch (level)
    {
    case log_level_e::error:
        return "error";
    case log_level_e::warn:
        return "warn";
    case log_level_e::info:
        return "info";
    case log_level_e::debug:
        return "debug";
    }
    return "?";
}

bool udp_logger::parse_level(const char* text, log_level_e* out)
{
    if (text == nullptr || out == nullptr || *text == '\0')
    {
        return false;
    }
    if (strcasecmp(text, "error") == 0 || strcasecmp(text, "err") == 0 ||
        strcasecmp(text, "e") == 0)
    {
        *out = log_level_e::error;
        return true;
    }
    if (strcasecmp(text, "warn") == 0 || strcasecmp(text, "warning") == 0 ||
        strcasecmp(text, "w") == 0)
    {
        *out = log_level_e::warn;
        return true;
    }
    if (strcasecmp(text, "info") == 0 || strcasecmp(text, "i") == 0)
    {
        *out = log_level_e::info;
        return true;
    }
    if (strcasecmp(text, "debug") == 0 || strcasecmp(text, "dbg") == 0 ||
        strcasecmp(text, "d") == 0)
    {
        *out = log_level_e::debug;
        return true;
    }
    return false;
}

bool udp_logger::init()
{
    return lock_.init();
}

bool udp_logger::ensure_socket()
{
    if (sock_.valid())
    {
        return true;
    }
    if (!sock_.open_udp(htonl(INADDR_ANY), 0))
    {
        ESP_LOGE(TAG, "udp socket failed: %d", errno);
        return false;
    }
    return true;
}

bool udp_logger::set(ip_port_t dest, log_level_e level)
{
    if (dest.host == 0 || dest.port == 0 || !lock_.ready())
    {
        return false;
    }
    bfc::semaphore::lock guard(lock_);
    if (!guard)
    {
        return false;
    }
    if (!ensure_socket())
    {
        return false;
    }
    dest_ = dest;
    level_ = level;
    set_ = true;
    last_emit_us_ = 0;
    return true;
}

bool udp_logger::unset()
{
    if (!lock_.ready())
    {
        return false;
    }
    bfc::semaphore::lock guard(lock_);
    if (!guard)
    {
        return false;
    }
    set_ = false;
    dest_ = {};
    level_ = log_level_e::warn;
    return true;
}

void udp_logger::fill_status(udp_logger_status_s* out) const
{
    if (out == nullptr)
    {
        return;
    }
    *out = {};
    if (!lock_.ready())
    {
        return;
    }
    bfc::semaphore::lock guard(const_cast<bfc::semaphore&>(lock_));
    if (!guard)
    {
        return;
    }
    out->set = set_;
    out->dest = dest_;
    out->level = level_;
    out->emitted = emitted_;
    out->dropped = dropped_;
}

bool udp_logger::enabled(log_level_e level) const
{
    if (!lock_.ready())
    {
        return false;
    }
    bfc::semaphore::lock guard(const_cast<bfc::semaphore&>(lock_));
    if (!guard || !set_)
    {
        return false;
    }
    return static_cast<uint8_t>(level) <= static_cast<uint8_t>(level_);
}

bool udp_logger::should_emit(log_level_e level)
{
    if (!set_)
    {
        return false;
    }
    if (static_cast<uint8_t>(level) > static_cast<uint8_t>(level_))
    {
        return false;
    }
    // Always allow errors through the level gate; still rate-limit to protect
    // Ethernet when the radio is hammering NO_MEM retries that escalate.
    const uint64_t now = static_cast<uint64_t>(esp_timer_get_time());
    if (last_emit_us_ != 0 && now >= last_emit_us_ &&
        (now - last_emit_us_) < k_min_interval_us)
    {
        dropped_++;
        return false;
    }
    last_emit_us_ = now;
    return true;
}

void udp_logger::log(log_level_e level, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vlog(level, fmt, args);
    va_end(args);
}

void udp_logger::vlog(log_level_e level, const char* fmt, va_list args)
{
    if (fmt == nullptr || !lock_.ready())
    {
        return;
    }

    ip_port_t dest = {};
    {
        bfc::semaphore::lock guard(lock_);
        if (!guard || !should_emit(level))
        {
            return;
        }
        dest = dest_;
        if (!ensure_socket())
        {
            dropped_++;
            return;
        }
    }

    char line[k_line_max];
    const uint64_t us = static_cast<uint64_t>(esp_timer_get_time());
    int n = snprintf(line, sizeof(line), "%llu %s ",
                     static_cast<unsigned long long>(us), level_name(level));
    if (n < 0)
    {
        return;
    }
    size_t used = static_cast<size_t>(n);
    if (used >= sizeof(line))
    {
        return;
    }
    n = vsnprintf(line + used, sizeof(line) - used, fmt, args);
    if (n < 0)
    {
        return;
    }
    used += static_cast<size_t>(n);
    if (used >= sizeof(line))
    {
        used = sizeof(line) - 1;
    }
    if (used + 1 < sizeof(line))
    {
        line[used++] = '\n';
        line[used] = '\0';
    }
    else
    {
        line[sizeof(line) - 2] = '\n';
        line[sizeof(line) - 1] = '\0';
        used = sizeof(line) - 1;
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = dest.host;
    addr.sin_port = htons(dest.port);

    bfc::semaphore::lock guard(lock_);
    if (!guard || !set_ || !sock_.valid())
    {
        return;
    }
    const ssize_t sent =
        sock_.send(line, used, 0, reinterpret_cast<const sockaddr*>(&addr),
                   sizeof(addr));
    if (sent == static_cast<ssize_t>(used))
    {
        emitted_++;
    }
    else
    {
        dropped_++;
    }
}
