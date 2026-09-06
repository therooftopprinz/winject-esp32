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
        mutex.lock();
        return true;
    }

    void give()
    {
        mutex.unlock();
    }

    class lock
    {
    public:
        explicit lock(semaphore& sem) : sem(&sem), owned(sem.take())
        {
        }

        ~lock()
        {
            if (owned && sem != nullptr)
            {
                sem->give();
            }
        }

        lock(lock&& other) noexcept : sem(other.sem), owned(other.owned)
        {
            other.sem = nullptr;
            other.owned = false;
        }

        lock(const lock&) = delete;
        lock& operator=(const lock&) = delete;
        lock& operator=(lock&&) = delete;

        explicit operator bool() const
        {
            return owned;
        }

    private:
        semaphore* sem;
        bool owned;
    };

private:
    std::mutex mutex;
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
        if (handle != nullptr)
        {
            return true;
        }
        handle = xSemaphoreCreateMutex();
        return handle != nullptr;
    }

    bool ready() const
    {
        return handle != nullptr;
    }

    bool take(TickType_t ticks = portMAX_DELAY)
    {
        return handle != nullptr && xSemaphoreTake(handle, ticks) == pdTRUE;
    }

    void give()
    {
        if (handle != nullptr)
        {
            xSemaphoreGive(handle);
        }
    }

    class lock
    {
    public:
        explicit lock(semaphore& sem, TickType_t ticks = portMAX_DELAY)
            : sem(&sem), owned(sem.take(ticks))
        {
        }

        ~lock()
        {
            if (owned && sem != nullptr)
            {
                sem->give();
            }
        }

        lock(lock&& other) noexcept : sem(other.sem), owned(other.owned)
        {
            other.sem = nullptr;
            other.owned = false;
        }

        lock(const lock&) = delete;
        lock& operator=(const lock&) = delete;
        lock& operator=(lock&&) = delete;

        explicit operator bool() const
        {
            return owned;
        }

    private:
        semaphore* sem;
        bool owned;
    };

private:
    SemaphoreHandle_t handle = nullptr;
};

}  // namespace bfc

#endif  // WINJECT_HOST_TEST

#endif  // BFC_SEMAPHORE_HPP_
