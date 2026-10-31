#include "game/race_results.h"
#include "game/hooks.h"
#include "net/server_link.h"
#include "game/map.h"
#include "game/car.h"
#include "game/race_mode.h"
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
    static void* g_origPopulateData = nullptr;
    static void* g_origSetTimeBonus = nullptr;
    static void* g_origSetPoints = nullptr;
    static void* g_origLapChanged = nullptr;

    static float g_lastRewardsTime = 0.0f;
    static float g_lastRewardsPoints = 0.0f;
    static uint32_t g_lastFinishedEventId = 0;
    static time_t g_lastLogTimestamp = 0;
    static std::vector<float> g_localLapTimes;

    // GRaceStatus (nfs.exe v1.1)
    static const uintptr_t RACE_STATUS_RACERS = 0x20;
    static const uintptr_t RACE_STATUS_RACER_COUNT = 0x2CE0;
    static const int RACER_SIZE = 0x594;
    static const int MAX_RACERS = 8;

    // GRacerInfo (nfs.exe v1.1)
    static const uintptr_t RACER_SIMABLE_HANDLE = 0x10;
    static const uintptr_t RACER_AI_OPPONENT = 0x14;
    static const uintptr_t RACER_NAME = 0x18;
    static const uintptr_t RACER_RANK = 0x5C;
    static const uintptr_t RACER_SCORE = 0x6C;
    static const uintptr_t RACER_RACE_TIME_SNAPSHOT = 0x70;
    static const uintptr_t RACER_LAPS_COMPLETED = 0x74;
    static const uintptr_t RACER_TOP_SPEED_MS = 0x78;
    static const uintptr_t RACER_BEST_LAP = 0x80;
    static const uintptr_t RACER_LAST_LAP = 0x2D4;
    static const uintptr_t RACER_RACE_TIMER_ELAPSED = 0x3EC;

    static uintptr_t FindLocalRacer(uintptr_t baseModule, uintptr_t gRaceStatus)
    {
        if (gRaceStatus == 0 || IsBadReadPtr((void*)gRaceStatus, RACE_STATUS_RACER_COUNT + sizeof(int32_t)) != 0) {
            return 0;
        }

        int count = *(int32_t*)(gRaceStatus + RACE_STATUS_RACER_COUNT);
        if (count <= 0 || count > MAX_RACERS) {
            return 0;
        }

        uintptr_t racers = gRaceStatus + RACE_STATUS_RACERS;

        // Local player list at 0xACE444 (count at 0xACE44C), the one Game.PlayerIsLocal walks.
        // Each entry's simable is at entry - 8, and the simable's handle at +8 is what GRacerInfo stores.
        uintptr_t listBase = baseModule + 0x6CE444;
        uintptr_t countBase = baseModule + 0x6CE44C;
        if (IsBadReadPtr((void*)listBase, sizeof(uintptr_t)) == 0 && IsBadReadPtr((void*)countBase, sizeof(int32_t)) == 0) {
            uintptr_t list = *(uintptr_t*)listBase;
            int localCount = *(int32_t*)countBase;
            if (list != 0 && localCount > 0 && IsBadReadPtr((void*)list, sizeof(uintptr_t)) == 0) {
                uintptr_t entry = *(uintptr_t*)list;
                if (entry > 8 && IsBadReadPtr((void*)(entry - 8), sizeof(uintptr_t)) == 0) {
                    uintptr_t simable = *(uintptr_t*)(entry - 8);
                    if (simable != 0 && IsBadReadPtr((void*)(simable + 8), sizeof(uint32_t)) == 0) {
                        uint32_t handle = *(uint32_t*)(simable + 8);
                        for (int i = 0; handle != 0 && i < count; ++i) {
                            uintptr_t racer = racers + (i * RACER_SIZE);
                            if (*(uint32_t*)(racer + RACER_SIMABLE_HANDLE) == handle) {
                                return racer;
                            }
                        }
                    }
                }
            }
        }

        // Fallback: AI racers have an opponent object, so a single racer without one is the player.
        uintptr_t humanRacer = 0;
        for (int i = 0; i < count; ++i) {
            uintptr_t racer = racers + (i * RACER_SIZE);
            if (*(uintptr_t*)(racer + RACER_AI_OPPONENT) == 0) {
                if (humanRacer != 0) {
                    return 0;
                }
                humanRacer = racer;
            }
        }
        return humanRacer;
    }

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
        if (pos <= 0) return "Unknown";
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
        if (now - g_lastLogTimestamp < 2 && g_lastLogTimestamp != 0) {
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
            if (raceObj != 0 && IsBadReadPtr((void*)raceObj, 0x20) == 0) {
                for (int off = 0x04; off <= 0x18; off += 4) {
                    uintptr_t dataPtr = *(uintptr_t*)(raceObj + off);
                    if (dataPtr != 0 && IsBadReadPtr((void*)dataPtr, 0x24) == 0) {
                        uint32_t h = *(uint32_t*)(dataPtr + 0x00);
                        if (h > 0x1000) {
                            rawEventHash = h;
                            break;
                        }
                        h = *(uint32_t*)(dataPtr + 0x04);
                        if (h > 0x1000) {
                            rawEventHash = h;
                            break;
                        }
                    }
                }
            }
        }

        // Skip if duplicate event
        if (rawEventHash != 0 && rawEventHash == g_lastFinishedEventId && (now - g_lastLogTimestamp < 10)) {
            return;
        }

        // 2. Locate Human Player RacerInfo in GRaceStatus
        uintptr_t playerRacer = FindLocalRacer(baseModule, gRaceStatus);
        if (!playerRacer) {
            printf("[OpenProStreet] Could not find the local racer, race result not saved.\n");
            return;
        }

        // 3. Track & Mode & Car
        std::string rawMap = ReadRawMapIdentifier(baseModule);
        std::string displayMap = TranslateMapIdentifier(rawMap);
        std::string raceMode = ReadRaceMode(baseModule);
        if (raceMode.empty()) {
            switch (screenId) {
                case 5: raceMode = "Drag"; break;
                case 3: raceMode = "Drift"; break;
                case 4: raceMode = "Speed Challenge"; break;
                case 2: raceMode = "Grip"; break;
                default: raceMode = "Time Attack"; break;
            }
        }

        std::string rawCar = ReadRawCarIdentifier(baseModule);
        std::string displayCar = TranslateCarIdentifier(rawCar);

        bool isDrag = (screenId == 5 || raceMode.find("Drag") != std::string::npos || raceMode.find("Wheelie") != std::string::npos);
        bool isDrift = (screenId == 3 || raceMode.find("Drift") != std::string::npos);

        // 4. Extract Timing from RacerInfo
        std::vector<float> lapTimes = g_localLapTimes;
        std::vector<float> heatTimes;

        float lastLap = *(float*)(playerRacer + RACER_LAST_LAP);
        if (lapTimes.empty() && lastLap > 0.1f && lastLap < 3600.0f) {
            lapTimes.push_back(lastLap);
        }

        // Determine Total Finish Time
        float totalTime = 0.0f;
        float raceTimer = *(float*)(playerRacer + RACER_RACE_TIMER_ELAPSED);
        float raceTimeSnapshot = *(float*)(playerRacer + RACER_RACE_TIME_SNAPSHOT);
        if (raceTimer > 0.1f && raceTimer < 36000.0f) {
            totalTime = raceTimer;
        } else if (raceTimeSnapshot > 0.1f && raceTimeSnapshot < 36000.0f) {
            totalTime = raceTimeSnapshot;
        } else if (g_lastRewardsTime > 0.1f && g_lastRewardsTime < 3600.0f) {
            totalTime = g_lastRewardsTime;
        }

        // Determine Best Lap / Run
        float bestLap = *(float*)(playerRacer + RACER_BEST_LAP);
        if (bestLap <= 0.1f || bestLap >= 3600.0f) {
            bestLap = totalTime;
        }

        if (isDrag) {
            heatTimes.swap(lapTimes);
        }

        // 5. Extract Position from RacerInfo
        int position = 0;
        int rank = *(int32_t*)(playerRacer + RACER_RANK);
        if (rank >= 1 && rank <= MAX_RACERS) {
            position = rank;
        }

        // 6. Extract Top Speed (stored in m/s) from RacerInfo and Score
        float topSpeed = 0.0f;
        float topSpeedMs = *(float*)(playerRacer + RACER_TOP_SPEED_MS);
        if (topSpeedMs > 0.0f && topSpeedMs < 200.0f) {
            topSpeed = topSpeedMs * 3.6f;
        }

        float score = (g_lastRewardsPoints > 0.0f) ? g_lastRewardsPoints : 0.0f;
        if (isDrift && score <= 0.0f) {
            score = *(float*)(playerRacer + RACER_SCORE);
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
        printf("[OpenProStreet] === RACE COMPLETED ===\n");
        printf("[OpenProStreet] Driver:        %.31s\n", (const char*)(playerRacer + RACER_NAME));
        printf("[OpenProStreet] Event ID:      %s\n", res.eventId.c_str());
        printf("[OpenProStreet] Track:         %s\n", res.trackName.c_str());
        printf("[OpenProStreet] Mode:          %s\n", res.raceMode.c_str());
        printf("[OpenProStreet] Car:           %s\n", res.carName.c_str());
        printf("[OpenProStreet] Position:      %s\n", FormatPosition(res.position).c_str());

        if (isDrag) {
            for (size_t h = 0; h < res.heatTimes.size(); ++h) {
                printf("[OpenProStreet] Heat %u:        %s (%.3fs)\n", (unsigned)(h + 1), FormatSecondsToTime(res.heatTimes[h]).c_str(), res.heatTimes[h]);
            }
            if (res.bestLapSeconds > 0.0f) {
                printf("[OpenProStreet] Best Run:      %s (%.3fs)\n", FormatSecondsToTime(res.bestLapSeconds).c_str(), res.bestLapSeconds);
            }
        } else {
            if (res.totalTimeSeconds > 0.0f) {
                printf("[OpenProStreet] Total Time:    %s (%.3fs)\n", FormatSecondsToTime(res.totalTimeSeconds).c_str(), res.totalTimeSeconds);
            }
            if (res.bestLapSeconds > 0.0f && res.bestLapSeconds != res.totalTimeSeconds) {
                printf("[OpenProStreet] Best Lap:      %s (%.3fs)\n", FormatSecondsToTime(res.bestLapSeconds).c_str(), res.bestLapSeconds);
            }
            // Print individual laps breakdown
            for (size_t l = 0; l < res.lapTimes.size(); ++l) {
                printf("[OpenProStreet] Lap %u:         %s (%.3fs)\n", (unsigned)(l + 1), FormatSecondsToTime(res.lapTimes[l]).c_str(), res.lapTimes[l]);
            }
        }

        if (res.score > 0.0f) {
            printf("[OpenProStreet] Score / Pts:   %.0f pts\n", res.score);
        }
        if (res.topSpeedKmh > 0.0f) {
            printf("[OpenProStreet] Top Speed:     %.1f km/h\n", res.topSpeedKmh);
        }
        printf("============================================================\n\n");

        // Save JSONL record
        SaveRaceResultToJson(res);

        // These are the racer's own fields, the ones the game sends the server in OLMSG_RacerFinished
        // (GRacerInfo +0x68 onwards), so the server can check them against its own copy.
        ServerLink::RaceReport report;
        report.trackKey = res.trackKey;
        report.trackName = res.trackName;
        report.raceMode = res.raceMode;
        report.carKey = res.carKey;
        report.position = res.position;
        report.raceTime = raceTimeSnapshot;
        report.bestLap = *(float*)(playerRacer + RACER_BEST_LAP);
        report.score = *(float*)(playerRacer + RACER_SCORE);
        report.lapsCompleted = *(int32_t*)(playerRacer + RACER_LAPS_COMPLETED);
        report.lapTimes = isDrag ? res.heatTimes : res.lapTimes;
        ServerLink::SubmitRaceResult(report);
    }

    static void __attribute__((thiscall)) FePostRaceRewards_PopulateData_Hook(void* thisPtr, void* pParam)
    {
        reinterpret_cast<void(__attribute__((thiscall))*)(void*, void*)>(g_origPopulateData)(thisPtr, pParam);

        // After PopulateData finishes, SetTimeBonus & SetPoints have fully executed!
        ProcessPostRaceResults(g_baseModule, 0);
    }

    static void __attribute__((thiscall)) SetTimeBonus_Hook(void* thisPtr, void* dataPtr)
    {
        if (dataPtr && !IsBadReadPtr(dataPtr, 32)) {
            float* fData = (float*)dataPtr;
            for (int i = 0; i < 8; ++i) {
                float val = fData[i];
                if (val > 1.0f && val < 3600.0f) {
                    g_lastRewardsTime = val;
                }
            }
        }

        reinterpret_cast<void(__attribute__((thiscall))*)(void*, void*)>(g_origSetTimeBonus)(thisPtr, dataPtr);
    }

    static void __attribute__((thiscall)) SetPoints_Hook(void* thisPtr, void* dataPtr)
    {
        if (dataPtr && !IsBadReadPtr(dataPtr, 16)) {
            uint32_t* pData = (uint32_t*)dataPtr;
            if (pData[0] > 0 && pData[0] < 1000000) {
                g_lastRewardsPoints = (float)pData[0];
            }
        }

        reinterpret_cast<void(__attribute__((thiscall))*)(void*, void*)>(g_origSetPoints)(thisPtr, dataPtr);
    }

    static void __attribute__((thiscall)) LapChanged_Hook(void* thisPtr, int lap)
    {
        reinterpret_cast<void(__attribute__((thiscall))*)(void*, int)>(g_origLapChanged)(thisPtr, lap);

        uintptr_t gRaceStatusBase = g_baseModule + 0x6B9D7C;
        if (IsBadReadPtr((void*)gRaceStatusBase, sizeof(uintptr_t)) != 0) {
            return;
        }
        if ((uintptr_t)thisPtr != FindLocalRacer(g_baseModule, *(uintptr_t*)gRaceStatusBase)) {
            return;
        }

        // A racer's lap count starts at 0, so this is a new race or a restart.
        int lapsCompleted = *(int32_t*)((uintptr_t)thisPtr + RACER_LAPS_COMPLETED);
        if (lapsCompleted <= 0) {
            g_localLapTimes.clear();
            g_lastRewardsTime = 0.0f;
            g_lastRewardsPoints = 0.0f;
            return;
        }

        float lastLap = *(float*)((uintptr_t)thisPtr + RACER_LAST_LAP);
        if ((size_t)lapsCompleted > g_localLapTimes.size() && lastLap > 0.1f && lastLap < 3600.0f) {
            g_localLapTimes.push_back(lastLap);
        }
    }

    static void __attribute__((thiscall)) SinglePlayer_HandleScreenConstructed_Hook(void* thisPtr)
    {
        uint32_t screenId = 0;
        if (thisPtr && !IsBadReadPtr((void*)((uintptr_t)thisPtr + 4), sizeof(uint32_t))) {
            screenId = *(uint32_t*)((uintptr_t)thisPtr + 4);
        }

        if (screenId != 0) { // Screen 0 is handled via PopulateData hook
            ProcessPostRaceResults(g_baseModule, screenId);
        }

        reinterpret_cast<void(__attribute__((thiscall))*)(void*)>(g_origSinglePlayerHandleScreenConstructed)(thisPtr);
    }

    static void __attribute__((thiscall)) Online_HandleScreenConstructed_Hook(void* thisPtr)
    {
        uint32_t screenId = 0;
        if (thisPtr && !IsBadReadPtr((void*)((uintptr_t)thisPtr + 4), sizeof(uint32_t))) {
            screenId = *(uint32_t*)((uintptr_t)thisPtr + 4);
        }

        ProcessPostRaceResults(g_baseModule, screenId);

        reinterpret_cast<void(__attribute__((thiscall))*)(void*)>(g_origOnlineHandleScreenConstructed)(thisPtr);
    }

    void Init(uintptr_t baseModule)
    {
        if (!baseModule) return;
        g_baseModule = baseModule;

        printf("[OpenProStreet] Initializing Post-Race State Manager Hooks...\n");

        uintptr_t loc_5A7750 = baseModule + 0x1A7750;
        uintptr_t loc_5FA460 = baseModule + 0x1FA460;
        uintptr_t loc_5982F0 = baseModule + 0x1982F0;
        uintptr_t loc_614340 = baseModule + 0x214340;
        uintptr_t loc_614360 = baseModule + 0x214360;
        uintptr_t loc_695DE0 = baseModule + 0x295DE0;

        if (Hooks::InstallDetour((void*)loc_5A7750, (void*)SinglePlayer_HandleScreenConstructed_Hook, &g_origSinglePlayerHandleScreenConstructed, 7)) {
            printf("[OpenProStreet] SP PostRaceStateManager hook installed at 0x%p\n", (void*)loc_5A7750);
        }

        if (Hooks::InstallDetour((void*)loc_5FA460, (void*)Online_HandleScreenConstructed_Hook, &g_origOnlineHandleScreenConstructed, 7)) {
            printf("[OpenProStreet] Online PostRaceStateManager hook installed at 0x%p\n", (void*)loc_5FA460);
        }

        if (Hooks::InstallDetour((void*)loc_5982F0, (void*)FePostRaceRewards_PopulateData_Hook, &g_origPopulateData, 7)) {
            printf("[OpenProStreet] FePostRaceRewards::PopulateData hook installed at 0x%p\n", (void*)loc_5982F0);
        }

        if (Hooks::InstallDetour((void*)loc_614340, (void*)SetTimeBonus_Hook, &g_origSetTimeBonus, 7)) {
            printf("[OpenProStreet] SetTimeBonus hook installed at 0x%p\n", (void*)loc_614340);
        }

        if (Hooks::InstallDetour((void*)loc_614360, (void*)SetPoints_Hook, &g_origSetPoints, 7)) {
            printf("[OpenProStreet] SetPoints hook installed at 0x%p\n", (void*)loc_614360);
        }

        if (Hooks::InstallDetour((void*)loc_695DE0, (void*)LapChanged_Hook, &g_origLapChanged, 7)) {
            printf("[OpenProStreet] Lap hook installed at 0x%p\n", (void*)loc_695DE0);
        }
    }
}
