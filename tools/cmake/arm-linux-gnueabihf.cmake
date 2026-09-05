# Cross-compile for Allwinner H3 / Orange Pi PC (32-bit armhf).
# Usage (repo root):
#   cmake -S src/manager -B build_manager_h3 \
#     -DCMAKE_TOOLCHAIN_FILE=tools/cmake/arm-linux-gnueabihf.cmake \
#     -DCMAKE_BUILD_TYPE=Release
#   cmake --build build_manager_h3 -j"$(nproc)"

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)
set(CMAKE_ASM_COMPILER arm-linux-gnueabihf-gcc)

set(CMAKE_FIND_ROOT_PATH /usr/arm-linux-gnueabihf)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
