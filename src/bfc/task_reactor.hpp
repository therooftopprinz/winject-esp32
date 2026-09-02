#ifndef BFC_TASK_REACTOR_HPP_
#define BFC_TASK_REACTOR_HPP_

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <utility>
#include <vector>

#include "bfc/function.hpp"
#include "bfc/task_queue.hpp"
#include "bfc/timer.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace bfc
{

// cv_reactor analog: wait with ulTaskNotifyTake, wake with xTaskNotifyGive.
template <typename cb_t = light_function<void()>>
class task_reactor
{
public:
    using context = reactive_task_queue_base<cb_t>;
    using timer_t = timer<cb_t>;
    using callback_t = cb_t;

    task_reactor(const task_reactor&) = delete;
    task_reactor& operator=(const task_reactor&) = delete;

    explicit task_reactor(uint64_t timeout_ms = 100) : timeout_ms_(timeout_ms)
    {
        ctx_lock_ = xSemaphoreCreateMutex();
        wake_lock_ = xSemaphoreCreateMutex();
    }

    ~task_reactor()
    {
        stop();
        if (ctx_lock_ != nullptr)
        {
            vSemaphoreDelete(ctx_lock_);
        }
        if (wake_lock_ != nullptr)
        {
            vSemaphoreDelete(wake_lock_);
        }
    }

    timer_t& get_timer()
    {
        return timer_;
    }

    bool add_read_rdy(context& ctx, cb_t cb)
    {
        ctx.set_callback(std::move(cb));
        if (ctx_lock_ == nullptr ||
            xSemaphoreTake(ctx_lock_, portMAX_DELAY) != pdTRUE)
        {
            return false;
        }
        if (std::find(contexts_.begin(), contexts_.end(), &ctx) ==
            contexts_.end())
        {
            contexts_.push_back(&ctx);
        }
        xSemaphoreGive(ctx_lock_);
        return true;
    }

    bool remove_read_rdy(context& ctx)
    {
        ctx.set_callback(nullptr);
        if (ctx_lock_ == nullptr ||
            xSemaphoreTake(ctx_lock_, portMAX_DELAY) != pdTRUE)
        {
            return false;
        }
        contexts_.erase(std::remove(contexts_.begin(), contexts_.end(), &ctx),
                        contexts_.end());
        xSemaphoreGive(ctx_lock_);
        return true;
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
        while (running_.load(std::memory_order_acquire))
        {
            TickType_t ticks = pdMS_TO_TICKS(timeout_ms_);
            int64_t next_deadline_us = 0;
            if (timer_.get_next_deadline_us(next_deadline_us))
            {
                const int64_t diff =
                    next_deadline_us - timer_t::current_time_us();
                if (diff <= 0)
                {
                    ticks = 0;
                }
                else
                {
                    const uint64_t diff_ms = static_cast<uint64_t>(diff) / 1000;
                    if (diff_ms < timeout_ms_)
                    {
                        ticks = pdMS_TO_TICKS(diff_ms);
                    }
                }
            }

            const bool pending =
                pending_wake_.exchange(false, std::memory_order_acq_rel);
            uint32_t n = 0;
            if (!pending)
            {
                n = ulTaskNotifyTake(pdTRUE, ticks);
            }
            else
            {
                n = ulTaskNotifyTake(pdTRUE, 0);
                n = n > 0 ? n : 1;
            }

            std::vector<cb_t> cbs;
            if (wake_lock_ != nullptr &&
                xSemaphoreTake(wake_lock_, portMAX_DELAY) == pdTRUE)
            {
                cbs.swap(wake_cbs_);
                xSemaphoreGive(wake_lock_);
            }

            const bool woken = pending || n > 0 || !cbs.empty();
            for (auto& cb : cbs)
            {
                if (cb)
                {
                    cb();
                }
            }

            if (woken && running_.load(std::memory_order_acquire))
            {
                std::vector<context*> ctxs;
                if (ctx_lock_ != nullptr &&
                    xSemaphoreTake(ctx_lock_, portMAX_DELAY) == pdTRUE)
                {
                    ctxs = contexts_;
                    xSemaphoreGive(ctx_lock_);
                }
                for (context* ctx : ctxs)
                {
                    if (ctx != nullptr && ctx->has_data())
                    {
                        ctx->notify_callback();
                    }
                }
            }

            timer_.schedule(timer_t::current_time_us());
        }
        task_.store(nullptr, std::memory_order_release);
    }

    void wake_up(cb_t cb = nullptr)
    {
        if (cb)
        {
            if (wake_lock_ != nullptr &&
                xSemaphoreTake(wake_lock_, portMAX_DELAY) == pdTRUE)
            {
                wake_cbs_.push_back(std::move(cb));
                xSemaphoreGive(wake_lock_);
            }
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

    void stop()
    {
        running_.store(false, std::memory_order_release);
        wake_up();
    }

private:
    uint64_t timeout_ms_ = 100;
    timer_t timer_;
    SemaphoreHandle_t ctx_lock_ = nullptr;
    SemaphoreHandle_t wake_lock_ = nullptr;
    std::vector<context*> contexts_;
    std::vector<cb_t> wake_cbs_;
    std::atomic<bool> running_{false};
    std::atomic<bool> pending_wake_{false};
    std::atomic<TaskHandle_t> task_{nullptr};
};

}  // namespace bfc

#endif  // BFC_TASK_REACTOR_HPP_
