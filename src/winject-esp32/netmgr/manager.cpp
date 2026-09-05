#include "manager.h"

#include "config.h"
#include "dhcp_client.h"
#include "dhcp_server.h"
#include "ethernet.h"

#include <strings.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char* TAG = "netmgr";

manager& manager::instance()
{
    static manager inst;
    return inst;
}

manager::manager()
    : eth_(ethernet::instance()),
      dhcp_client_(dhcp_client::instance()),
      dhcp_server_(dhcp_server::instance())
{
}

manager::~manager()
{
    if (started_.load(std::memory_order_acquire))
    {
        reactor_.stop();
    }
}

static bool staticIpValid(uint32_t ip)
{
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&ip);
    if (ip == 0 || b[0] == 0 || b[0] == 127 || b[0] >= 224)
    {
        return false;
    }
    return b[3] >= 1 && b[3] <= 254;
}

void manager::ensure_static_ip() const
{
    if (static_ip_.load(std::memory_order_relaxed) != 0)
    {
        return;
    }
    esp_ip4_addr_t addr = {};
    esp_netif_set_ip4_addr(&addr, ETH_FALLBACK_ADDR);
    uint32_t expected = 0;
    static_ip_.compare_exchange_strong(expected, addr.addr,
                                       std::memory_order_relaxed);
}

uint32_t manager::static_ip() const
{
    ensure_static_ip();
    return static_ip_.load(std::memory_order_relaxed);
}

bool manager::dhcp_server_should_run() const
{
    auto is_static = network_mode_.load(std::memory_order_relaxed) == NETMGR_MODE_STATIC;
    auto is_wanted = dhcp_server_wanted_.load(std::memory_order_relaxed);
    return is_static && is_wanted;
}

bool manager::apply_network_locked()
{
    if (!eth_.ready())
    {
        return true;
    }

    esp_netif_t* netif = eth_.netif();
    if (network_mode_.load(std::memory_order_relaxed) == NETMGR_MODE_AUTO)
    {
        auto_gen_.fetch_add(1, std::memory_order_relaxed);
        if (eth_.netif_is_dhcp_server())
        {
            if (!eth_.rebuild_dhcp_client())
            {
                return false;
            }
            netif = eth_.netif();
        }
        else if (!dhcp_server_.stop(netif) || !dhcp_client_.start(netif))
        {
            return false;
        }
        eth_.set_using_static(false);
        eth_.set_connected(eth_.has_ipv4());
        ESP_LOGI(TAG, "network AUTO (dhcp client; dhcps blocked)");
        schedule_static_fallback();
        return true;
    }

    cancel_static_fallback();
    auto_gen_.fetch_add(1, std::memory_order_relaxed);
    const uint32_t ip = static_ip();
    if (dhcp_server_should_run())
    {
        if (!eth_.netif_is_dhcp_server())
        {
            return eth_.rebuild_dhcp_server(ip);
        }
        return eth_.apply_static_ip(ip) && dhcp_server_.start(netif, ip);
    }

    if (eth_.netif_is_dhcp_server())
    {
        if (!eth_.rebuild_dhcp_client())
        {
            return false;
        }
        netif = eth_.netif();
    }
    else if (!dhcp_server_.stop(netif))
    {
        return false;
    }
    return eth_.apply_static_ip(ip);
}

bool manager::apply_network()
{
    if (!eth_.mutex().ready())
    {
        return true;
    }
    bfc::semaphore::lock lock(eth_.mutex());
    if (!lock)
    {
        return false;
    }
    return apply_network_locked();
}

void manager::cancel_static_fallback()
{
    if (!fallback_timer_set_)
    {
        return;
    }
    reactor_.get_timer().cancel(fallback_timer_id_);
    fallback_timer_set_ = false;
}

void manager::on_static_fallback(uint32_t gen)
{
    fallback_timer_set_ = false;
    if (auto_gen_.load(std::memory_order_relaxed) != gen ||
        network_mode_.load(std::memory_order_relaxed) != NETMGR_MODE_AUTO)
    {
        return;
    }

    bfc::semaphore::lock lock(eth_.mutex());
    if (!lock)
    {
        ESP_LOGE(TAG, "static fallback lock failed");
        return;
    }
    if (auto_gen_.load(std::memory_order_relaxed) != gen ||
        network_mode_.load(std::memory_order_relaxed) != NETMGR_MODE_AUTO)
    {
        return;
    }
    if (eth_.has_ipv4())
    {
        return;
    }
    ESP_LOGI(TAG, "no DHCP lease; static fallback");
    if (!eth_.apply_static_ip(static_ip()))
    {
        ESP_LOGE(TAG, "static fallback failed");
    }
}

void manager::schedule_static_fallback()
{
    cancel_static_fallback();
    const uint32_t gen = auto_gen_.load(std::memory_order_relaxed);
    fallback_timer_id_ =
        reactor_.get_timer().wait_ms(DHCP_FALLBACK_MS,
                                     [this, gen]()
                                     {
                                         on_static_fallback(gen);
                                     });
    fallback_timer_set_ = true;
}

void manager::bring_up()
{
    ensure_static_ip();
    eth_.begin();
    dhcp_server_.register_events();
    if (!apply_network())
    {
        ESP_LOGE(TAG, "network apply failed");
        init_ok_.store(false, std::memory_order_release);
        return;
    }
    init_ok_.store(true, std::memory_order_release);
}

void manager::run_reactor()
{
    bring_up();
    SemaphoreHandle_t done = init_done_;
    if (done != nullptr)
    {
        xSemaphoreGive(done);
    }
    reactor_.run();
}

void manager::reactor_task(void* arg)
{
    static_cast<manager*>(arg)->run_reactor();
}

bool manager::start()
{
    if (started_.load(std::memory_order_acquire))
    {
        return init_ok_.load(std::memory_order_acquire);
    }

    SemaphoreHandle_t init_done = xSemaphoreCreateBinary();
    if (init_done == nullptr)
    {
        ESP_LOGE(TAG, "init semaphore alloc failed");
        return false;
    }
    init_done_ = init_done;

    const BaseType_t ok =
        xTaskCreatePinnedToCore(reactor_task, "reactor", 6144, this,
                                NETMGR_TASK_PRIO, nullptr, APP_TASK_CORE);
    if (ok != pdPASS)
    {
        ESP_LOGE(TAG, "reactor task create failed");
        init_done_ = nullptr;
        vSemaphoreDelete(init_done);
        return false;
    }

    if (xSemaphoreTake(init_done, portMAX_DELAY) != pdTRUE)
    {
        ESP_LOGE(TAG, "reactor init wait failed");
        init_done_ = nullptr;
        vSemaphoreDelete(init_done);
        reactor_.stop();
        return false;
    }
    init_done_ = nullptr;
    vSemaphoreDelete(init_done);

    started_.store(true, std::memory_order_release);
    return init_ok_.load(std::memory_order_acquire);
}

manager::reactor_t& manager::reactor()
{
    return reactor_;
}

bool manager::connected() const
{
    return eth_.connected();
}

NetmgrMode manager::network_mode() const
{
    return network_mode_.load(std::memory_order_relaxed);
}

const char* manager::network_mode_name(NetmgrMode mode)
{
    if (mode == NETMGR_MODE_STATIC)
    {
        return "STATIC";
    }
    if (mode == NETMGR_MODE_AUTO)
    {
        return "AUTO";
    }
    return "UNKNOWN";
}

bool manager::parse_network_mode(const char* text, NetmgrMode* mode)
{
    if (text == nullptr || mode == nullptr)
    {
        return false;
    }
    if (strcasecmp(text, "STATIC") == 0)
    {
        *mode = NETMGR_MODE_STATIC;
        return true;
    }
    if (strcasecmp(text, "AUTO") == 0)
    {
        *mode = NETMGR_MODE_AUTO;
        return true;
    }
    return false;
}

bool manager::set_network_mode(NetmgrMode mode)
{
    if (mode != NETMGR_MODE_STATIC && mode != NETMGR_MODE_AUTO)
    {
        return false;
    }
    const NetmgrMode previous =
        network_mode_.exchange(mode, std::memory_order_relaxed);
    if (previous == mode && eth_.ready())
    {
        return true;
    }
    return apply_network();
}

bool manager::dhcp_server_enabled() const
{
    return dhcp_server_wanted_.load(std::memory_order_relaxed);
}

bool manager::dhcp_server_active() const
{
    return dhcp_server_.active();
}

bool manager::set_dhcp_server_enabled(bool enabled)
{
    dhcp_server_wanted_.store(enabled, std::memory_order_relaxed);
    if (network_mode_.load(std::memory_order_relaxed) == NETMGR_MODE_AUTO)
    {
        ESP_LOGI(TAG, "dhcps %s (blocked in AUTO)",
                 enabled ? "enabled" : "disabled");
        return true;
    }
    return apply_network();
}

bool manager::set_ip(uint32_t ip)
{
    if (!staticIpValid(ip))
    {
        return false;
    }
    static_ip_.store(ip, std::memory_order_relaxed);
    if (!eth_.ready())
    {
        return true;
    }
    if (network_mode_.load(std::memory_order_relaxed) == NETMGR_MODE_STATIC)
    {
        return apply_network();
    }
    if (eth_.using_static())
    {
        bfc::semaphore::lock lock(eth_.mutex());
        if (!lock)
        {
            return false;
        }
        return eth_.apply_static_ip(ip);
    }
    return true;
}

bool manager::static_ipv4(uint32_t* out) const
{
    if (out == nullptr)
    {
        return false;
    }
    *out = static_ip();
    return true;
}

bool manager::dhcp_pool(uint32_t* start, uint32_t* end) const
{
    return dhcp_server_.pool_range(static_ip(), start, end);
}

bool manager::local_ipv4(uint32_t* out) const
{
    return eth_.local_ipv4(out);
}
