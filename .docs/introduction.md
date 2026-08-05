# ProStreetRPC Documentation

Welcome to the **ProStreetRPC** technical documentation. This documentation covers the underlying mechanics, memory hooks, and Discord Game SDK integration inside Need for Speed: ProStreet.

---

## Overview

`ProStreetRPC` is a 32-bit (x86) ASI plugin written in C++ designed to interface NFS: ProStreet with the official **Discord Game SDK (v2.5.6)**. It injects into `nfs.exe` to read memory offsets for active race sessions, player career statistics, and map identifiers in real-time.

---

## Architectural Workflow

1. **ASI Loader Hook**: When loaded into `nfs.exe`, `DllMain` intercepts `DLL_PROCESS_ATTACH`, verifies the target process name is `nfs.exe`, and initializes Discord RPC.
2. **Discord Core Initialization**: Creates the `discord::Core` instance using application ID `656565390411038731`.
3. **LAN Fix Hook**: Calls `LANFix::Init()` to apply memory patches required for LAN broadcast binding.
4. **Game Memory Polling Loop**: Periodically reads game pointers for:
   - Current race state (In Menu, Race Day, Custom Race).
   - Track ID and mapping to human-readable string (`map.cpp`).
   - Player cash ($) and Raceday XP.
5. **Activity Dispatch**: Updates `discord::Activity` with formatted strings, small/large image assets, and timestamps.

---

## Memory Offset Safety

To prevent crashes when navigating main menus or loading screens:
- All dereferenced pointers are validated against null pointers before reading values.
- Floating point and integer statistics are sanitized prior to string formatting.
