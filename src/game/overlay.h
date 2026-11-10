#pragma once

#include <cstdint>
#include <string>
#include <vector>

// An information overlay drawn over the game, in the style of OpenProStreet's website: a card with
// where the player stands on the event they're racing, and short notifications about their results.
// It only shows things; it never takes the mouse or keyboard.
namespace Overlay
{
    struct Board
    {
        std::string metric; // best_lap, race_time, score or top_speed
        int         laps = 0;
        double      record = 0.0;
        std::string recordHolder;
        double      best = 0.0; // 0 when the player has no result here
        int         place = 0;
        int         players = 0;
        double      beat = 0.0; // 0 when the player holds the record
        std::string beatHolder;
    };

    // Reads [Overlay] from the client's ini and hooks the display. Call before the game makes it.
    void Init(uintptr_t baseModule);

    // The game's state, from the client's polling thread. A new track or mode asks the server where
    // the player stands; outside a race the card is hidden.
    void OnGameState(bool racing, const std::string& rawTrack, const std::string& trackName, const std::string& raceMode);

    // From the server.
    void SetStanding(const std::string& trackKey, const std::string& raceMode, const std::vector<Board>& boards);
    void PushNotice(const std::string& kind, const std::string& title, const std::vector<std::string>& lines);
}
