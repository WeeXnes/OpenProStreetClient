cmake_minimum_required(VERSION 3.10)

set(CMAKE_TOOLCHAIN_FILE "C:/Users/WeeXnes/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")

project(ProStreetRPC)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED True)

add_library(ProStreetRPC SHARED library.cpp)

set(VCPKG_INSTALLED_DIR "C:/Users/WeeXnes/vcpkg/installed/x86-windows")

target_include_directories(ProStreetRPC PRIVATE
    "${VCPKG_INSTALLED_DIR}/include/discord-game-sdk"
)

target_link_libraries(ProStreetRPC PRIVATE
    "${VCPKG_INSTALLED_DIR}/lib/discord_game_sdk.lib"
    "${VCPKG_INSTALLED_DIR}/lib/discord_game_sdk_cpp.lib"
)

set_target_properties(ProStreetRPC PROPERTIES OUTPUT_NAME "ProStreetRPC")
set_target_properties(ProStreetRPC PROPERTIES SUFFIX ".asi")
