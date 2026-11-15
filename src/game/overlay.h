#pragma once

#include <cstdint>
#include <string>
#include <utility>
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
    void Init(uintptr_t baseModule, const std::string& version);

    // The game's state, from the client's polling thread. A new track or mode asks the server where
    // the player stands; outside a race the card is hidden.
    void OnGameState(bool racing, const std::string& rawTrack, const std::string& trackName, const std::string& raceMode);

    // From the server.
    void SetStanding(const std::string& trackKey, const std::string& raceMode, const std::vector<Board>& boards);
    void PushNotice(const std::string& kind, const std::string& title, const std::vector<std::string>& lines);
    // Who is in the room and the car each picked (empty while picking), shown while not racing. An
    // empty list hides the card.
    struct LobbyPlayer
    {
        std::string name;
        std::string car;
        std::string status; // ready, menus, picking, blocked, racing
    };

    struct LobbyTimerState
    {
        int         seconds = -1;
        bool        paused = false;
        std::string reason; // vote, car, wait, ready
        std::string who;
        int         ready = 0;
        int         total = 0;
        int         allReadyIn = 0;
    };

    void SetLobbyState(const std::string& room, const std::string& atmosphere, int eventIndex, int eventCount,
                       const std::vector<LobbyPlayer>& players);
    void SetLobbyTimer(const LobbyTimerState& timer);
    bool GetPausedTimerSeconds(int* outSeconds);
    void ResetRoomState();

    // The encrypted connection to the server is up, which happens on logging in from the main menu.
    // Shows a short notice, so it's clear the overlay works before the first race.
    void OnConnected();
}
