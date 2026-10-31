# OpenProStreet Client Documentation

The OpenProStreet client is a 32-bit (x86) ASI plugin for Need for Speed: ProStreet, and the companion to the [OpenProStreet](https://code.weexnes.dev/OpenProStreet) server. It started as ProStreetRPC, a Discord Rich Presence plugin.

---

## Overview

It runs inside `nfs.exe`, without changing the game's files:

- **Secure login:** encrypts everything the game sends to an OpenProStreet server, and logs you in with a key instead of the game's plaintext password.
- **Race results:** reports lap times and race details to the server after each race, for records and leaderboards.
- **Quick match:** tells the server when you pick quick match, so it can place you in a room.
- **Windows 10 and 11 LAN fix:** lets the game's 2007 network code connect at all.
- **Discord Rich Presence:** shows your track, race mode, car and Raceday XP.
- **Custom PNG decals** and other tweaks.

---

## Startup

1. **ASI loader:** `DllMain` checks that the process is `nfs.exe`.
2. **Server link (`src/net/server_link.cpp`):** loads your identity and hooks the game's socket calls.
3. **LAN fix (`src/game/lanfix.cpp`):** patches socket creation, and tells the server link when the game connects.
4. **Hooks:** quick match menu commands, race results, decals and tweaks.
5. **Discord thread:** polls game memory and updates the activity.

---

## Memory Offset Safety

To prevent crashes when navigating main menus or loading screens:
- All dereferenced pointers are validated against null pointers before reading values.
- Floating point and integer statistics are sanitized prior to string formatting.
