cmake_minimum_required(VERSION 3.16)
project(winject-tests CXX)

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

set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(googletest)

include(${CMAKE_CURRENT_SOURCE_DIR}/../manager/isal_ec.cmake)
winject_add_isal_ec()

add_library(winject_esp32_frame OBJECT
    ${CMAKE_CURRENT_SOURCE_DIR}/../winject-esp32/radio/frame.cpp
)
target_compile_definitions(winject_esp32_frame PRIVATE WINJECT_HOST_TEST)
target_include_directories(winject_esp32_frame PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../winject-esp32
    ${CMAKE_CURRENT_SOURCE_DIR}/../winject-esp32/radio
)

add_executable(winject-tests
    frame_test.cpp
    config_test.cpp
    basic_fec_test.cpp
    net_util_test.cpp
    tcp_stream_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../manager/config.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../manager/frames/basic_fec.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../manager/net_util.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../manager/stream/tcp_stream.cpp
    $<TARGET_OBJECTS:winject_esp32_frame>
)

target_compile_definitions(winject-tests PRIVATE WINJECT_HOST_TEST)
target_include_directories(winject-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../manager
    ${CMAKE_CURRENT_SOURCE_DIR}/../winject-esp32
    ${CMAKE_CURRENT_SOURCE_DIR}/../winject-esp32/radio
    ${bfc_SOURCE_DIR}/src
)
target_compile_options(winject-tests PRIVATE
    -Wall
    -Wextra
    -Werror=return-type
)
target_link_libraries(winject-tests PRIVATE GTest::gtest_main pthread isal_ec)

enable_testing()
include(GoogleTest)
gtest_discover_tests(winject-tests)
