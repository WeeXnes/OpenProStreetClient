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

`[0xBFB400]` is `SimSystem`, not a player object.

---

## Race Results (`race_results.cpp`)

`[0xAB9D7C]` is `GRaceStatus`. Racers start at `+0x20`, each `0x594` bytes, with the count at `+0x2CE0`. The player is not always slot 0: in an 8-car race the player was slot 7 and slots 0-6 were AI.

**Finding the local racer**: `[0xACE444]` is the local player list (count at `[0xACE44C]`), the one `PlayerIsLocal` walks. An entry's simable is at `entry - 8`, and the simable's handle at `+8` matches `GRacerInfo +0x10`. If that finds nothing, a single racer with no AI opponent at `+0x14` is used. If neither works, no result is saved, so an AI's data is never recorded as the player's.

| `GRacerInfo` field | Meaning | Evidence |
| :--- | :--- | :--- |
| `+0x10` | Simable handle | `GetSimable` (`0x667290`) |
| `+0x14` | AI opponent object, null for humans | `0x677130`, `0x660920` |
| `+0x18` | Name, `char[32]` | `GetName` (`0x46E1E0`) |
| `+0x5C` | Finishing rank, starting at 1 | `CalculateRankings` (`0x683F90`) |
| `+0x6C` | Score | `0x683B50`, `0x656B20` |
| `+0x70` | Race time snapshot | `PrePassStatistics` (`0x68DF70`) |
| `+0x74` | Laps completed | `GetLapsCompleted` (`0x404C80`) |
| `+0x78` | Top speed in m/s | Max kept while racing (`0x6964D6`), mph to m/s conversion at `0x68DBFC` |
| `+0x80` | Best lap, `-1` when none | Lap handler (`0x695DE0`) |
| `+0x2D4` | Last lap, `-1` when none | Lap handler (`0x695DE0`) |
| `+0x3E8` | Race timer: start, elapsed at `+0x3EC`, running flag at `+0x3F0` | `GetRaceTime` (`0x404C90`) |

The struct has no per-lap time array, so lap times are captured by a hook on the lap handler at `0x695DE0`. A lap count of 0 there means a new race or a restart, which clears the captured laps and the rewards values.

Offsets v5.8 and earlier used wrongly: `+0x48` is inside a second 32-byte string at `+0x38`, `+0x464` and `+0x474` are 3D vectors, and `+0x8C` is an array the game takes a maximum over, not lap times.

---

## Memory Offsets & Pointer Safety

The plugin reads game state safely via memory addresses:
- **Game Session State**: Checks active game session pointers to distinguish between menu navigation and active racing.
- **Player Stats**: Reads player profile pointers for career cash and Raceday XP.
- **Sanitization**: All pointers undergo strict boundary checks (`if (ptr == nullptr) return;`) to ensure game stability across scene changes.
