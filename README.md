# OpenProStreet Client

> The game plugin for [OpenProStreet](https://code.weexnes.dev/OpenProStreet) servers, for Need for Speed: ProStreet. Formerly ProStreetRPC.

[![Download Artifacts](https://img.shields.io/badge/Download-Artifacts-blue?style=for-the-badge&logo=package)](https://weexnes.dev/project/OpenProStreetClient#artifacts)
[![Repository](https://img.shields.io/badge/Git-Repository-orange?style=for-the-badge&logo=git)](https://code.weexnes.dev/OpenProStreetClient)

---

## 📦 Downloads & Releases

Pre-compiled release bundles (`OpenProStreetClient_v*.zip`) containing the ASI plugin and `discord_game_sdk.dll` can be downloaded directly from:
👉 **[https://weexnes.dev/project/OpenProStreetClient#artifacts](https://weexnes.dev/project/OpenProStreetClient#artifacts)**

---

## 🚀 Features

- **Secure Login**: Encrypts all traffic to an OpenProStreet server and logs you in with a key stored on your PC, instead of the game's plaintext password. Older servers still work, without encryption.
- **Race Results**: Sends your lap times, track, race mode and position to the server after each race, for records and leaderboards.
- **Quick Match**: "Schnelles Spiel" no longer tries EA's closed servers. It opens the server login, then joins the fullest open room. If there isn't one, it creates a race day with a random location and events, with AI opponents.
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

1. Download the latest release from [weexnes.dev Artifacts](https://weexnes.dev/project/OpenProStreetClient#artifacts).
2. Extract the archive contents:
   - Place `OpenProStreetClient.asi` into your NFS ProStreet `scripts/` directory (`<prostreet-root>/scripts/OpenProStreetClient.asi`).
   - Place `discord_game_sdk.dll` into your main NFS ProStreet root directory (where `nfs.exe` is located).
3. **If you're upgrading from ProStreetRPC, delete `scripts/ProStreetRPC.asi`.** Running both would hook the game twice.
4. Ensure Discord is running and launch NFS ProStreet!

### Your identity

The first time the game starts, the plugin creates `%LOCALAPPDATA%\OpenProStreet\identity.key`. This file is what logs you in to your account, so back it up, and never share it. To play from another PC, copy it there.

### Choosing a server

The plugin goes straight to `weexnes.dev`, without the address screen. To use another server, create `scripts/OpenProStreetClient.ini`:

```ini
[Server]
Address=play.example.com
Port=10104
```

`Address` is a domain or IP address. `Port` is optional. With `Address=` left empty, the game searches your LAN for a server, then asks for an address if it finds none.

### Borderless fullscreen

To run the game in a borderless window that fills your primary monitor, add this to `scripts/OpenProStreetClient.ini`:

```ini
[Display]
Borderless=1
```

The game keeps running when you tab out. Set its resolution to your monitor's for the sharpest picture, because other resolutions are stretched to fit.

### Console log

If another plugin opens a console, such as XNFSPS MultiFix with `EnablePrints = 1`, this plugin shows short, coloured lines there. For the detailed log, or to turn colour off, create `scripts/OpenProStreetClient.ini`:

```ini
[Log]
Verbose=1
Colour=0
```

Colour is also off when `NO_COLOR` is set, and the detailed log has no colour.

---

## 💻 Building from Source

The Monocypher encryption library is included in `vendor/`.

### Option A: Linux Cross-Compilation (Recommended)
```bash
# Install dependencies (MinGW-w64 toolchain, CMake, Make, curl, unzip)
sudo pacman -S mingw-w64-gcc cmake make curl unzip

# Run automated cross-build script
bash build.sh
```
Output binaries and release zip will be placed in `build/`.

To test the encryption code against a server without the game, build `tests/` and run `secure_proxy`, then point OpenProStreet's `tests/race_test.js` at it with `PROXY_PORT`.

### Option B: Windows Native Build (Visual Studio & vcpkg)
```cmd
vcpkg install discord-game-sdk:x86-windows
cmake -B build -A Win32 -DCMAKE_TOOLCHAIN_FILE=[path-to-vcpkg]/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

---

## 📜 License & Repository

- **Repository**: [https://code.weexnes.dev/OpenProStreetClient](https://code.weexnes.dev/OpenProStreetClient)
- **Part of the Need for Speed: ProStreet Modding Ecosystem**
