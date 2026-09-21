cmake_minimum_required(VERSION 3.16)
project(sim-winject-esp32 CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(sim-winject-esp32
    main.cpp
    radio_sim.cpp
    modulation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../winject-esp32/radio/frame.cpp
)

target_include_directories(sim-winject-esp32 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../winject-esp32
    ${CMAKE_CURRENT_SOURCE_DIR}/../winject-esp32/radio
    ${CMAKE_CURRENT_SOURCE_DIR}/../test
    ${CMAKE_CURRENT_SOURCE_DIR}/../bfc-esp32
)

target_compile_definitions(sim-winject-esp32 PRIVATE WINJECT_HOST_TEST)

target_compile_options(sim-winject-esp32 PRIVATE
    -Wall
    -Wextra
    -Werror=return-type
    -Wno-missing-field-initializers
)

target_link_libraries(sim-winject-esp32 PRIVATE pthread)
