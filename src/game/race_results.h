#ifndef OPENPROSTREET_RACE_RESULTS_H
#define OPENPROSTREET_RACE_RESULTS_H

#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

struct RaceResult {
    std::string eventId;         // Hex string of the event hash (e.g. "0x881F8EFA")
    std::string trackKey;        // Raw internal track identifier (e.g. "autobahn_gd")
    std::string trackName;       // Display track name (e.g. "Autobahnring")
    std::string raceMode;        // Race mode (e.g. "Speed Challenge", "Grip", "1/4 Mile Drag")
    std::string carKey;          // Raw car key (e.g. "GTR")
    std::string carName;         // Translated car name (e.g. "Nissan GT-R (R35)")
    int position;                // Finish position (1 = 1st, 2 = 2nd, etc.)
    float totalTimeSeconds;      // Total race time or best drag run in seconds
    float bestLapSeconds;        // Best lap time in seconds
    std::vector<float> lapTimes; // List of individual lap times
    std::vector<float> heatTimes;// List of drag heat times (Heat 1, Heat 2, Heat 3)
    float score;                 // Score (e.g. Drift points)
    float topSpeedKmh;           // Top speed in km/h
    float reactionTime;          // Drag reaction time in seconds
    uint64_t timestamp;          // Unix epoch timestamp when finished
};

namespace RaceResults
{
    // Initializes the Post-Race State Manager trampoline hooks
    void Init(uintptr_t baseModule);
}

#endif // OPENPROSTREET_RACE_RESULTS_H
