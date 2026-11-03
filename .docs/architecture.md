# Architecture & Memory Maps

This document details the memory structure and track mapping table used in the OpenProStreet client.

---

## Game Executable Requirements

- **Executable Target**: `nfs.exe` v1.1 (Europe) "Battery 1.1 No-CD fix".
- **Architecture**: 32-bit (x86) PE executable.

---

## Track & Map Identification (`map.cpp`)

The track name comes from `[0xB309C0]`, which is `MassiveAds` (the in-game ad system), at `+0x05`. It is set while an event loads, about 35 seconds before the race started in a recorded session, and it stays set afterwards. Discord therefore shows the track and race mode only while `IsRaceActive` is true (`[0xAB9D7C] + 0x2D04`, the race object, is non-null). Race results still read the name after the race. `[0xB309B4]` is a console text buffer that held PunkBuster log lines; it is not a track source.

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

The car handle is loaded straight from the save file, so at the main menu it can be a car from an earlier session. `IsCurrentCarConfirmed` hides the car until a race is active, the handle changes, or the handle appears in the career car table (`FECareer +0x20`, filled when an event starts). The first car can therefore stay hidden in the menus until one of those happens. Race results read the handle directly.

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

## Server Link (`src/net`)

The game's reliable connection uses `netcodeWinsockPlatUDPSocket`: `socket` and `connect` at `0x827190`, `send` at `0x8270E2`, and a blocking `recv` at `0x827110` on its own thread. `sendto` and `recvfrom` are only used for LAN discovery (`netcodeWinsockPlatUDPBcastSocket`), which stays plaintext.

`ServerLink` replaces the `send`, `recv` and `closesocket` import slots at `0x96758C`, `0x967590` and `0x967584` (WS2_32 ordinals 19, 16 and 3). It does this only if all three hold the expected functions. `LANFix` tells it which socket is the data socket, and other sockets pass straight through.

`SecureClient` has no Windows code, so it can be tested without the game (`tests/secure_proxy.cpp`). The protocol is described in the daemon's `src/secure/secure_gateway.h`.

| File in `%LOCALAPPDATA%\\OpenProStreet` | Contents |
| :--- | :--- |
| `identity.key` | The PC's Ed25519 secret key, used for the encrypted connection. It isn't the login: an account's name and password work on any PC. |

The client doesn't remember servers. If a server never answers the handshake within 2 seconds, it's treated as an old server and the game's plaintext protocol is used.

---

## Race Result Blob

`OLMSG_RacerFinished` carries 0x70 bytes copied straight from `GRacerInfo +0x68` (`0x673C5A`), and `OLMSG_OfficialResults` copies them back (`0x656B00`). So the server already holds score, race time, laps, top speed and best lap for every car. The plugin adds what the server can't see: individual lap times, the track, the race mode and the finishing position.

`OLMSG_PlayerCarData`'s 4-byte field is the vehicle key, the same value as `FECarRecord +0x08`, so the server knows each player's car too.

---

## Quick Match

"Schnelles Spiel" and LAN Play are options in the same menu handler, `FEOnlineMainStateManager` at `0x5CA5E0` (state 2):

| Option | Original behaviour |
| :--- | :--- |
| `0x1F` | LAN Play: creates `FELanStateManager` as a child |
| `0x25` | Ranked quick match: logs in to EA, then `0x5A8E20` |
| `0x26` | Unranked quick match: logs in to EA, then `0x5A8E20` |

The EA login uses a different backend (`Fesl`, Theater), whose servers are gone. So the plugin changes options `0x25` and `0x26` to `0x1F`, which opens the LAN flow, and tells the server this player wants a match.

The same menu click also picks a race day, in case no room is open. `ListEvents` walks the career's race days, the data behind the "create race day" menus (`0x56C2E0`, `0x578EB0`), and groups each usable event by location. It then picks a random location, a difficulty from 1 to 4, and 1 to 4 events at that location. The server builds the race day from that:

| Hub field | Value | Source |
| :--- | :--- | :--- |
| Header +0 | Location key | Event +0x30 (`0x6828F0`) |
| Header +8 | Difficulty, 1 to 4 | `RACE_DAY_DIFFICULTY_%d` (`0x57023F`) |
| Event +0 | Event key | Career race day (`0x659AF0`) |
| Event +8 | Laps | Event +0x22 (`0x657CF0`) or a choice up to that |
| Event +0xC | Heats | Event +0x24 (`0x657C90`) |

Modes `0x13` to `0x15` are left out at difficulty 1 and 2, and mode 8 always, as the menus do (`0x579164`).

When the lobby's game list arrives (`0x7E15C0`), the plugin calls the lobby's join function (`0x7D2620`). The list's first entry is selected, and during a quick match the server only lists rooms the player can join, best first. With an empty list the lobby stays open, so the player can create a race day.

The player still goes through the LAN address and login screens.

---

## Memory Offsets & Pointer Safety

The plugin reads game state safely via memory addresses:
- **Game Session State**: Checks active game session pointers to distinguish between menu navigation and active racing.
- **Player Stats**: Reads player profile pointers for career cash and Raceday XP.
- **Sanitization**: All pointers undergo strict boundary checks (`if (ptr == nullptr) return;`) to ensure game stability across scene changes.
