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
        std::string gameFiles; // VaultFixes::FilesHash()
        std::string speedLimit; // VaultFixes::SpeedLimit()
    };

    // Hooks the game's socket calls and loads the player's identity.
    void Init(uintptr_t baseModule);

    // The game's data socket connected to a server (netcodeWinsockPlatUDPSocket::Connect).
    void OnDataSocketConnected(uintptr_t socketHandle);

    // True while connected to a server that speaks the encrypted protocol.
    bool IsConnected();

    void SubmitRaceResult(const RaceReport& report);

    // Asks where the player stands on the event they're about to race, for the overlay. The server
    // knows the layout and lap count from the room. The answer goes to Overlay::SetStanding.
    void RequestStanding(const std::string& trackKey, const std::string& raceMode, const std::string& speedLimit);

    // Event votes in the room (EventVote): call one for the race day's event at this index, or vote.
    void CallVote(int eventIndex);
    void CastVote(bool yes);

    // The player picked quick match in the game's menus, so the server should match them. This
    // happens before the game connects, so it's sent as soon as the connection starts.
    // location, difficulty and events ("key:laps:heats,...") are the race day to create if no room
    // is open. events can be empty.
    void RequestQuickMatch(bool ranked, const std::string& location, int difficulty, const std::string& events);
    void CancelQuickMatch();
}
