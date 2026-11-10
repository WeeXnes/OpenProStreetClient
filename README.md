# <img src="assets/openprostreet.png" width="36" height="36" align="top" alt=""> OpenProStreet Client

> The game plugin for [OpenProStreet](https://code.weexnes.dev/OpenProStreet) servers, for Need for Speed: ProStreet. Formerly ProStreetRPC.

[![Download Artifacts](https://img.shields.io/badge/Download-Artifacts-blue?style=for-the-badge&logo=package)](https://weexnes.dev/project/OpenProStreetClient#artifacts)
[![Repository](https://img.shields.io/badge/Git-Repository-orange?style=for-the-badge&logo=git)](https://code.weexnes.dev/OpenProStreetClient)

---

## 📦 Downloads & Releases

Pre-compiled release bundles (`OpenProStreetClient_v*.zip`) containing the ASI plugin and `discord_game_sdk.dll` can be downloaded directly from:
👉 **[https://weexnes.dev/project/OpenProStreetClient#artifacts](https://weexnes.dev/project/OpenProStreetClient#artifacts)**

---

## 🚀 Features

- **Secure Login**: Encrypts all traffic to an OpenProStreet server, so the game's password isn't sent in plaintext. Your name and password work on any PC. Older servers still work, without encryption.
- **Race Results**: Sends your lap times, track, race mode and position to the server after each race, for records and leaderboards.
- **Quick Play**: "Schnelles Spiel" no longer tries EA's closed servers. It opens the server login and lists the same race days as LAN play, so you can join any of them. If no quick play race day is running, it starts one with a random location and events, with AI opponents.
- **Dynamic Status**: Displays whether you are in a race day, navigating menus, or in career.
- **Player Car Display**: Detects the car currently driven by the player and translates internal models to real vehicle names (e.g. "Nissan GT-R (R35)", "Mazda RX-7", "Chevrolet Corvette Z06", "Toyota Supra").
- **Race Mode & Event Type**: Identifies specific race types (e.g. "Grip", "Drift", "1/4 Mile Drag", "Wheelie Competition", "Speed Challenge", "Top Speed Run", "Sector Shootout", "Time Attack", "Grip Class").
- **Detailed Stats**: Shows your current career balance and Raceday XP (formatted with commas).
- **Map Identification**: Detects the current track/map and translates internal identifiers to human-readable names (e.g. "Nevada Highway", "Chicago Airfield").
- **Dynamic Cycle**: Automatically cycles Discord details line between track name, race type, current car, and Raceday XP during gameplay.
- **Car Fixes Built In**: No tank slapper and no speed limiter on any car, the same changes the NFS-VltEd scripts "Tank Unslapper" and "Speed Limiter Removed" make, without VltEd. See [Car fixes](#car-fixes).
- **Main Menu**: The "LAN Play" button reads "OpenProStreet" in every language, the copyright line shows the client version, and the game no longer tries to log in to EA (the "Connecting to EA server..." popup you had to cancel).
- **Account Screen**: "Create new LAN account" and "Use existing LAN account" read "Create OpenProStreet account" and "Log in to OpenProStreet account", the title above them reads "OpenProStreet", and the messages around creating an account say OpenProStreet too.
- **Safe Memory Management**: Validates pointers and handles memory reading safely to prevent game crashes.

---

## 🕹️ Game & Executable Requirements

This plugin was developed for the **v1.1 (Europe) "Battery 1.1 nocdfix"** executable layout.

- **Compatibility**: Battery No-CD fix is recommended for exact memory address offset resolution.
- **Packed executables**: Not supported, for example the "Vitality" 1.1 No-CD (3.7 MB, MD5 `15082baf8bb09b3298768c9b5c2b946e`). Its code is still compressed when plugins load, so neither this client nor XNFSPS MultiFix can patch it, and settings like the resolution won't stick. The client detects this, shows a message and stays off.
- **Game version**: Requires NFS ProStreet updated to version **1.1**.
- **XNFSPS MultiFix**: Required. It also loads plugins from the `scripts` folder.
- **Fusion Fix**: Not compatible. Remove it before installing.
- **Disclaimer**: This project does not promote software piracy; ownership of a legitimate copy of the game is expected.

---

## 🛠️ Installation

1. Download the latest release from [weexnes.dev Artifacts](https://weexnes.dev/project/OpenProStreetClient#artifacts).
2. Extract the archive contents:
   - Place `OpenProStreetClient.asi` and `discord_game_sdk.dll` into your NFS ProStreet `scripts/` directory (`<prostreet-root>/scripts/`).
3. **If you're upgrading from ProStreetRPC, delete `scripts/ProStreetRPC.asi`.** Running both would hook the game twice.
4. Ensure Discord is running and launch NFS ProStreet!

### Your account

Your name and password log you in, from any PC with the plugin. The plugin also keeps a key in `%LOCALAPPDATA%\OpenProStreet\identity.key` for the encrypted login. It's created on its own, and you don't need to copy it to another PC.

To log in on your own as soon as you open OpenProStreet, turn on "Log in automatically" in the launcher's settings and enter your name and password there. The launcher saves them under `[Account]` in `scripts/OpenProStreetClient.ini`:

```ini
[Account]
AutoLogin=1
Username=YourName
Password=dpapi:...
```

On Windows the password is encrypted for your Windows user, so other users of the PC can't read it from the file. On Linux it's saved as you typed it. If the automatic login fails, the account screen stays open and you log in by hand.

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

### Car fixes

The client changes these values while the game loads `GLOBAL\ATTRIBUTES.BIN`:

- **No tank slapper:** each car's `TANK_SLAPPER_TIMER` is set to 0, so cars don't start weaving at high speed.
- **No speed limiter:** both `SPEED_LIMITER` values on each car are set to 900, so a car's top speed comes from its engine and gearing.
- **No camera vibration:** `Enable` is set to false in every `cameravibration` preset (`default`, `bumperhood`, `close`, `far`, `staging`).

These are the same edits as the NFS-VltEd scripts "Tank Unslapper" and "Speed Limiter Removed" and turning off camera vibration in VltEd, so you don't need VltEd anymore. The file on disk isn't changed. If you already applied those scripts, nothing changes: values that are already set are left alone. Camera vibration only changes your own view. To turn any of them off:

```ini
[Physics]
NoTankSlapper=0
NoSpeedLimiter=0

[Display]
NoCameraVibration=0
```

You can race with or without the speed limiter on the same server. The client tells the server which one your cars have, and records are kept apart for each, so the leaderboards only compare like with like.

The client also sends the server a hash of this file with each race result. The hash is taken with all three fixes made, so it's the same whether you turned a fix off or VltEd already made it. A server can use it to keep results from edited car data off its leaderboard.

### Writing a server's event catalog

Server admins who use the control panel need a list of the game's events, and only the game knows them. Add this to `scripts/OpenProStreetClient.ini`, start the game and open LAN play or Quick Match once:

```ini
[Debug]
DumpEvents=1
```

The plugin writes `%LOCALAPPDATA%\OpenProStreet\event_catalog.json`. Copy it next to the server, then remove the setting.

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
