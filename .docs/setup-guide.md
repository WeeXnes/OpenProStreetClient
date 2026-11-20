# Setup & Cross-Compilation Guide

Detailed instructions for building, packaging, and configuring the OpenProStreet client on Linux and Windows.

---

## 1. Linux Cross-Compilation Setup

The repository includes a automated shell script `build.sh` and CMake toolchain `mingw-w64-i686.cmake`.

### Toolchain Dependencies
Install MinGW-w64 and CMake:
```bash
# Arch Linux
sudo pacman -S mingw-w64-gcc cmake make curl unzip

# Ubuntu / Debian
sudo apt install gcc-mingw-w64-i686 g++-mingw-w64-i686 cmake make curl unzip
```

### Build Process
```bash
./build.sh
```

What `build.sh` does under the hood:
1. Downloads Discord Game SDK `v2.5.6` zip from official Discord CDN if not present in `thirdparty/`.
2. Extracts C++ headers and 32-bit `discord_game_sdk.dll` into `thirdparty/discord_game_sdk`.
3. Invokes CMake with `mingw-w64-i686.cmake`.
4. Compiles `OpenProStreetClient.asi`.
5. Packages `OpenProStreetClient.asi` and `discord_game_sdk.dll` into a versioned release ZIP under `build/`.

---

## 2. CI/CD Integration (OneDev)

The project includes `.onedev-buildspec.yml` configured for OneDev pipelines:
- Automatically parses version string from `src/main.cpp`.
- Checks for tag collisions on git origin.
- Executes cross-compilation inside a builder container.
- Publishes artifacts to `weexnes.dev/project/OpenProStreet/client#artifacts`.
