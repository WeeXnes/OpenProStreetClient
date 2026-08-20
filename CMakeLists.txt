cmake_minimum_required(VERSION 3.10)

set(CMAKE_TOOLCHAIN_FILE "C:/Users/WeeXnes/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")

project(ProStreetRPC)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Check if we have a local thirdparty Discord Game SDK
set(THIRDPARTY_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/discord_game_sdk")
if(EXISTS "${THIRDPARTY_SDK_DIR}")
    message(STATUS "Using local Discord Game SDK from thirdparty/")
    
    # Gather all C++ wrapper files
    file(GLOB DISCORD_SDK_SOURCES "${THIRDPARTY_SDK_DIR}/cpp/*.cpp")
    
    # Compile the Discord Game SDK C++ wrapper files directly from source
    add_library(ProStreetRPC SHARED 
        library.cpp utils.cpp map.cpp car.cpp race_mode.cpp race_results.cpp lanfix.cpp discord_rpc.cpp
        ${DISCORD_SDK_SOURCES}
    )
    
    target_include_directories(ProStreetRPC PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${THIRDPARTY_SDK_DIR}/cpp"
    )

    # Force include our helper header for GCC/MinGW to fix SDK compilation
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_compile_options(ProStreetRPC PRIVATE
            "-include" "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty_imports.h"
        )
    endif()

    if(MINGW)
        target_link_options(ProStreetRPC PRIVATE
            -static-libgcc
            -static-libstdc++
            -static
        )
    endif()
    
    # For MinGW, we can link directly to the 32-bit dll.lib (import library)
    target_link_libraries(ProStreetRPC PRIVATE
        "${THIRDPARTY_SDK_DIR}/lib/x86/discord_game_sdk.dll.lib"
    )
else()
    # Traditional vcpkg setup for Windows builds
    add_library(ProStreetRPC SHARED library.cpp utils.cpp map.cpp car.cpp race_mode.cpp race_results.cpp lanfix.cpp discord_rpc.cpp)
    
    set(VCPKG_INSTALLED_DIR "C:/Users/WeeXnes/vcpkg/installed/x86-windows")
    
    target_include_directories(ProStreetRPC PRIVATE
        "${VCPKG_INSTALLED_DIR}/include/discord-game-sdk"
    )
    
    target_link_libraries(ProStreetRPC PRIVATE
        "${VCPKG_INSTALLED_DIR}/lib/discord_game_sdk.lib"
        "${VCPKG_INSTALLED_DIR}/lib/discord_game_sdk_cpp.lib"
    )
endif()

set_target_properties(ProStreetRPC PROPERTIES OUTPUT_NAME "ProStreetRPC")
set_target_properties(ProStreetRPC PROPERTIES PREFIX "")
set_target_properties(ProStreetRPC PROPERTIES SUFFIX ".asi")

