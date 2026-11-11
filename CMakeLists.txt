cmake_minimum_required(VERSION 3.10)

set(CMAKE_TOOLCHAIN_FILE "C:/Users/WeeXnes/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")

project(OpenProStreetClient)

# The client's version lives in src/main.cpp, where the release job reads it too. The file details
# Windows and the launcher read are made from it, so they can't fall behind.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp" CLIENT_VERSION_LINE REGEX "const std::string version = \"")
if(NOT CLIENT_VERSION_LINE MATCHES "\"([0-9]+)\\.([0-9]+)")
    message(FATAL_ERROR "Couldn't read the client version from src/main.cpp")
endif()
set(CLIENT_VERSION_MAJOR ${CMAKE_MATCH_1})
set(CLIENT_VERSION_MINOR ${CMAKE_MATCH_2})
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS src/main.cpp)
configure_file(src/version.rc.in "${CMAKE_CURRENT_BINARY_DIR}/version.rc" @ONLY)
message(STATUS "Client version ${CLIENT_VERSION_MAJOR}.${CLIENT_VERSION_MINOR}")

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED True)

set(CLIENT_SOURCES
    src/log.cpp
    src/main.cpp
    src/utils.cpp
    src/discord/discord_rpc.cpp
    src/game/auto_login.cpp
    src/game/borderless.cpp
    src/game/car.cpp
    src/game/car_lock.cpp
    src/game/display_test.cpp
    src/game/hooks.cpp
    src/game/input_focus.cpp
    src/game/lan_menu.cpp
    src/game/lanfix.cpp
    src/game/map.cpp
    src/game/quick_match.cpp
    src/game/race_mode.cpp
    src/game/race_results.cpp
    src/game/vault_fixes.cpp
    src/game/tweaks.cpp
    src/net/secure_client.cpp
    src/net/server_link.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/version.rc"
    vendor/monocypher/monocypher.c
    vendor/monocypher/monocypher-ed25519.c
    vendor/minhook/hook.c
    vendor/minhook/buffer.c
    vendor/minhook/trampoline.c
    vendor/minhook/hde/hde32.c
)

# Check if we have a local thirdparty Discord Game SDK
set(THIRDPARTY_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/discord_game_sdk")
if(EXISTS "${THIRDPARTY_SDK_DIR}")
    message(STATUS "Using local Discord Game SDK from thirdparty/")

    # Gather all C++ wrapper files
    file(GLOB DISCORD_SDK_SOURCES "${THIRDPARTY_SDK_DIR}/cpp/*.cpp")

    # Compile the Discord Game SDK C++ wrapper files directly from source
    add_library(OpenProStreetClient SHARED
        ${CLIENT_SOURCES}
        ${DISCORD_SDK_SOURCES}
    )

    target_include_directories(OpenProStreetClient PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor"
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor/monocypher"
        "${THIRDPARTY_SDK_DIR}/cpp"
    )

    # Force include our helper header for GCC/MinGW to fix SDK compilation
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_compile_options(OpenProStreetClient PRIVATE
            "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/src/thirdparty_imports.h>"
        )
    endif()

    if(MINGW)
        target_compile_definitions(OpenProStreetClient PRIVATE _WIN32_WINNT=0x0601)
        target_link_options(OpenProStreetClient PRIVATE
            -static-libgcc
            -static-libstdc++
            -static
        )
        # Most of the file would be debug info from the static libraries. The release file leaves
        # it out, and the copy with it stays next to it, to look into crashes and detections.
        add_custom_command(TARGET OpenProStreetClient POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:OpenProStreetClient> $<TARGET_FILE:OpenProStreetClient>.debug
            COMMAND ${CMAKE_STRIP} --strip-all $<TARGET_FILE:OpenProStreetClient>
        )
    endif()

    # For MinGW, we can link directly to the 32-bit dll.lib (import library)
    target_link_libraries(OpenProStreetClient PRIVATE
        "${THIRDPARTY_SDK_DIR}/lib/x86/discord_game_sdk.dll.lib"
        ws2_32
        crypt32
        bcrypt
    )
else()
    # Traditional vcpkg setup for Windows builds
    add_library(OpenProStreetClient SHARED ${CLIENT_SOURCES})

    set(VCPKG_INSTALLED_DIR "C:/Users/WeeXnes/vcpkg/installed/x86-windows")

    target_include_directories(OpenProStreetClient PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor"
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor/monocypher"
        "${VCPKG_INSTALLED_DIR}/include/discord-game-sdk"
    )

    target_link_libraries(OpenProStreetClient PRIVATE
        "${VCPKG_INSTALLED_DIR}/lib/discord_game_sdk.lib"
        "${VCPKG_INSTALLED_DIR}/lib/discord_game_sdk_cpp.lib"
        ws2_32
        crypt32
        bcrypt
    )
endif()

set_target_properties(OpenProStreetClient PROPERTIES OUTPUT_NAME "OpenProStreetClient")
set_target_properties(OpenProStreetClient PROPERTIES PREFIX "")
set_target_properties(OpenProStreetClient PROPERTIES SUFFIX ".asi")
