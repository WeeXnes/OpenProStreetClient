#ifndef PROSTREETRPC_RACE_RESULTS_H
#define PROSTREETRPC_RACE_RESULTS_H

#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

struct RaceResult {
    std::string eventId;        // Hex string of the event hash (e.g. "0x881F8EFA")
    std::string trackKey;       // Raw internal track identifier (e.g. "portland_b")
    std::string trackName;      // Display track name (e.g. "Portland International Raceway")
    std::string raceMode;       // Race mode (e.g. "Grip", "Drift", "1/4 Mile Drag")
    std::string carKey;         // Raw car key (e.g. "GTR")
    std::string carName;        // Translated car name (e.g. "Nissan GT-R (R35)")
    int position;               // Finish position (1 = 1st, 2 = 2nd, etc.)
    float totalTimeSeconds;     // Total race time in seconds
    float bestLapSeconds;       // Best lap time in seconds
    std::vector<float> lapTimes;// List of individual lap times
    float score;                // Score (e.g. Drift points, top speed, or 0)
    uint64_t timestamp;         // Unix epoch timestamp when finished
};

// Initializes and updates the race results lifecycle watcher
void UpdateRaceResultsTracker(uintptr_t baseModule);

#endif // PROSTREETRPC_RACE_RESULTS_H
