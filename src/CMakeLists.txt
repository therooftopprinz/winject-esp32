set(winject_srcs
    "winject/console.cpp"
    "winject/netmgr/dhcp_client.cpp"
    "winject/netmgr/dhcp_server.cpp"
    "winject/netmgr/ethernet_rmii.cpp"
    "winject/netmgr/manager.cpp"
    "winject/ota.cpp"
    "winject/radio/frame.cpp"
    "winject/radio/wifi.cpp"
    "winject/radio/wifi_rx.cpp"
    "winject/radio/wifi_tx.cpp"
    "winject/settings.cpp"
    "winject/upstream_rx.cpp"
    "winject/upstream_tx.cpp"
)

if("${CMAKE_BINARY_DIR}" MATCHES "/test$")
    set(app_srcs "test/main.cpp")
else()
    set(app_srcs "winject/main.cpp")
endif()

idf_component_register(
    SRCS ${winject_srcs} ${app_srcs}
    INCLUDE_DIRS
        "."
        "winject"
        "winject/netmgr"
        "winject/radio"
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
