#ifndef BFC_SEMAPHORE_HPP_
#define BFC_SEMAPHORE_HPP_

#ifdef WINJECT_HOST_TEST

#include <mutex>

namespace bfc
{

class semaphore
{
public:
    semaphore() = default;
    semaphore(const semaphore&) = delete;
    semaphore& operator=(const semaphore&) = delete;

    bool init()
    {
        ready_ = true;
        return true;
    }

    bool ready() const
    {
        return ready_;
    }

    bool take()
    {
        mutex_.lock();
        return true;
    }

    void give()
    {
        mutex_.unlock();
    }

    class lock
    {
    public:
        explicit lock(semaphore& sem) : sem_(&sem), owned_(sem.take())
        {
        }

        ~lock()
        {
            if (owned_ && sem_ != nullptr)
            {
                sem_->give();
            }
        }

        lock(lock&& other) noexcept : sem_(other.sem_), owned_(other.owned_)
        {
            other.sem_ = nullptr;
            other.owned_ = false;
        }

        lock(const lock&) = delete;
        lock& operator=(const lock&) = delete;
        lock& operator=(lock&&) = delete;

        explicit operator bool() const
        {
            return owned_;
        }

    private:
        semaphore* sem_;
        bool owned_;
    };

private:
    std::mutex mutex_;
    bool ready_ = false;
};

}  // namespace bfc

#else

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace bfc
{

// FreeRTOS mutex (priority inheritance) with unique_lock-style RAII.
// Not for ISR use. Binary/counting semaphores are not this type.
class semaphore
{
public:
    semaphore() = default;
    semaphore(const semaphore&) = delete;
    semaphore& operator=(const semaphore&) = delete;

    bool init()
    {
        if (handle_ != nullptr)
        {
            return true;
        }
        handle_ = xSemaphoreCreateMutex();
        return handle_ != nullptr;
    }

    bool ready() const
    {
        return handle_ != nullptr;
    }

    bool take(TickType_t ticks = portMAX_DELAY)
    {
        return handle_ != nullptr && xSemaphoreTake(handle_, ticks) == pdTRUE;
    }

    void give()
    {
        if (handle_ != nullptr)
        {
            xSemaphoreGive(handle_);
        }
    }

    class lock
    {
    public:
        explicit lock(semaphore& sem, TickType_t ticks = portMAX_DELAY)
            : sem_(&sem), owned_(sem.take(ticks))
        {
        }

        ~lock()
        {
            if (owned_ && sem_ != nullptr)
            {
                sem_->give();
            }
        }

        lock(lock&& other) noexcept : sem_(other.sem_), owned_(other.owned_)
        {
            other.sem_ = nullptr;
            other.owned_ = false;
        }

        lock(const lock&) = delete;
        lock& operator=(const lock&) = delete;
        lock& operator=(lock&&) = delete;

        explicit operator bool() const
        {
            return owned_;
        }

    private:
        semaphore* sem_;
        bool owned_;
    };

private:
    SemaphoreHandle_t handle_ = nullptr;
};

}  // namespace bfc

#endif  // WINJECT_HOST_TEST

#endif  // BFC_SEMAPHORE_HPP_
