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
    return lock.init();
}

bool udp_logger::ensure_socket()
{
    if (sock.valid())
    {
        return true;
    }
    if (!sock.open_udp(htonl(INADDR_ANY), 0))
    {
        ESP_LOGE(TAG, "udp socket failed: %d", errno);
        return false;
    }
    return true;
}

bool udp_logger::set(ip_port_t dest, log_level_e level)
{
    if (dest.host == 0 || dest.port == 0 || !lock.ready())
    {
        return false;
    }
    bfc::semaphore::lock guard(lock);
    if (!guard)
    {
        return false;
    }
    if (!ensure_socket())
    {
        return false;
    }
    this->dest = dest;
    this->level = level;
    configured = true;
    last_emit_us = 0;
    return true;
}

bool udp_logger::unset()
{
    if (!lock.ready())
    {
        return false;
    }
    bfc::semaphore::lock guard(lock);
    if (!guard)
    {
        return false;
    }
    configured = false;
    dest = {};
    level = log_level_e::warn;
    return true;
}

void udp_logger::fill_status(udp_logger_status_s* out) const
{
    if (out == nullptr)
    {
        return;
    }
    *out = {};
    if (!lock.ready())
    {
        return;
    }
    bfc::semaphore::lock guard(const_cast<bfc::semaphore&>(lock));
    if (!guard)
    {
        return;
    }
    out->set = configured;
    out->dest = dest;
    out->level = level;
    out->emitted = emitted;
    out->dropped = dropped;
}

bool udp_logger::enabled(log_level_e level) const
{
    if (!lock.ready())
    {
        return false;
    }
    bfc::semaphore::lock guard(const_cast<bfc::semaphore&>(lock));
    if (!guard || !configured)
    {
        return false;
    }
    return static_cast<uint8_t>(level) <= static_cast<uint8_t>(level);
}

bool udp_logger::should_emit(log_level_e level)
{
    if (!configured)
    {
        return false;
    }
    if (static_cast<uint8_t>(level) > static_cast<uint8_t>(level))
    {
        return false;
    }
    // Always allow errors through the level gate; still rate-limit to protect
    // Ethernet when the radio is hammering NO_MEM retries that escalate.
    const uint64_t now = static_cast<uint64_t>(esp_timer_get_time());
    if (last_emit_us != 0 && now >= last_emit_us &&
        (now - last_emit_us) < k_min_interval_us)
    {
        dropped++;
        return false;
    }
    last_emit_us = now;
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
    if (fmt == nullptr || !lock.ready())
    {
        return;
    }

    ip_port_t dest = {};
    {
        bfc::semaphore::lock guard(lock);
        if (!guard || !should_emit(level))
        {
            return;
        }
        this->dest = dest;
        if (!ensure_socket())
        {
            dropped++;
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

    bfc::semaphore::lock guard(lock);
    if (!guard || !configured || !sock.valid())
    {
        return;
    }
    const ssize_t sent =
        sock.send(line, used, 0, reinterpret_cast<const sockaddr*>(&addr),
                   sizeof(addr));
    if (sent == static_cast<ssize_t>(used))
    {
        emitted++;
    }
    else
    {
        dropped++;
    }
}
