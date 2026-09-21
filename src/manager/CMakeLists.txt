cmake_minimum_required(VERSION 3.16)
project(winject-manager CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(${CMAKE_CURRENT_SOURCE_DIR}/../bfc_fetch.cmake)

include(${CMAKE_CURRENT_SOURCE_DIR}/isal_ec.cmake)
winject_add_isal_ec()

add_executable(winject-manager
    main.cpp
    app.cpp
    config.cpp
    log.cpp
    console_client.cpp
    console_service.cpp
    frames/basic_fec.cpp
    frames/air_seq.cpp
    net_util.cpp
    radio/channel_info.cpp
    radio/mpdu.cpp
    radio/wifi_udp.cpp
    tx_scheduler.cpp
    endpoint/tcp_endpoint.cpp
    endpoint/tcp_client_endpoint.cpp
    endpoint/tcp_server_endpoint.cpp
    endpoint/udp_endpoint.cpp
    stream/tcp_stream.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../winject-esp32/radio/frame.cpp
)

target_include_directories(winject-manager PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../winject-esp32
    ${CMAKE_CURRENT_SOURCE_DIR}/../winject-esp32/radio
    ${CMAKE_CURRENT_SOURCE_DIR}/../test
    ${bfc_SOURCE_DIR}/src
)

target_compile_definitions(winject-manager PRIVATE WINJECT_HOST_TEST)

target_compile_options(winject-manager PRIVATE
    -Wall
    -Wextra
    -Werror=return-type
    -Wno-missing-field-initializers
)

target_link_libraries(winject-manager PRIVATE pthread isal_ec)
