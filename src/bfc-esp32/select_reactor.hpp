#ifndef BFC_SELECT_REACTOR_HPP_
#define BFC_SELECT_REACTOR_HPP_

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "bfc-esp32/function.hpp"
#include "bfc-esp32/socket.hpp"
#include "bfc-esp32/timer.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

namespace bfc
{

// poll_reactor analog using lwIP select(). Write watches are one-shot
// (re-arm with req_write). Wake uses a UDP loopback datagram.
template <typename cb_t = light_function<void()>>
class select_reactor
{
public:
    using fd_t = int;
    using timer_t = timer<cb_t>;

    select_reactor(const select_reactor&) = delete;
    select_reactor& operator=(const select_reactor&) = delete;

    select_reactor()
    {
        lock_ = xSemaphoreCreateMutex();
    }

    ~select_reactor()
    {
        stop();
        wake_sock_.close();
        if (lock_ != nullptr)
        {
            vSemaphoreDelete(lock_);
        }
    }

    int get_last_error_code()
    {
        return errno;
    }

    bool add_read_rdy(fd_t fd, cb_t cb)
    {
        if (fd < 0)
        {
            return false;
        }
        fd_entry_s& entry = find_or_add(fd);
        entry.read_cb = std::move(cb);
        entry.read_active = true;
        return true;
    }

    bool rem_read_rdy(fd_t fd, cb_t done_cb = nullptr)
    {
        queue_rem(fd, true, std::move(done_cb));
        wake_up();
        return true;
    }

    bool req_read(fd_t)
    {
        return true;
    }

    bool add_write_rdy(fd_t fd, cb_t cb)
    {
        if (fd < 0)
        {
            return false;
        }
        fd_entry_s& entry = find_or_add(fd);
        entry.write_cb = std::move(cb);
        entry.write_active = true;
        entry.write_armed = false;
        return true;
    }

    bool rem_write_rdy(fd_t fd, cb_t done_cb = nullptr)
    {
        queue_rem(fd, false, std::move(done_cb));
        wake_up();
        return true;
    }

    bool req_write(fd_t fd)
    {
        fd_entry_s* entry = find(fd);
        if (entry == nullptr || !entry->write_active)
        {
            return false;
        }
        entry->write_armed = true;
        return true;
    }

    void wake_up(cb_t cb = nullptr)
    {
        if (cb)
        {
            if (lock_ != nullptr &&
                xSemaphoreTake(lock_, portMAX_DELAY) == pdTRUE)
            {
                wake_cbs_.push_back(std::move(cb));
                xSemaphoreGive(lock_);
            }
        }
        ensure_wake();
        if (wake_sock_.valid())
        {
            const uint8_t one = 1;
            wake_sock_.send(&one, sizeof(one), 0,
                            reinterpret_cast<const sockaddr*>(&wake_addr_),
                            sizeof(wake_addr_));
            return;
        }
        const TaskHandle_t task = task_.load(std::memory_order_acquire);
        if (task != nullptr)
        {
            xTaskNotifyGive(task);
        }
        else
        {
            pending_wake_.store(true, std::memory_order_release);
        }
    }

    bool is_reactor_thread() const
    {
        const TaskHandle_t task = task_.load(std::memory_order_acquire);
        return task != nullptr && xTaskGetCurrentTaskHandle() == task;
    }

    void run()
    {
        task_.store(xTaskGetCurrentTaskHandle(), std::memory_order_release);
        running_.store(true, std::memory_order_release);
        ensure_wake();

        while (running_.load(std::memory_order_acquire))
        {
            apply_pending_rem();

            int timeout_ms = -1;
            int64_t next_deadline_us = 0;
            if (timer_.get_next_deadline_us(next_deadline_us))
            {
                const int64_t diff =
                    next_deadline_us - timer_t::current_time_us();
                if (diff <= 0)
                {
                    timeout_ms = 0;
                }
                else
                {
                    const int64_t diff_ms = diff / 1000;
                    if (diff_ms > std::numeric_limits<int>::max())
                    {
                        timeout_ms = std::numeric_limits<int>::max();
                    }
                    else
                    {
                        timeout_ms = static_cast<int>(diff_ms);
                    }
                }
            }
            if (!wake_sock_.valid() && timeout_ms < 0)
            {
                timeout_ms = 100;
            }

            fd_set readfds;
            fd_set writefds;
            FD_ZERO(&readfds);
            FD_ZERO(&writefds);
            int maxfd = -1;

            auto watch = [&](int fd, fd_set* set)
            {
                if (fd < 0)
                {
                    return;
                }
                FD_SET(fd, set);
                if (fd > maxfd)
                {
                    maxfd = fd;
                }
            };

            if (wake_sock_.valid())
            {
                watch(wake_sock_.fd(), &readfds);
            }
            for (fd_entry_s& entry : entries_)
            {
                if (entry.read_active)
                {
                    watch(entry.fd, &readfds);
                }
                if (entry.write_active && entry.write_armed)
                {
                    watch(entry.fd, &writefds);
                }
            }

            timeval tv = {};
            timeval* tvp = nullptr;
            if (timeout_ms >= 0)
            {
                tv.tv_sec = timeout_ms / 1000;
                tv.tv_usec = (timeout_ms % 1000) * 1000;
                tvp = &tv;
            }

            int nfds = 0;
            if (maxfd >= 0)
            {
                nfds = ::select(maxfd + 1, &readfds, &writefds, nullptr, tvp);
                if (nfds < 0 && errno == EINTR)
                {
                    continue;
                }
            }
            else
            {
                const bool pending =
                    pending_wake_.exchange(false, std::memory_order_acq_rel);
                if (!pending)
                {
                    ulTaskNotifyTake(pdTRUE, timeout_ms < 0
                                                 ? portMAX_DELAY
                                                 : pdMS_TO_TICKS(timeout_ms));
                }
            }

            if (wake_sock_.valid() && FD_ISSET(wake_sock_.fd(), &readfds))
            {
                uint8_t tmp[32];
                while (wake_sock_.recv(tmp, sizeof(tmp)) > 0)
                {
                }
            }

            if (nfds > 0)
            {
                for (fd_entry_s& entry : entries_)
                {
                    if (entry.read_active && FD_ISSET(entry.fd, &readfds) &&
                        entry.read_cb)
                    {
                        entry.read_cb();
                    }
                    if (entry.write_active && entry.write_armed &&
                        FD_ISSET(entry.fd, &writefds) && entry.write_cb)
                    {
                        entry.write_armed = false;
                        entry.write_cb();
                    }
                }
            }

            std::vector<cb_t> cbs;
            if (lock_ != nullptr &&
                xSemaphoreTake(lock_, portMAX_DELAY) == pdTRUE)
            {
                cbs.swap(wake_cbs_);
                xSemaphoreGive(lock_);
            }
            for (auto& cb : cbs)
            {
                if (cb)
                {
                    cb();
                }
            }

            apply_pending_rem();
            timer_.schedule(timer_t::current_time_us());
        }

        task_.store(nullptr, std::memory_order_release);
    }

    void stop()
    {
        running_.store(false, std::memory_order_release);
        wake_up();
    }

    timer_t& get_timer()
    {
        return timer_;
    }

private:
    struct fd_entry_s
    {
        int fd = -1;
        cb_t read_cb = nullptr;
        cb_t write_cb = nullptr;
        bool read_active = false;
        bool write_active = false;
        bool write_armed = false;
    };

    struct pending_rem_s
    {
        int fd = -1;
        bool read = true;
        cb_t done = nullptr;
    };

    fd_entry_s* find(int fd)
    {
        for (fd_entry_s& entry : entries_)
        {
            if (entry.fd == fd)
            {
                return &entry;
            }
        }
        return nullptr;
    }

    fd_entry_s& find_or_add(int fd)
    {
        fd_entry_s* existing = find(fd);
        if (existing != nullptr)
        {
            return *existing;
        }
        entries_.push_back(fd_entry_s{});
        entries_.back().fd = fd;
        return entries_.back();
    }

    void queue_rem(int fd, bool read, cb_t done)
    {
        if (lock_ == nullptr || xSemaphoreTake(lock_, portMAX_DELAY) != pdTRUE)
        {
            return;
        }
        pending_rem_.push_back(pending_rem_s{fd, read, std::move(done)});
        xSemaphoreGive(lock_);
    }

    void apply_pending_rem()
    {
        std::vector<pending_rem_s> pending;
        if (lock_ != nullptr && xSemaphoreTake(lock_, portMAX_DELAY) == pdTRUE)
        {
            pending.swap(pending_rem_);
            xSemaphoreGive(lock_);
        }
        for (pending_rem_s& rem : pending)
        {
            fd_entry_s* entry = find(rem.fd);
            if (entry != nullptr)
            {
                if (rem.read)
                {
                    entry->read_active = false;
                    entry->read_cb = nullptr;
                }
                else
                {
                    entry->write_active = false;
                    entry->write_armed = false;
                    entry->write_cb = nullptr;
                }
            }
            if (rem.done)
            {
                rem.done();
            }
        }
    }

    bool ensure_wake()
    {
        if (wake_sock_.valid())
        {
            return true;
        }
        if (!wake_sock_.open_udp(htonl(INADDR_LOOPBACK), 0))
        {
            return false;
        }
        socklen_t len = sizeof(wake_addr_);
        if (getsockname(wake_sock_.fd(),
                        reinterpret_cast<sockaddr*>(&wake_addr_), &len) != 0)
        {
            wake_sock_.close();
            return false;
        }
        return true;
    }

    timer_t timer_;
    socket wake_sock_;
    sockaddr_in wake_addr_{};
    SemaphoreHandle_t lock_ = nullptr;
    std::vector<fd_entry_s> entries_;
    std::vector<cb_t> wake_cbs_;
    std::vector<pending_rem_s> pending_rem_;
    std::atomic<bool> running_{false};
    std::atomic<bool> pending_wake_{false};
    std::atomic<TaskHandle_t> task_{nullptr};
};

}  // namespace bfc

#endif  // BFC_SELECT_REACTOR_HPP_
