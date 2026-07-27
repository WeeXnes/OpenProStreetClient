#!/bin/bash
set -e

# Define directories
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
THIRDPARTY_DIR="${SCRIPT_DIR}/thirdparty"
SDK_DIR="${THIRDPARTY_DIR}/discord_game_sdk"
SDK_ZIP_URL="https://dl-game-sdk.discordapp.net/2.5.6/discord_game_sdk.zip"
SDK_ZIP="${THIRDPARTY_DIR}/discord_game_sdk.zip"

echo "=== Need for Speed: ProStreet RPC - Linux Cross-Build Script ==="

# 1. Download and extract Discord Game SDK if not already done
if [ ! -d "${SDK_DIR}" ]; then
    echo "Discord Game SDK not found. Preparing download..."
    mkdir -p "${THIRDPARTY_DIR}"
    
    if [ ! -f "${SDK_ZIP}" ]; then
        echo "Downloading Discord Game SDK v2.5.6..."
        curl -L -o "${SDK_ZIP}" "${SDK_ZIP_URL}"
    fi
    
    echo "Extracting Discord Game SDK..."
    unzip -q "${SDK_ZIP}" -d "${SDK_DIR}"
    rm "${SDK_ZIP}"
    echo "Discord Game SDK downloaded and extracted successfully."
else
    echo "Discord Game SDK already exists in thirdparty/."
fi

# 2. Configure and build
echo "Configuring build using CMake..."
cmake -B build \
  -DCMAKE_TOOLCHAIN_FILE="${SCRIPT_DIR}/mingw-w64-i686.cmake" \
  -DCMAKE_BUILD_TYPE=Release

echo "Building project..."
cmake --build build --config Release

# 3. Post-build tasks
# Copy discord_game_sdk.dll to build folder for convenience
if [ -f "${SDK_DIR}/lib/x86/discord_game_sdk.dll" ]; then
    echo "Copying discord_game_sdk.dll to build directory..."
    cp "${SDK_DIR}/lib/x86/discord_game_sdk.dll" build/
fi

echo "Creating ZIP bundle..."
python3 -c "
import zipfile, os
version = '1.5'
if os.path.exists('library.cpp'):
    with open('library.cpp') as f:
        for line in f:
            if 'const std::string version =' in line:
                version = line.split('\"')[1]
                break
zip_name = f'build/ProStreetRPC_v{version}.zip'
with zipfile.ZipFile(zip_name, 'w', zipfile.ZIP_DEFLATED) as zipf:
    zipf.write('build/ProStreetRPC.asi', 'ProStreetRPC.asi')
    zipf.write('build/discord_game_sdk.dll', 'discord_game_sdk.dll')
print(f'Successfully created release bundle: {zip_name}')
"

echo "=== Build Completed Successfully! ==="
echo "Output files are in the 'build/' directory:"
echo "  - build/ProStreetRPC.asi"
echo "  - build/discord_game_sdk.dll"
echo "  - build/ProStreetRPC_v*.zip (Release Bundle)"
