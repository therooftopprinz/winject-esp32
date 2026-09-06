#ifndef BFC_TASK_QUEUE_HPP_
#define BFC_TASK_QUEUE_HPP_

#include <utility>
#include <vector>

#include "bfc-esp32/function.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace bfc
{

template <typename cb_t>
class reactive_task_queue_base
{
public:
    using callback_t = cb_t;
    virtual ~reactive_task_queue_base() = default;
    virtual void set_callback(callback_t cb) = 0;
    virtual bool has_data() = 0;
    virtual void notify_callback() = 0;
};

template <typename T, typename cb_t>
class reactive_task_queue : public reactive_task_queue_base<cb_t>
{
public:
    reactive_task_queue()
    {
        queue_lock = xSemaphoreCreateMutex();
        cb_lock = xSemaphoreCreateMutex();
    }

    ~reactive_task_queue()
    {
        if (queue_lock != nullptr)
        {
            vSemaphoreDelete(queue_lock);
        }
        if (cb_lock != nullptr)
        {
            vSemaphoreDelete(cb_lock);
        }
    }

    reactive_task_queue(const reactive_task_queue&) = delete;
    reactive_task_queue& operator=(const reactive_task_queue&) = delete;

    template <typename U>
    size_t push(U&& u)
    {
        if (queue_lock == nullptr ||
            xSemaphoreTake(queue_lock, portMAX_DELAY) != pdTRUE)
        {
            return 0;
        }
        queue.emplace_back(std::forward<U>(u));
        const size_t n = queue.size();
        xSemaphoreGive(queue_lock);
        return n;
    }

    std::vector<T> pop()
    {
        std::vector<T> out;
        if (queue_lock == nullptr ||
            xSemaphoreTake(queue_lock, portMAX_DELAY) != pdTRUE)
        {
            return out;
        }
        out = std::move(queue);
        xSemaphoreGive(queue_lock);
        return out;
    }

    size_t size()
    {
        if (queue_lock == nullptr ||
            xSemaphoreTake(queue_lock, portMAX_DELAY) != pdTRUE)
        {
            return 0;
        }
        const size_t n = queue.size();
        xSemaphoreGive(queue_lock);
        return n;
    }

    void set_callback(cb_t cb) override
    {
        if (cb_lock == nullptr ||
            xSemaphoreTake(cb_lock, portMAX_DELAY) != pdTRUE)
        {
            return;
        }
        this->cb = std::move(cb);
        xSemaphoreGive(cb_lock);
    }

    bool has_data() override
    {
        if (queue_lock == nullptr ||
            xSemaphoreTake(queue_lock, portMAX_DELAY) != pdTRUE)
        {
            return false;
        }
        const bool ok = !queue.empty();
        xSemaphoreGive(queue_lock);
        return ok;
    }

    void notify_callback() override
    {
        if (cb_lock == nullptr ||
            xSemaphoreTake(cb_lock, portMAX_DELAY) != pdTRUE)
        {
            return;
        }
        cb_t cb = this->cb;
        xSemaphoreGive(cb_lock);
        if (cb)
        {
            cb();
        }
    }

private:
    SemaphoreHandle_t queue_lock = nullptr;
    SemaphoreHandle_t cb_lock = nullptr;
    std::vector<T> queue;
    cb_t cb = nullptr;
};

// Blocking (or polled) queue. The waiter uses this task's notification
// value, so the waiting task must not also be a task_reactor.
template <typename T>
class task_queue
{
public:
    explicit task_queue(bool blocking = true) : blocking(blocking)
    {
        lock = xSemaphoreCreateMutex();
    }

    ~task_queue()
    {
        if (lock != nullptr)
        {
            vSemaphoreDelete(lock);
        }
    }

    task_queue(const task_queue&) = delete;
    task_queue& operator=(const task_queue&) = delete;

    template <typename U>
    size_t push(U&& u)
    {
        if (lock == nullptr || xSemaphoreTake(lock, portMAX_DELAY) != pdTRUE)
        {
            return 0;
        }
        queue.emplace_back(std::forward<U>(u));
        const size_t n = queue.size();
        const TaskHandle_t waiter = this->waiter;
        xSemaphoreGive(lock);
        if (blocking && waiter != nullptr)
        {
            xTaskNotifyGive(waiter);
        }
        return n;
    }

    std::vector<T> pop()
    {
        std::vector<T> out;
        if (lock == nullptr || xSemaphoreTake(lock, portMAX_DELAY) != pdTRUE)
        {
            return out;
        }
        while (blocking && queue.empty())
        {
            waiter = xTaskGetCurrentTaskHandle();
            xSemaphoreGive(lock);
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            if (xSemaphoreTake(lock, portMAX_DELAY) != pdTRUE)
            {
                return out;
            }
        }
        waiter = nullptr;
        out = std::move(queue);
        xSemaphoreGive(lock);
        return out;
    }

    size_t size()
    {
        if (lock == nullptr || xSemaphoreTake(lock, portMAX_DELAY) != pdTRUE)
        {
            return 0;
        }
        const size_t n = queue.size();
        xSemaphoreGive(lock);
        return n;
    }

    void wake_up()
    {
        if (!blocking)
        {
            return;
        }
        TaskHandle_t waiter = nullptr;
        if (lock != nullptr && xSemaphoreTake(lock, portMAX_DELAY) == pdTRUE)
        {
            waiter = this->waiter;
            xSemaphoreGive(lock);
        }
        if (waiter != nullptr)
        {
            xTaskNotifyGive(waiter);
        }
    }

private:
    bool blocking = true;
    SemaphoreHandle_t lock = nullptr;
    TaskHandle_t waiter = nullptr;
    std::vector<T> queue;
};

}  // namespace bfc

#endif  // BFC_TASK_QUEUE_HPP_
