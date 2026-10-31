#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The plugin's connection to the OpenProStreet server. It sits on the game's own data socket:
// everything the game sends is encrypted on the way out and decrypted on the way in, so the game
// doesn't know it's there. The plugin's own messages to the server travel the same way.
namespace ServerLink
{
    struct RaceReport {
        std::string trackKey;
        std::string trackName;
        std::string raceMode;
        std::string carKey;
        int position = 0;
        float raceTime = 0.0f; // the racer's own fields, exactly as the game sends them to the server
        float bestLap = 0.0f;
        float score = 0.0f;
        int lapsCompleted = 0;
        std::vector<float> lapTimes;
    };

    // Hooks the game's socket calls and loads the player's identity.
    void Init(uintptr_t baseModule);

    // The game's data socket connected to a server (netcodeWinsockPlatUDPSocket::Connect).
    void OnDataSocketConnected(uintptr_t socketHandle);

    // True while connected to a server that speaks the encrypted protocol.
    bool IsConnected();

    void SubmitRaceResult(const RaceReport& report);

    // The player picked quick match in the game's menus, so the next game they join is matched.
    void RequestQuickMatch();
    void CancelQuickMatch();
}
