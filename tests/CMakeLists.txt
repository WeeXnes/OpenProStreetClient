# Builds the test relay on its own, without the game plugin or the Discord SDK.
cmake_minimum_required(VERSION 3.15)
project(OpenProStreetClientTests C CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(secure_proxy
    secure_proxy.cpp
    ../src/net/secure_client.cpp
    ../vendor/monocypher/monocypher.c
    ../vendor/monocypher/monocypher-ed25519.c
)
target_include_directories(secure_proxy PRIVATE ../src ../vendor/monocypher)
target_compile_definitions(secure_proxy PRIVATE _WIN32_WINNT=0x0601 NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(secure_proxy PRIVATE ws2_32 bcrypt)
