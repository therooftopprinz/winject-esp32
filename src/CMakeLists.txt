set(common_srcs "common/ethernet.cpp")

if("${CMAKE_BINARY_DIR}" MATCHES "/test$")
    set(app_srcs "test/main.cpp")
else()
    set(app_srcs
        "winject/main.cpp"
        "winject/console.cpp"
        "winject/frame.cpp"
        "winject/ota.cpp"
        "winject/upstream.cpp"
        "winject/wifi_radio.cpp"
    )
endif()

idf_component_register(
    SRCS ${common_srcs} ${app_srcs}
    INCLUDE_DIRS "." "../include"
    REQUIRES
        app_update
        driver
        esp_eth
        esp_event
        esp_http_server
        esp_netif
        esp_timer
        esp_wifi
        lwip
        nvs_flash
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
