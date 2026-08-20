#include "race_mode.h"

// Maps the internal event/race mode ID from eventData + 0x23 to a human-readable name
// using the exact game engine classifier table from 0x00657E10
static std::string TranslateRaceModeId(int8_t modeId) {
    if (modeId < 0 || modeId >= 36) {
        return "";
    }

    static const uint8_t tbl[36] = {
        0, 0, 0, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4, 4, 10,
        5, 5, 5, 6, 6, 6, 10, 7, 7, 7, 8, 8, 8, 9, 9, 9, 10, 2, 1, 2
    };

    uint8_t case_idx = tbl[modeId];
    static const int case_returns[11] = {
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, -1
    };
    int category = case_returns[case_idx];

    switch (category) {
        case 1:
            return "Speed Challenge";
        case 2:
            return "Top Speed Run";
        case 3:
            return "Grip";
        case 4:
            return "Sector Shootout";
        case 5:
            return "Time Attack";
        case 6:
            return "1/2 Mile Drag";
        case 7:
            return "Wheelie Competition";
        case 8:
            return "Drift";
        case 9:
            return "1/4 Mile Drag";
        case 10:
            return "Grip";
        default:
            return "";
    }
}

std::string ReadRaceMode(uintptr_t baseModule) {
    uintptr_t gRaceStatusBase = baseModule + 0x6B9D7C;
    if (IsBadReadPtr((void*)gRaceStatusBase, sizeof(uintptr_t)) == 0) {
        uintptr_t gRaceStatus = *(uintptr_t*)gRaceStatusBase;
        if (gRaceStatus != 0 && IsBadReadPtr((void*)(gRaceStatus + 0x2D04), sizeof(uintptr_t)) == 0) {
            uintptr_t raceObj = *(uintptr_t*)(gRaceStatus + 0x2D04);
            if (raceObj != 0 && IsBadReadPtr((void*)(raceObj + 0x04), sizeof(uintptr_t)) == 0) {
                uintptr_t eventData = *(uintptr_t*)(raceObj + 0x04);
                if (eventData != 0 && IsBadReadPtr((void*)(eventData + 0x23), 1) == 0) {
                    int8_t raceModeId = *(int8_t*)(eventData + 0x23);
                    return TranslateRaceModeId(raceModeId);
                }
            }
        }
    }

    return "";
}
