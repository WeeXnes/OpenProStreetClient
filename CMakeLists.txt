cmake_minimum_required(VERSION 3.10)

# vcpkg Toolchain muss vor project() gesetzt werden
set(CMAKE_TOOLCHAIN_FILE "C:/Users/WeeXnes/.vcpkg-clion/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")

project(ProStreetRPC)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Shared Library erstellen (.asi)
add_library(ProStreetRPC SHARED library.cpp)

# Pfad zu vcpkg x86-windows Installation
set(VCPKG_INSTALLED_DIR "C:/Users/WeeXnes/.vcpkg-clion/vcpkg/installed/x86-windows")

# Include-Pfad direkt auf discord-game-sdk setzen
target_include_directories(ProStreetRPC PRIVATE "${VCPKG_INSTALLED_DIR}/include/discord-game-sdk")

# Import-Libs verlinken
target_link_libraries(ProStreetRPC PRIVATE
        "${VCPKG_INSTALLED_DIR}/lib/discord_game_sdk.lib"
        "${VCPKG_INSTALLED_DIR}/lib/discord_game_sdk_cpp.lib"
)

# Ausgabedatei konfigurieren
set_target_properties(ProStreetRPC PROPERTIES OUTPUT_NAME "ProStreetRPC")
set_target_properties(ProStreetRPC PROPERTIES SUFFIX ".asi")
