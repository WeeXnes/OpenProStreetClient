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

enum RaceTrackerState {
    TRACKER_IDLE,
    TRACKER_RACING,
    TRACKER_FINISHED
};

static RaceTrackerState g_trackerState = TRACKER_IDLE;
static uint32_t g_lastFinishedEventId = 0;
static float g_lastTotalTime = 0.0f;

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
         << "\"best_lap_s\":" << std::fixed << std::setprecision(3) << res.bestLapSeconds << ","
         << "\"score\":" << std::fixed << std::setprecision(1) << res.score << ","
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

void UpdateRaceResultsTracker(uintptr_t baseModule) {
    uintptr_t gRaceStatusBase = baseModule + 0x6B9D7C;
    if (IsBadReadPtr((void*)gRaceStatusBase, sizeof(uintptr_t)) != 0) {
        g_trackerState = TRACKER_IDLE;
        return;
    }

    uintptr_t gRaceStatus = *(uintptr_t*)gRaceStatusBase;
    if (gRaceStatus == 0 || IsBadReadPtr((void*)gRaceStatus, 0x2D10) != 0) {
        g_trackerState = TRACKER_IDLE;
        return;
    }

    // Check race status state (0x2CFC: 1 = in race, 2 = race over)
    uint32_t raceState = *(uint32_t*)(gRaceStatus + 0x2CFC);
    uint32_t numRacers = *(uint32_t*)(gRaceStatus + 0x2CE0);

    // Locate the local human player racer slot
    uintptr_t playerRacer = 0;
    for (uint32_t i = 0; i < numRacers && i < 8; ++i) {
        uintptr_t racer = gRaceStatus + 0x20 + (i * 0x594);
        if (IsBadReadPtr((void*)racer, 0x80)) break;

        uint32_t flags = *(uint32_t*)(racer + 0x68);
        if (flags & 0x02) { // Local human player
            playerRacer = racer;
            break;
        }
    }

    // Fallback to slot 0 if flags not found
    if (!playerRacer && IsBadReadPtr((void*)(gRaceStatus + 0x20), 0x80) == 0) {
        playerRacer = gRaceStatus + 0x20;
    }

    if (!playerRacer) {
        g_trackerState = TRACKER_IDLE;
        return;
    }

    // State management:
    // When racing starts:
    if (raceState == 1) {
        if (g_trackerState != TRACKER_RACING) {
            g_trackerState = TRACKER_RACING;
        }
    }

    // Check for race completion transition:
    // Either global raceState == 2 OR racer is finished while we were in TRACKER_RACING
    bool isRacerFinished = false;
    if (raceState == 2) {
        isRacerFinished = true;
    } else if (g_trackerState == TRACKER_RACING) {
        // Check if player vehicle has crossed the finish line
        uintptr_t pVehicle = *(uintptr_t*)(playerRacer + 0x0C);
        if (pVehicle != 0 && !IsBadReadPtr((void*)pVehicle, 0x70)) {
            // In drift, score > 0 and state transitions
            float driftScore = *(float*)(pVehicle + 0x6C);
            if (driftScore > 0.0f && raceState == 2) {
                isRacerFinished = true;
            }
        }
    }

    if (isRacerFinished && g_trackerState == TRACKER_RACING) {
        g_trackerState = TRACKER_FINISHED;

        // 1. Read Event ID / Hash
        uint32_t rawEventHash = 0;
        uintptr_t raceObj = *(uintptr_t*)(gRaceStatus + 0x2D04);
        if (raceObj != 0 && !IsBadReadPtr((void*)raceObj, 0x10)) {
            uintptr_t eventData = *(uintptr_t*)(raceObj + 0x04);
            if (eventData != 0 && !IsBadReadPtr((void*)eventData, 0x10)) {
                rawEventHash = *(uint32_t*)(eventData + 0x00);
                if (rawEventHash == 0) {
                    rawEventHash = *(uint32_t*)(eventData + 0x04);
                }
            }
        }

        std::ostringstream eventIdHex;
        if (rawEventHash != 0) {
            eventIdHex << "0x" << std::uppercase << std::hex << std::setfill('0') << std::setw(8) << rawEventHash;
        } else {
            eventIdHex << "EVENT_GENERIC";
        }

        // 2. Track & Mode & Car
        std::string rawMap = ReadRawMapIdentifier(baseModule);
        std::string displayMap = TranslateMapIdentifier(rawMap);
        std::string raceMode = ReadRaceMode(baseModule);
        std::string rawCar = ReadRawCarIdentifier(baseModule);
        std::string displayCar = TranslateCarIdentifier(rawCar);

        // 3. Timing and Laps
        float totalTime = *(float*)(playerRacer + 0x428);
        if (totalTime <= 0.0f) {
            totalTime = *(float*)(playerRacer + 0x458);
        }

        std::vector<float> lapTimes;
        float bestLap = 0.0f;

        for (int off = 0x8C; off <= 0xCC; off += 4) {
            float lap = *(float*)(playerRacer + off);
            if (lap > 0.001f && lap < 36000.0f) {
                lapTimes.push_back(lap);
                if (bestLap <= 0.0f || lap < bestLap) {
                    bestLap = lap;
                }
            }
        }

        if (totalTime <= 0.0f && !lapTimes.empty()) {
            for (float l : lapTimes) totalTime += l;
        }

        // 4. Score
        float score = 0.0f;
        uintptr_t pVehicle = *(uintptr_t*)(playerRacer + 0x0C);
        if (pVehicle != 0 && !IsBadReadPtr((void*)(pVehicle + 0x6C), sizeof(float))) {
            float driftScore = *(float*)(pVehicle + 0x6C);
            if (driftScore > 0.0f) {
                score = driftScore;
            }
        }

        int position = 1; // Player 0 default

        // Prevent duplicate trigger for identical time/event
        if (rawEventHash == g_lastFinishedEventId && totalTime == g_lastTotalTime && totalTime > 0.0f) {
            return;
        }

        g_lastFinishedEventId = rawEventHash;
        g_lastTotalTime = totalTime;

        RaceResult res;
        res.eventId = eventIdHex.str();
        res.trackKey = rawMap;
        res.trackName = displayMap.empty() ? rawMap : displayMap;
        res.raceMode = raceMode.empty() ? "Race" : raceMode;
        res.carKey = rawCar;
        res.carName = displayCar.empty() ? rawCar : displayCar;
        res.position = position;
        res.totalTimeSeconds = totalTime;
        res.bestLapSeconds = (bestLap > 0.0f) ? bestLap : totalTime;
        res.lapTimes = lapTimes;
        res.score = score;
        res.timestamp = (uint64_t)time(nullptr);

        // Print formatted result to console
        printf("\n============================================================\n");
        printf("[ProStreetRPC] === RACE FINISHED ===\n");
        printf("[ProStreetRPC] Event ID:      %s\n", res.eventId.c_str());
        printf("[ProStreetRPC] Track:         %s\n", res.trackName.c_str());
        printf("[ProStreetRPC] Mode:          %s\n", res.raceMode.c_str());
        printf("[ProStreetRPC] Car:           %s\n", res.carName.c_str());
        printf("[ProStreetRPC] Position:      %s\n", FormatPosition(res.position).c_str());
        printf("[ProStreetRPC] Total Time:    %s (%.3fs)\n", FormatSecondsToTime(res.totalTimeSeconds).c_str(), res.totalTimeSeconds);
        if (res.bestLapSeconds > 0.0f) {
            printf("[ProStreetRPC] Best Lap:      %s (%.3fs)\n", FormatSecondsToTime(res.bestLapSeconds).c_str(), res.bestLapSeconds);
        }
        if (res.score > 0.0f) {
            printf("[ProStreetRPC] Score/Points:  %.0f pts\n", res.score);
        }
        printf("============================================================\n\n");

        // Save to JSONL
        SaveRaceResultToJson(res);
    }
}
