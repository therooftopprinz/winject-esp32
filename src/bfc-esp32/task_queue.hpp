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
        queue_lock_ = xSemaphoreCreateMutex();
        cb_lock_ = xSemaphoreCreateMutex();
    }

    ~reactive_task_queue()
    {
        if (queue_lock_ != nullptr)
        {
            vSemaphoreDelete(queue_lock_);
        }
        if (cb_lock_ != nullptr)
        {
            vSemaphoreDelete(cb_lock_);
        }
    }

    reactive_task_queue(const reactive_task_queue&) = delete;
    reactive_task_queue& operator=(const reactive_task_queue&) = delete;

    template <typename U>
    size_t push(U&& u)
    {
        if (queue_lock_ == nullptr ||
            xSemaphoreTake(queue_lock_, portMAX_DELAY) != pdTRUE)
        {
            return 0;
        }
        queue_.emplace_back(std::forward<U>(u));
        const size_t n = queue_.size();
        xSemaphoreGive(queue_lock_);
        return n;
    }

    std::vector<T> pop()
    {
        std::vector<T> out;
        if (queue_lock_ == nullptr ||
            xSemaphoreTake(queue_lock_, portMAX_DELAY) != pdTRUE)
        {
            return out;
        }
        out = std::move(queue_);
        xSemaphoreGive(queue_lock_);
        return out;
    }

    size_t size()
    {
        if (queue_lock_ == nullptr ||
            xSemaphoreTake(queue_lock_, portMAX_DELAY) != pdTRUE)
        {
            return 0;
        }
        const size_t n = queue_.size();
        xSemaphoreGive(queue_lock_);
        return n;
    }

    void set_callback(cb_t cb) override
    {
        if (cb_lock_ == nullptr ||
            xSemaphoreTake(cb_lock_, portMAX_DELAY) != pdTRUE)
        {
            return;
        }
        cb_ = std::move(cb);
        xSemaphoreGive(cb_lock_);
    }

    bool has_data() override
    {
        if (queue_lock_ == nullptr ||
            xSemaphoreTake(queue_lock_, portMAX_DELAY) != pdTRUE)
        {
            return false;
        }
        const bool ok = !queue_.empty();
        xSemaphoreGive(queue_lock_);
        return ok;
    }

    void notify_callback() override
    {
        if (cb_lock_ == nullptr ||
            xSemaphoreTake(cb_lock_, portMAX_DELAY) != pdTRUE)
        {
            return;
        }
        cb_t cb = cb_;
        xSemaphoreGive(cb_lock_);
        if (cb)
        {
            cb();
        }
    }

private:
    SemaphoreHandle_t queue_lock_ = nullptr;
    SemaphoreHandle_t cb_lock_ = nullptr;
    std::vector<T> queue_;
    cb_t cb_ = nullptr;
};

// Blocking (or polled) queue. The waiter uses this task's notification
// value, so the waiting task must not also be a task_reactor.
template <typename T>
class task_queue
{
public:
    explicit task_queue(bool blocking = true) : blocking_(blocking)
    {
        lock_ = xSemaphoreCreateMutex();
    }

    ~task_queue()
    {
        if (lock_ != nullptr)
        {
            vSemaphoreDelete(lock_);
        }
    }

    task_queue(const task_queue&) = delete;
    task_queue& operator=(const task_queue&) = delete;

    template <typename U>
    size_t push(U&& u)
    {
        if (lock_ == nullptr || xSemaphoreTake(lock_, portMAX_DELAY) != pdTRUE)
        {
            return 0;
        }
        queue_.emplace_back(std::forward<U>(u));
        const size_t n = queue_.size();
        const TaskHandle_t waiter = waiter_;
        xSemaphoreGive(lock_);
        if (blocking_ && waiter != nullptr)
        {
            xTaskNotifyGive(waiter);
        }
        return n;
    }

    std::vector<T> pop()
    {
        std::vector<T> out;
        if (lock_ == nullptr || xSemaphoreTake(lock_, portMAX_DELAY) != pdTRUE)
        {
            return out;
        }
        while (blocking_ && queue_.empty())
        {
            waiter_ = xTaskGetCurrentTaskHandle();
            xSemaphoreGive(lock_);
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            if (xSemaphoreTake(lock_, portMAX_DELAY) != pdTRUE)
            {
                return out;
            }
        }
        waiter_ = nullptr;
        out = std::move(queue_);
        xSemaphoreGive(lock_);
        return out;
    }

    size_t size()
    {
        if (lock_ == nullptr || xSemaphoreTake(lock_, portMAX_DELAY) != pdTRUE)
        {
            return 0;
        }
        const size_t n = queue_.size();
        xSemaphoreGive(lock_);
        return n;
    }

    void wake_up()
    {
        if (!blocking_)
        {
            return;
        }
        TaskHandle_t waiter = nullptr;
        if (lock_ != nullptr && xSemaphoreTake(lock_, portMAX_DELAY) == pdTRUE)
        {
            waiter = waiter_;
            xSemaphoreGive(lock_);
        }
        if (waiter != nullptr)
        {
            xTaskNotifyGive(waiter);
        }
    }

private:
    bool blocking_ = true;
    SemaphoreHandle_t lock_ = nullptr;
    TaskHandle_t waiter_ = nullptr;
    std::vector<T> queue_;
};

}  // namespace bfc

#endif  // BFC_TASK_QUEUE_HPP_
