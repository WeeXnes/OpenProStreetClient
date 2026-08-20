#include "race_mode.h"

// Maps the internal event/race mode ID from eventData + 0x23 to a human-readable name
static std::string TranslateRaceModeId(int8_t modeId) {
    switch (modeId) {
        case 0:
        case 1:
        case 2:
            return "Grip";

        case 3:
            return "1/4 Mile Drag";
        case 4:
            return "1/2 Mile Drag";
        case 5:
        case 34:
            return "Drag";

        case 6:
        case 7:
        case 8:
        case 33:
        case 35:
            return "Drift";

        case 9:
        case 10:
        case 11:
            return "Speed Challenge";

        case 12:
        case 13:
        case 14:
            return "Top Speed Run";

        case 16:
        case 17:
        case 18:
            return "Sector Shootout";

        case 19:
        case 20:
        case 21:
            return "Drag";

        case 23:
        case 24:
        case 25:
            return "Time Attack";

        case 26:
        case 27:
        case 28:
            return "Wheelie Competition";

        case 15:
        case 22:
        case 29:
        case 30:
        case 31:
        case 32:
            return "Grip Class";

        default:
            if (modeId >= 0) {
                return "Race (Mode " + std::to_string(modeId) + ")";
            }
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
