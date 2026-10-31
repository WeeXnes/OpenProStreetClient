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

## Current Car Identification (`car.cpp`)

The car is read from the same data the game uses, not by scanning memory for strings. All addresses are for the v1.1 executable (image base `0x400000`).

1. **Car handle**: `[0xAB0EA8]` is `FE::Career::FECareer`, and `+0x18` is the local player's car handle (`DALCareer` id `0x1772`). It is `0xFFFFFFFF` when no car is set. `GHub.UpdateCars` reads this handle and passes it to `GProStreetGame` (`[0xAB9DC4]`). Career, hub and LAN car selection all write it (`FELanCarSelectStateManager` through `0x7D6F90`).
2. **Car record**: `[0xAB0EAC]` is `UserProfile`, and `+0x240` is `FEPlayerCarDB`, a table of `0x19A` records of `0x18` bytes. Match the handle at record `+0x00`. Record `+0x08` is the vehicle key, the value `DALVehicleCommands::GetPVehicleKey` returns.
3. **Model**: the vehicle key is `Attrib::StringHash32` (Bob Jenkins lookup2, init `0xABCDEF00`) of the lowercase `pvehicle` collection name, such as `rx8` or `dlcbp_veyron`. `VEHICLE_KEY_TO_MODEL` maps it to the `CarTypeInfo` model name, and `CAR_TRANSLATION_TABLE` maps that to a display name from the game's `CARNAME_*` strings.

Collection names don't always match the model: `showdown_king` is a `LANCEREVOX`, and `gtrproto2` is a `SKYLINER35`. A key that isn't in the table is logged and no car is shown, and nothing is cached, so the status can't go stale.

`GRaceStatus` racer slots hold `GRacerInfo`, and the player is not always slot 0: in an 8-car race the player was slot 7 and slots 0-6 were AI. `[0xBFB400]` is `SimSystem`, not a player object.

---

## Memory Offsets & Pointer Safety

The plugin reads game state safely via memory addresses:
- **Game Session State**: Checks active game session pointers to distinguish between menu navigation and active racing.
- **Player Stats**: Reads player profile pointers for career cash and Raceday XP.
- **Sanitization**: All pointers undergo strict boundary checks (`if (ptr == nullptr) return;`) to ensure game stability across scene changes.
