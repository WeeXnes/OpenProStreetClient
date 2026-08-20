# ProStreetRPC

> Discord Rich Presence ASI plugin for Need for Speed: ProStreet.

[![Download Artifacts](https://img.shields.io/badge/Download-Artifacts-blue?style=for-the-badge&logo=package)](https://weexnes.dev/project/ProStreetRPC#artifacts)
[![Repository](https://img.shields.io/badge/Git-Repository-orange?style=for-the-badge&logo=git)](https://code.weexnes.dev/ProStreetRPC)

---

## 📦 Downloads & Releases

Pre-compiled release bundles (`ProStreetRPC_v*.zip`) containing the ASI plugin and `discord_game_sdk.dll` can be downloaded directly from:
👉 **[https://weexnes.dev/project/ProStreetRPC#artifacts](https://weexnes.dev/project/ProStreetRPC#artifacts)**

---

## 🚀 Features

- **Dynamic Status**: Displays whether you are in a race day, navigating menus, or in career.
- **Player Car Display**: Detects the car currently driven by the player and translates internal models to real vehicle names (e.g. "Nissan GT-R (R35)", "Mazda RX-7", "Chevrolet Corvette Z06", "Toyota Supra").
- **Race Mode & Event Type**: Identifies specific race types (e.g. "Grip", "Drift", "1/4 Mile Drag", "Wheelie Competition", "Speed Challenge", "Top Speed Run", "Sector Shootout", "Time Attack", "Grip Class").
- **Detailed Stats**: Shows your current career balance and Raceday XP (formatted with commas).
- **Map Identification**: Detects the current track/map and translates internal identifiers to human-readable names (e.g. "Nevada Highway", "Chicago Airfield").
- **Dynamic Cycle**: Automatically cycles Discord details line between track name, race type, current car, and Raceday XP during gameplay.
- **Safe Memory Management**: Validates pointers and handles memory reading safely to prevent game crashes.

---

## 🕹️ Game & Executable Requirements

This plugin was developed for the **v1.1 (Europe) "Battery 1.1 nocdfix"** executable layout.

- **Compatibility**: Battery No-CD fix is recommended for exact memory address offset resolution.
- **ASI Loader**: Requires an ASI loader (such as the one included in **Multifix 1.1**) to auto-load plugins from the `scripts` folder.
- **Disclaimer**: This project does not promote software piracy; ownership of a legitimate copy of the game is expected.

---

## 🛠️ Installation

1. Download the latest release from [weexnes.dev Artifacts](https://weexnes.dev/project/ProStreetRPC#artifacts).
2. Extract the archive contents:
   - Place `ProStreetRPC.asi` into your NFS ProStreet `scripts/` directory (`<prostreet-root>/scripts/ProStreetRPC.asi`).
   - Place `discord_game_sdk.dll` into your main NFS ProStreet root directory (where `nfs.exe` is located).
3. Ensure Discord is running and launch NFS ProStreet!

---

## 💻 Building from Source

### Option A: Linux Cross-Compilation (Recommended)
```bash
# Install dependencies (MinGW-w64 toolchain, CMake, Make, curl, unzip)
sudo pacman -S mingw-w64-gcc cmake make curl unzip

# Run automated cross-build script
bash build.sh
```
Output binaries and release zip will be placed in `build/`.

### Option B: Windows Native Build (Visual Studio & vcpkg)
```cmd
vcpkg install discord-game-sdk:x86-windows
cmake -B build -A Win32 -DCMAKE_TOOLCHAIN_FILE=[path-to-vcpkg]/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

---

## 📜 License & Repository

- **Repository**: [https://code.weexnes.dev/ProStreetRPC](https://code.weexnes.dev/ProStreetRPC)
- **Part of the Need for Speed: ProStreet Modding Ecosystem**
