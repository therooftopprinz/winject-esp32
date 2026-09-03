#ifndef BFC_TIMER_HPP_
#define BFC_TIMER_HPP_

#include <chrono>
#include <cstdint>
#include <list>
#include <map>
#include <utility>

#include "bfc-esp32/function.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace bfc
{

template <typename cb_t = light_function<void()>>
class timer
{
public:
    using timer_id_t = std::pair<int64_t, uint64_t>;

    timer()
    {
        lock_ = xSemaphoreCreateMutex();
    }

    ~timer()
    {
        if (lock_ != nullptr)
        {
            vSemaphoreDelete(lock_);
        }
    }

    timer(const timer&) = delete;
    timer& operator=(const timer&) = delete;

    timer_id_t wait_us(int64_t for_us, cb_t cb,
                       int64_t now_us = current_time_us())
    {
        const int64_t next_us = now_us + for_us;
        timer_id_t id{next_us, 0};
        if (lock_ == nullptr || xSemaphoreTake(lock_, portMAX_DELAY) != pdTRUE)
        {
            return id;
        }
        id.second = timer_ctr_++;
        cb_map_.emplace(id, std::move(cb));
        xSemaphoreGive(lock_);
        return id;
    }

    timer_id_t wait_ms(int64_t for_ms, cb_t cb,
                       int64_t now_us = current_time_us())
    {
        return wait_us(for_ms * 1000, std::move(cb), now_us);
    }

    bool get_next_deadline_us(int64_t& deadline_us) const
    {
        if (lock_ == nullptr || xSemaphoreTake(lock_, portMAX_DELAY) != pdTRUE)
        {
            return false;
        }
        const bool ok = !cb_map_.empty();
        if (ok)
        {
            deadline_us = cb_map_.begin()->first.first;
        }
        xSemaphoreGive(lock_);
        return ok;
    }

    bool get_next_deadline_ms(int64_t& deadline_ms) const
    {
        return get_next_deadline_us(deadline_ms);
    }

    bool cancel(timer_id_t id)
    {
        if (lock_ == nullptr || xSemaphoreTake(lock_, portMAX_DELAY) != pdTRUE)
        {
            return false;
        }
        const bool erased = cb_map_.erase(id) != 0;
        xSemaphoreGive(lock_);
        return erased;
    }

    void schedule(int64_t now_us = current_time_us())
    {
        using node_type = typename std::map<timer_id_t, cb_t>::node_type;
        std::list<node_type> extracted;
        if (lock_ == nullptr || xSemaphoreTake(lock_, portMAX_DELAY) != pdTRUE)
        {
            return;
        }
        auto it = cb_map_.begin();
        while (it != cb_map_.end())
        {
            auto next = it;
            ++next;
            if (now_us >= it->first.first)
            {
                extracted.emplace_back(cb_map_.extract(it));
                it = next;
                continue;
            }
            break;
        }
        xSemaphoreGive(lock_);

        for (auto& node : extracted)
        {
            if (node.mapped())
            {
                node.mapped()();
            }
        }
    }

    static int64_t current_time_us()
    {
        using namespace std::chrono;
        return duration_cast<microseconds>(
                   steady_clock::now().time_since_epoch())
            .count();
    }

    static int64_t current_time_ms()
    {
        return current_time_us();
    }

private:
    uint64_t timer_ctr_ = 0;
    mutable SemaphoreHandle_t lock_ = nullptr;
    std::map<timer_id_t, cb_t> cb_map_;
};

}  // namespace bfc

#endif  // BFC_TIMER_HPP_
