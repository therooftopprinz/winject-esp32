set(winject_srcs
    "winject-esp32/console.cpp"
    "winject-esp32/console_cmd.cpp"
    "winject-esp32/ether_bench.cpp"
    "winject-esp32/wifi_bench.cpp"
    "winject-esp32/netmgr/dhcp_client.cpp"
    "winject-esp32/netmgr/ethernet_rmii.cpp"
    "winject-esp32/netmgr/eth_dma_burst.cpp"
    "winject-esp32/netmgr/manager.cpp"
    "winject-esp32/ota.cpp"
    "winject-esp32/endpoint/channel_info_endpoint.cpp"
    "winject-esp32/endpoint/lc_rx_endpoint.cpp"
    "winject-esp32/endpoint/lc_tx_endpoint.cpp"
    "winject-esp32/endpoint/udp_logger.cpp"
    "winject-esp32/radio/frame.cpp"
    "winject-esp32/radio/packet.cpp"
    "winject-esp32/radio/wifi.cpp"
    "winject-esp32/radio/wifi_rx.cpp"
    "winject-esp32/radio/wifi_tx.cpp"
    "winject-esp32/settings.cpp"
)

idf_component_register(
    SRCS ${winject_srcs} "winject-esp32/main.cpp"
    INCLUDE_DIRS
        "."
        "winject-esp32"
        "winject-esp32/netmgr"
        "winject-esp32/endpoint"
        "winject-esp32/radio"
    REQUIRES
        app_update
        esp_driver_gpio
        esp_driver_uart
        esp_eth
        esp_event
        esp_http_server
        esp_netif
        esp_timer
        esp_wifi
        lwip
        nvs_flash
        spi_flash
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
