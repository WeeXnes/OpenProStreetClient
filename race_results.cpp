#include "race_results.h"
#include "map.h"
#include "car.h"
#include "race_mode.h"
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <vector>

namespace RaceResults
{
    static uintptr_t g_baseModule = 0;
    static void* g_origSinglePlayerHandleScreenConstructed = nullptr;
    static void* g_origOnlineHandleScreenConstructed = nullptr;

    static uint32_t g_lastFinishedEventId = 0;
    static time_t g_lastLogTimestamp = 0;

    static std::string FormatSecondsToTime(float seconds) {
        if (seconds <= 0.0f || seconds > 36000.0f) {
            return "0:00.000";
        }

        int totalMs = (int)(seconds * 1000.0f + 0.5f);
        int mins = totalMs / 60000;
        int secs = (totalMs % 60000) / 1000;
        int ms = totalMs % 1000;

        std::ostringstream oss;
        oss << mins << ":" << std::setfill('0') << std::setw(2) << secs << "." << std::setw(3) << ms;
        return oss.str();
    }

    static std::string FormatPosition(int pos) {
        if (pos <= 0) return "1st";
        switch (pos) {
            case 1: return "1st";
            case 2: return "2nd";
            case 3: return "3rd";
            default: return std::to_string(pos) + "th";
        }
    }

    static void SaveRaceResultToJson(const RaceResult& res) {
        std::ofstream file("race_results.jsonl", std::ios::app);
        if (!file.is_open()) {
            return;
        }

        std::ostringstream json;
        json << "{"
             << "\"timestamp\":" << res.timestamp << ","
             << "\"event_id\":\"" << res.eventId << "\","
             << "\"track_key\":\"" << res.trackKey << "\","
             << "\"track_name\":\"" << res.trackName << "\","
             << "\"race_mode\":\"" << res.raceMode << "\","
             << "\"car_key\":\"" << res.carKey << "\","
             << "\"car_name\":\"" << res.carName << "\","
             << "\"position\":" << res.position << ","
             << "\"total_time_s\":" << std::fixed << std::setprecision(3) << res.totalTimeSeconds << ","
             << "\"best_lap_s\":" << std::fixed << std::setprecision(3) << res.bestLapSeconds << ",";

        json << "\"heat_times\":[";
        for (size_t i = 0; i < res.heatTimes.size(); ++i) {
            json << std::fixed << std::setprecision(3) << res.heatTimes[i];
            if (i + 1 < res.heatTimes.size()) json << ",";
        }
        json << "],";

        json << "\"score\":" << std::fixed << std::setprecision(1) << res.score << ","
             << "\"top_speed_kmh\":" << std::fixed << std::setprecision(1) << res.topSpeedKmh << ","
             << "\"reaction_time_s\":" << std::fixed << std::setprecision(3) << res.reactionTime << ","
             << "\"lap_times\":[";

        for (size_t i = 0; i < res.lapTimes.size(); ++i) {
            json << std::fixed << std::setprecision(3) << res.lapTimes[i];
            if (i + 1 < res.lapTimes.size()) {
                json << ",";
            }
        }
        json << "]}";

        file << json.str() << "\n";
        file.flush();
    }

    static void ProcessPostRaceResults(uintptr_t baseModule, uint32_t screenId)
    {
        time_t now = time(nullptr);
        // Throttle duplicate calls in rapid succession
        if (now - g_lastLogTimestamp < 3 && g_lastLogTimestamp != 0) {
            return;
        }

        uintptr_t gRaceStatusBase = baseModule + 0x6B9D7C;
        if (IsBadReadPtr((void*)gRaceStatusBase, sizeof(uintptr_t)) != 0) {
            return;
        }

        uintptr_t gRaceStatus = *(uintptr_t*)gRaceStatusBase;
        if (gRaceStatus == 0 || IsBadReadPtr((void*)gRaceStatus, 0x100) != 0) {
            return;
        }

        // 1. Read Event ID / Hash
        uint32_t rawEventHash = 0;
        if (IsBadReadPtr((void*)(gRaceStatus + 0x2D04), sizeof(uintptr_t)) == 0) {
            uintptr_t raceObj = *(uintptr_t*)(gRaceStatus + 0x2D04);
            if (raceObj != 0 && IsBadReadPtr((void*)raceObj, 0x10) == 0) {
                uintptr_t eventData = *(uintptr_t*)(raceObj + 0x04);
                if (eventData != 0 && IsBadReadPtr((void*)eventData, 0x10) == 0) {
                    rawEventHash = *(uint32_t*)(eventData + 0x00);
                    if (rawEventHash == 0) {
                        rawEventHash = *(uint32_t*)(eventData + 0x04);
                    }
                }
            }
        }

        // Skip if duplicate event
        if (rawEventHash != 0 && rawEventHash == g_lastFinishedEventId && (now - g_lastLogTimestamp < 10)) {
            return;
        }

        uint32_t numRacers = 1;
        if (IsBadReadPtr((void*)(gRaceStatus + 0x2CE0), sizeof(uint32_t)) == 0) {
            numRacers = *(uint32_t*)(gRaceStatus + 0x2CE0);
        }

        // Locate local human player racer slot
        uintptr_t playerRacer = 0;
        for (uint32_t i = 0; i < numRacers && i < 8; ++i) {
            uintptr_t racer = gRaceStatus + 0x20 + (i * 0x594);
            if (IsBadReadPtr((void*)racer, 0x70)) break;

            uint32_t flags = *(uint32_t*)(racer + 0x68);
            if (flags & 0x02) { // Local human player
                playerRacer = racer;
                break;
            }
        }

        if (!playerRacer && IsBadReadPtr((void*)(gRaceStatus + 0x20), 0x70) == 0) {
            playerRacer = gRaceStatus + 0x20;
        }

        if (!playerRacer) return;

        // 2. Track & Mode & Car
        std::string rawMap = ReadRawMapIdentifier(baseModule);
        std::string displayMap = TranslateMapIdentifier(rawMap);
        std::string raceMode = ReadRaceMode(baseModule);
        if (raceMode.empty()) {
            switch (screenId) {
                case 5: raceMode = "Drag"; break;
                case 3: raceMode = "Drift"; break;
                case 4: raceMode = "Speed Challenge"; break;
                case 2: raceMode = "Grip"; break;
                default: raceMode = "Race"; break;
            }
        }

        std::string rawCar = ReadRawCarIdentifier(baseModule);
        std::string displayCar = TranslateCarIdentifier(rawCar);

        bool isDrag = (screenId == 5 || raceMode.find("Drag") != std::string::npos || raceMode.find("Wheelie") != std::string::npos);
        bool isDrift = (screenId == 3 || raceMode.find("Drift") != std::string::npos);
        bool isSpeed = (screenId == 4 || raceMode.find("Speed") != std::string::npos || raceMode.find("Sector") != std::string::npos);

        // 3. Timing and Lap Extraction
        std::vector<float> lapTimes;
        std::vector<float> heatTimes;
        float bestLap = 0.0f;
        float totalTime = 0.0f;

        float bestLapRecorded = 0.0f;
        if (IsBadReadPtr((void*)(playerRacer + 0x464), sizeof(float)) == 0) {
            bestLapRecorded = *(float*)(playerRacer + 0x464);
        }

        float finishTimeRecorded = 0.0f;
        if (IsBadReadPtr((void*)(playerRacer + 0x474), sizeof(float)) == 0) {
            finishTimeRecorded = *(float*)(playerRacer + 0x474);
        }

        if (isDrag) {
            // Drag: read Heat 1, Heat 2, Heat 3
            for (int off = 0x8C; off <= 0x94; off += 4) {
                if (IsBadReadPtr((void*)(playerRacer + off), sizeof(float)) == 0) {
                    float heat = *(float*)(playerRacer + off);
                    if (heat > 0.1f && heat < 300.0f) {
                        heatTimes.push_back(heat);
                        if (bestLap <= 0.0f || heat < bestLap) {
                            bestLap = heat;
                        }
                    }
                }
            }
            if (heatTimes.empty() && bestLapRecorded > 0.1f && bestLapRecorded < 300.0f) {
                heatTimes.push_back(bestLapRecorded);
                bestLap = bestLapRecorded;
            }
            totalTime = bestLap;
        } else if (isSpeed) {
            // Speed Challenge / Top Speed (Point-to-point / Single run)
            if (bestLapRecorded > 0.1f && bestLapRecorded < 36000.0f) {
                bestLap = bestLapRecorded;
            } else if (finishTimeRecorded > 0.1f && finishTimeRecorded < 36000.0f) {
                bestLap = finishTimeRecorded;
            } else if (IsBadReadPtr((void*)(playerRacer + 0x8C), sizeof(float)) == 0) {
                float l1 = *(float*)(playerRacer + 0x8C);
                if (l1 > 0.1f && l1 < 36000.0f) bestLap = l1;
            }
            totalTime = bestLap;
        } else {
            // Grip / Circuit: read laps
            for (int off = 0x8C; off <= 0xCC; off += 4) {
                if (IsBadReadPtr((void*)(playerRacer + off), sizeof(float)) == 0) {
                    float lap = *(float*)(playerRacer + off);
                    if (lap > 0.1f && lap < 36000.0f) {
                        lapTimes.push_back(lap);
                        if (bestLap <= 0.0f || lap < bestLap) {
                            bestLap = lap;
                        }
                        totalTime += lap;
                    }
                }
            }
            if (bestLap <= 0.0f && bestLapRecorded > 0.1f && bestLapRecorded < 36000.0f) {
                bestLap = bestLapRecorded;
            }
            if (totalTime <= 0.0f) {
                totalTime = bestLap;
            }
        }

        // 4. Score / Top Speed
        float score = 0.0f;
        float topSpeed = 0.0f;
        uintptr_t pVehicle = *(uintptr_t*)(playerRacer + 0x0C);
        if (pVehicle != 0 && IsBadReadPtr((void*)pVehicle, 0x80) == 0) {
            if (isDrift) {
                score = *(float*)(pVehicle + 0x6C);
            }
            if (isSpeed || isDrag) {
                topSpeed = *(float*)(pVehicle + 0x70);
            }
        }

        // 5. Position
        int position = 1;
        if (IsBadReadPtr((void*)(playerRacer + 0x48), sizeof(int32_t)) == 0) {
            int pos = *(int32_t*)(playerRacer + 0x48);
            if (pos >= 1 && pos <= 8) {
                position = pos;
            }
        }

        g_lastFinishedEventId = rawEventHash;
        g_lastLogTimestamp = now;

        std::ostringstream eventIdHex;
        if (rawEventHash != 0) {
            eventIdHex << "0x" << std::uppercase << std::hex << std::setfill('0') << std::setw(8) << rawEventHash;
        } else {
            eventIdHex << "EVENT_ACTIVE";
        }

        RaceResult res;
        res.eventId = eventIdHex.str();
        res.trackKey = rawMap;
        res.trackName = displayMap.empty() ? rawMap : displayMap;
        res.raceMode = raceMode;
        res.carKey = rawCar;
        res.carName = displayCar.empty() ? rawCar : displayCar;
        res.position = position;
        res.totalTimeSeconds = totalTime;
        res.bestLapSeconds = bestLap;
        res.lapTimes = lapTimes;
        res.heatTimes = heatTimes;
        res.score = score;
        res.topSpeedKmh = topSpeed;
        res.reactionTime = 0.0f;
        res.timestamp = (uint64_t)now;

        // Formatted Console Output for Online Leaderboard Proof-of-Concept
        printf("\n============================================================\n");
        printf("[ProStreetRPC] === RACE COMPLETED ===\n");
        printf("[ProStreetRPC] Event ID:      %s\n", res.eventId.c_str());
        printf("[ProStreetRPC] Track:         %s\n", res.trackName.c_str());
        printf("[ProStreetRPC] Mode:          %s\n", res.raceMode.c_str());
        printf("[ProStreetRPC] Car:           %s\n", res.carName.c_str());
        printf("[ProStreetRPC] Position:      %s\n", FormatPosition(res.position).c_str());

        if (isDrag) {
            for (size_t h = 0; h < res.heatTimes.size(); ++h) {
                printf("[ProStreetRPC] Heat %u Time:   %s (%.3fs)\n", (unsigned)(h + 1), FormatSecondsToTime(res.heatTimes[h]).c_str(), res.heatTimes[h]);
            }
            if (res.bestLapSeconds > 0.0f) {
                printf("[ProStreetRPC] Best Run Time: %s (%.3fs)\n", FormatSecondsToTime(res.bestLapSeconds).c_str(), res.bestLapSeconds);
            }
        } else {
            if (res.totalTimeSeconds > 0.0f) {
                printf("[ProStreetRPC] Total Time:    %s (%.3fs)\n", FormatSecondsToTime(res.totalTimeSeconds).c_str(), res.totalTimeSeconds);
            }
            if (res.bestLapSeconds > 0.0f && res.bestLapSeconds != res.totalTimeSeconds) {
                printf("[ProStreetRPC] Best Lap:      %s (%.3fs)\n", FormatSecondsToTime(res.bestLapSeconds).c_str(), res.bestLapSeconds);
            }
        }

        if (res.score > 0.0f) {
            printf("[ProStreetRPC] Score/Points:  %.0f pts\n", res.score);
        }
        if (res.topSpeedKmh > 0.0f) {
            printf("[ProStreetRPC] Top Speed:     %.1f km/h\n", res.topSpeedKmh);
        }
        printf("============================================================\n\n");

        // Save JSONL record
        SaveRaceResultToJson(res);
    }

    static void __attribute__((thiscall)) SinglePlayer_HandleScreenConstructed_Hook(void* thisPtr)
    {
        uint32_t screenId = 0;
        if (thisPtr && !IsBadReadPtr((void*)((uintptr_t)thisPtr + 4), sizeof(uint32_t))) {
            screenId = *(uint32_t*)((uintptr_t)thisPtr + 4);
        }

        reinterpret_cast<void(__attribute__((thiscall))*)(void*)>(g_origSinglePlayerHandleScreenConstructed)(thisPtr);

        ProcessPostRaceResults(g_baseModule, screenId);
    }

    static void __attribute__((thiscall)) Online_HandleScreenConstructed_Hook(void* thisPtr)
    {
        uint32_t screenId = 0;
        if (thisPtr && !IsBadReadPtr((void*)((uintptr_t)thisPtr + 4), sizeof(uint32_t))) {
            screenId = *(uint32_t*)((uintptr_t)thisPtr + 4);
        }

        reinterpret_cast<void(__attribute__((thiscall))*)(void*)>(g_origOnlineHandleScreenConstructed)(thisPtr);

        ProcessPostRaceResults(g_baseModule, screenId);
    }

    static bool InstallDetour(void* target, void* detour, void** original, size_t patchSize = 7)
    {
        if (!target || IsBadReadPtr(target, patchSize)) return false;

        DWORD oldProtect = 0;
        if (!VirtualProtect(target, patchSize, PAGE_EXECUTE_READWRITE, &oldProtect)) return false;

        BYTE* trampoline = (BYTE*)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!trampoline) {
            VirtualProtect(target, patchSize, oldProtect, &oldProtect);
            return false;
        }

        // Copy original bytes to trampoline
        memcpy(trampoline, target, patchSize);

        // Write JMP back from trampoline to target + patchSize
        trampoline[patchSize] = 0xE9;
        uintptr_t jmpBackOffset = ((uintptr_t)target + patchSize) - ((uintptr_t)trampoline + patchSize + 5);
        memcpy(&trampoline[patchSize + 1], &jmpBackOffset, 4);

        if (original) {
            *original = (void*)trampoline;
        }

        // Write JMP from target to detour
        std::vector<BYTE> jmpBuf(patchSize, 0x90);
        jmpBuf[0] = 0xE9;
        uintptr_t detourOffset = (uintptr_t)detour - ((uintptr_t)target + 5);
        memcpy(&jmpBuf[1], &detourOffset, 4);
        memcpy(target, jmpBuf.data(), patchSize);

        VirtualProtect(target, patchSize, oldProtect, &oldProtect);
        return true;
    }

    void Init(uintptr_t baseModule)
    {
        if (!baseModule) return;
        g_baseModule = baseModule;

        printf("[ProStreetRPC] Initializing Post-Race State Manager Hooks...\n");

        uintptr_t loc_5A7750 = baseModule + 0x1A7750;
        uintptr_t loc_5FA460 = baseModule + 0x1FA460;

        if (InstallDetour((void*)loc_5A7750, (void*)SinglePlayer_HandleScreenConstructed_Hook, &g_origSinglePlayerHandleScreenConstructed, 7)) {
            printf("[ProStreetRPC] SP PostRaceStateManager hook installed at 0x%p (orig trampoline: 0x%p)\n", (void*)loc_5A7750, g_origSinglePlayerHandleScreenConstructed);
        }

        if (InstallDetour((void*)loc_5FA460, (void*)Online_HandleScreenConstructed_Hook, &g_origOnlineHandleScreenConstructed, 7)) {
            printf("[ProStreetRPC] Online PostRaceStateManager hook installed at 0x%p (orig trampoline: 0x%p)\n", (void*)loc_5FA460, g_origOnlineHandleScreenConstructed);
        }
    }
}
