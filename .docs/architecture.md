# Architecture & Memory Maps

This document details the memory structure and track mapping table used in `ProStreetRPC`.

---

## Game Executable Requirements

- **Executable Target**: `nfs.exe` v1.1 (Europe) "Battery 1.1 No-CD fix".
- **Architecture**: 32-bit (x86) PE executable.

---

## Track & Map Identification (`map.cpp`)

Internal map identifiers in `nfs.exe` are mapped to human-readable names:

| Internal Identifier | Display Name |
| :--- | :--- |
| `autobahn` | Autobahnring |
| `chicago` | Chicago Airfield |
| `ebisu` | Ebisu Circuit |
| `infineon` | Infineon Raceway |
| `nevada` | Nevada Highway |
| `portalnd` | Portland International Raceway |
| `mondello` | Mondello Park |
| `willow` | Willow Springs |
| `tokyo` | Tokyo Dockyard |

---

## Memory Offsets & Pointer Safety

The plugin reads game state safely via memory addresses:
- **Game Session State**: Checks active game session pointers to distinguish between menu navigation and active racing.
- **Player Stats**: Reads player profile pointers for career cash and Raceday XP.
- **Sanitization**: All pointers undergo strict boundary checks (`if (ptr == nullptr) return;`) to ensure game stability across scene changes.
