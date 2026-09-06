#ifndef BFC_TASK_REACTOR_HPP_
#define BFC_TASK_REACTOR_HPP_

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <utility>
#include <vector>

#include "bfc-esp32/function.hpp"
#include "bfc-esp32/task_queue.hpp"
#include "bfc-esp32/timer.hpp"
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

    explicit task_reactor(uint64_t timeout_ms = 100) : timeout_ms(timeout_ms)
    {
        ctx_lock = xSemaphoreCreateMutex();
        wake_lock = xSemaphoreCreateMutex();
    }

    ~task_reactor()
    {
        stop();
        if (ctx_lock != nullptr)
        {
            vSemaphoreDelete(ctx_lock);
        }
        if (wake_lock != nullptr)
        {
            vSemaphoreDelete(wake_lock);
        }
    }

    timer_t& get_timer()
    {
        return timer;
    }

    bool add_read_rdy(context& ctx, cb_t cb)
    {
        ctx.set_callback(std::move(cb));
        if (ctx_lock == nullptr ||
            xSemaphoreTake(ctx_lock, portMAX_DELAY) != pdTRUE)
        {
            return false;
        }
        if (std::find(contexts.begin(), contexts.end(), &ctx) ==
            contexts.end())
        {
            contexts.push_back(&ctx);
        }
        xSemaphoreGive(ctx_lock);
        return true;
    }

    bool remove_read_rdy(context& ctx)
    {
        ctx.set_callback(nullptr);
        if (ctx_lock == nullptr ||
            xSemaphoreTake(ctx_lock, portMAX_DELAY) != pdTRUE)
        {
            return false;
        }
        contexts.erase(std::remove(contexts.begin(), contexts.end(), &ctx),
                        contexts.end());
        xSemaphoreGive(ctx_lock);
        return true;
    }

    bool is_reactor_thread() const
    {
        const TaskHandle_t task = task.load(std::memory_order_acquire);
        return task != nullptr && xTaskGetCurrentTaskHandle() == task;
    }

    bool start_pinned(const char* name, BaseType_t core, UBaseType_t prio,
                      uint32_t stack_bytes = 6144)
    {
        bool expected = false;
        if (!pinned.compare_exchange_strong(expected, true,
                                             std::memory_order_acq_rel))
        {
            return true;
        }
        const char* task_name = (name != nullptr && name[0] != '\0')
                                    ? name
                                    : "task_reactor";
        if (xTaskCreatePinnedToCore(pinned_task, task_name, stack_bytes, this,
                                    prio, nullptr, core) != pdPASS)
        {
            pinned.store(false, std::memory_order_release);
            return false;
        }
        return true;
    }

    void run()
    {
        task.store(xTaskGetCurrentTaskHandle(), std::memory_order_release);
        running.store(true, std::memory_order_release);
        while (running.load(std::memory_order_acquire))
        {
            TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
            int64_t next_deadline_us = 0;
            if (timer.get_next_deadline_us(next_deadline_us))
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
                    if (diff_ms < timeout_ms)
                    {
                        ticks = pdMS_TO_TICKS(diff_ms);
                    }
                }
            }

            const bool pending =
                pending_wake.exchange(false, std::memory_order_acq_rel);
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
            if (wake_lock != nullptr &&
                xSemaphoreTake(wake_lock, portMAX_DELAY) == pdTRUE)
            {
                cbs.swap(wake_cbs);
                xSemaphoreGive(wake_lock);
            }

            const bool woken = pending || n > 0 || !cbs.empty();
            for (auto& cb : cbs)
            {
                if (cb)
                {
                    cb();
                }
            }

            if (woken && running.load(std::memory_order_acquire))
            {
                std::vector<context*> ctxs;
                if (ctx_lock != nullptr &&
                    xSemaphoreTake(ctx_lock, portMAX_DELAY) == pdTRUE)
                {
                    ctxs = contexts;
                    xSemaphoreGive(ctx_lock);
                }
                for (context* ctx : ctxs)
                {
                    if (ctx != nullptr && ctx->has_data())
                    {
                        ctx->notify_callback();
                    }
                }
            }

            timer.schedule(timer_t::current_time_us());
        }
        task.store(nullptr, std::memory_order_release);
    }

    void wake_up(cb_t cb = nullptr)
    {
        if (cb)
        {
            if (wake_lock != nullptr &&
                xSemaphoreTake(wake_lock, portMAX_DELAY) == pdTRUE)
            {
                wake_cbs.push_back(std::move(cb));
                xSemaphoreGive(wake_lock);
            }
        }

        const TaskHandle_t task = task.load(std::memory_order_acquire);
        if (task != nullptr)
        {
            xTaskNotifyGive(task);
        }
        else
        {
            pending_wake.store(true, std::memory_order_release);
        }
    }

    void stop()
    {
        running.store(false, std::memory_order_release);
        wake_up();
    }

private:
    static void pinned_task(void* arg)
    {
        static_cast<task_reactor*>(arg)->run();
        vTaskDelete(nullptr);
    }

    uint64_t timeout_ms = 100;
    timer_t timer;
    SemaphoreHandle_t ctx_lock = nullptr;
    SemaphoreHandle_t wake_lock = nullptr;
    std::vector<context*> contexts;
    std::vector<cb_t> wake_cbs;
    std::atomic<bool> running{false};
    std::atomic<bool> pending_wake{false};
    std::atomic<bool> pinned{false};
    std::atomic<TaskHandle_t> task{nullptr};
};

}  // namespace bfc

#endif  // BFC_TASK_REACTOR_HPP_
