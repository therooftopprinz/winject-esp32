cmake_minimum_required(VERSION 3.16)
project(winject-manager CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)
FetchContent_Declare(bfc
    GIT_REPOSITORY https://github.com/therooftopprinz/BFC.git
    GIT_TAG master
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(bfc)
if(NOT bfc_POPULATED)
    FetchContent_Populate(bfc)
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/isal_ec.cmake)
winject_add_isal_ec()

add_executable(winject-manager
    main.cpp
    app.cpp
    config.cpp
    console_client.cpp
    fec.cpp
    net_util.cpp
    radio_udp.cpp
    scheduler.cpp
    tcp_endpoint.cpp
    tcp_stream.cpp
    udp_endpoint.cpp
)

target_include_directories(winject-manager PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${bfc_SOURCE_DIR}/src
)

target_compile_options(winject-manager PRIVATE
    -Wall
    -Wextra
    -Werror=return-type
    -Wno-missing-field-initializers
)

target_link_libraries(winject-manager PRIVATE pthread isal_ec)
