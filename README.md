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

The plugin must be built as a **32-bit (x86) Windows DLL** (named `.asi`). You can build it natively on Windows or cross-compile it on Linux.

### Option A: Linux (Cross-Compilation Build for Windows)
This is the recommended method if you are developing on Linux. The project includes an automated script to handle everything.

#### Prerequisites
Install the MinGW-w64 toolchain, CMake, and Make. For example, on Arch Linux:
```bash
sudo pacman -S mingw-w64-gcc cmake make curl unzip
```

#### Build Steps
1. Run the build script in the root directory:
   ```bash
   bash build.sh
   ```
2. The script will automatically:
   - Download and extract the official Discord Game SDK v2.5.6 into `thirdparty/`.
   - Configure CMake using the `mingw-w64-i686.cmake` toolchain.
   - Build the C++ SDK wrapper and target project.
   - Copy the 32-bit `discord_game_sdk.dll` alongside the compiled `ProStreetRPC.asi` in the `build/` directory.

---

### Option B: Windows (Native Build)
If you prefer to build natively on Windows, you will use Microsoft Visual Studio and `vcpkg`.

#### Prerequisites
1. **Visual Studio** (with the C++ Desktop Development workload installed).
2. **CMake** (version 3.10 or higher).
3. **vcpkg** package manager with the `discord-game-sdk` package installed for target `x86-windows`:
   ```cmd
   vcpkg install discord-game-sdk:x86-windows
   ```

#### Build Steps
1. Configure the project for a 32-bit (x86) target using the vcpkg toolchain:
   ```cmd
   cmake -B build -A Win32 -DCMAKE_TOOLCHAIN_FILE=[path-to-vcpkg]/scripts/buildsystems/vcpkg.cmake
   ```
2. Build the project in Release configuration:
   ```cmd
   cmake --build build --config Release
   ```
3. The output files (`ProStreetRPC.asi` and `discord_game_sdk.dll`) will be created in the `build/Release/` directory.

---

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

