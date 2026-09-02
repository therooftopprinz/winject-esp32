#ifndef WINJECT_INDICATOR_LED_H_
#define WINJECT_INDICATOR_LED_H_

#include "config.h"

#include <atomic>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_timer.h"

class indicator_led
{
public:
    explicit indicator_led(int gpio) : gpio_(gpio) {}

    indicator_led(const indicator_led&) = delete;
    indicator_led& operator=(const indicator_led&) = delete;

    bool enabled() const
    {
        return gpio_ >= 0;
    }

    int gpio() const
    {
        return gpio_;
    }

    bool init()
    {
        if (ready_)
        {
            return true;
        }
        if (!enabled())
        {
            return false;
        }

        gpio_config_t io = {};
        io.pin_bit_mask = 1ULL << gpio_;
        io.mode = GPIO_MODE_OUTPUT;
        io.pull_up_en = GPIO_PULLUP_DISABLE;
        io.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io.intr_type = GPIO_INTR_DISABLE;
        if (gpio_config(&io) != ESP_OK)
        {
            return false;
        }

        gpio_set_level(static_cast<gpio_num_t>(gpio_), WIFI_LED_OFF);
        ready_ = true;
        if (s_count < k_max_leds)
        {
            s_leds[s_count++] = this;
        }
        return true;
    }

    void pulse()
    {
        until_.store(esp_timer_get_time() + WIFI_LED_STRETCH_US,
                     std::memory_order_relaxed);
    }

    static bool start_poll()
    {
        if (s_timer != nullptr)
        {
            return true;
        }
        if (s_count == 0)
        {
            return false;
        }

        esp_timer_create_args_t args = {};
        args.callback = poll;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "led_poll";
        if (esp_timer_create(&args, &s_timer) != ESP_OK ||
            esp_timer_start_periodic(s_timer, k_poll_period_us) != ESP_OK)
        {
            s_timer = nullptr;
            return false;
        }
        return true;
    }

private:
    static constexpr size_t k_max_leds = 2;
    static constexpr uint64_t k_poll_period_us = 10000;

    static void poll(void* arg)
    {
        (void)arg;
        const int64_t now = esp_timer_get_time();
        for (size_t i = 0; i < s_count; i++)
        {
            s_leds[i]->update(now);
        }
    }

    void update(int64_t now)
    {
        if (!ready_)
        {
            return;
        }
        gpio_set_level(static_cast<gpio_num_t>(gpio_),
                       now < until_.load(std::memory_order_relaxed)
                           ? WIFI_LED_ON
                           : WIFI_LED_OFF);
    }

    const int gpio_;
    bool ready_ = false;
    std::atomic<int64_t> until_{0};

    inline static indicator_led* s_leds[k_max_leds] = {};
    inline static size_t s_count = 0;
    inline static esp_timer_handle_t s_timer = nullptr;
};

#endif  // WINJECT_INDICATOR_LED_H_
