# ProStreetRPC

An ASI plugin that adds Discord Rich Presence integration to Need for Speed: ProStreet.

## Features

- **Dynamic Status**: Shows whether you are in a race day, navigating menus, or in main menus.
- **Detailed Stats**: Displays your current career money and Raceday XP (automatically formatted with commas).
- **Map Identification**: Detects the current map and translates raw internal map identifiers to human-readable names (e.g., "Nevada Highway", "Chicago Airfield").
- **Dynamic Details**: Automatically cycles the Discord details line between the active track name and Raceday XP when in a race.
- **Safe Memory Reading**: Validates pointers and handles memory reading safely to prevent game crashes.

## Prerequisites

- **CMake** (version 3.10 or higher)
- **Visual Studio** (with C++ Desktop Development workload)
- **vcpkg** with the `discord-game-sdk` package installed for target `x86-windows`:
  ```bash
  vcpkg install discord-game-sdk:x86-windows
  ```

## Building the Project

### Windows (Native Build)

Configure the project for a 32-bit (x86) Windows target:
```bash
cmake -B build -A Win32 -DCMAKE_TOOLCHAIN_FILE=[path-to-vcpkg]/scripts/buildsystems/vcpkg.cmake
```

Build the project in Release configuration:
```bash
cmake --build build --config Release
```

The output files (`ProStreetRPC.asi` and `discord_game_sdk.dll`) will be created in the `build/Release/` directory.

### Linux (Cross-Compilation Build)

To compile for Windows using a Linux host:

1. Install the `mingw-w64` cross-compiler toolchain.
2. Install the `discord-game-sdk` dependency using vcpkg for MinGW:
   ```bash
   vcpkg install discord-game-sdk:x86-mingw-dynamic
   ```
3. Configure and build using a MinGW toolchain:
   ```bash
   cmake -B build \
     -DCMAKE_TOOLCHAIN_FILE=[path-to-vcpkg]/scripts/buildsystems/vcpkg.cmake \
     -DVCPKG_TARGET_TRIPLET=x86-mingw-dynamic
   cmake --build build --config Release
   ```

## Game & Executable Requirements

This plugin was developed on and for the **v1.1 (Europe) "Battery 1.1 nocdfix"** executable layout. 

- **Compatibility**: We cannot confirm if the plugin works on non-Battery v1.1 executables or other versions. Therefore, the Battery No-CD fix is considered required for the plugin's memory offsets to resolve correctly.
- **Disclaimer**: This project does not promote or condone software piracy. The dependency on this specific executable is purely due to the memory addressing layout used during development. Users are expected to own a legitimate copy of the game.
- **ASI Loader**: A loader (such as the one included in **Multifix 1.1**) is required to run the plugin.

## Installation

1. Ensure the `scripts` folder exists in your Need for Speed: ProStreet root directory (where `nfs.exe` is located): `<prostreet-root>/scripts/`.
2. Place `ProStreetRPC.asi` into the `scripts` folder (i.e. `<prostreet-root>/scripts/ProStreetRPC.asi`).
3. Place `discord_game_sdk.dll` into your Need for Speed: ProStreet root directory (where `nfs.exe` is located).
4. No external ASI loader is needed (Multifix 1.1 handles loading ASI plugins from the `scripts` directory automatically).
5. Start Discord, launch the game, and enjoy!

