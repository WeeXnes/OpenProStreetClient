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
    // A heat the local player just finished, before the server has counted it: a time or score
    // better than their best on the card becomes their best at once, so the next heat or lap
    // chases it. The server's answer replaces it when it comes.
    void ApplyLocalResult(const std::string& trackKey, const std::string& raceMode, float raceTime, float bestLap,
                          int laps, float score, float topSpeedKmh);
    // Asks the server where the player stands again, as after it counted a result.
    void RefreshStanding();
    // The XP a race earned, shown under its notice as the level bar filling up, with a level up
    // popup when it passes into a new level. levelStarts holds the XP each level starts at, from
    // levelBefore to the level after the one the player has now.
    struct XpGain
    {
        int                    gained = 0;
        long long              total = 0;
        int                    levelBefore = 0;
        std::vector<long long> levelStarts;
        // The race type's pool the same XP went to (grip, drift, drag or speed; empty from servers
        // before 1.27), the XP in it now, the place in it on the server, and the player's team.
        std::string            family;
        long long              pool = 0;
        int                    poolRank = 0;
        std::string            team;
        bool                   joinedTeam = false;
        bool                   crowned = false; // the race made them the pool's king
    };
    void PushNotice(const std::string& kind, const std::string& title, const std::vector<std::string>& lines,
                    const XpGain& xp = XpGain());
    // Real in-game player chat from the current race day room (never echoed Discord/server notices).
    void PushChat(const std::string& sender, int level, const std::string& text);
    // Who is in the room and the car each picked (empty while picking), shown while not racing. An
    // empty list hides the card.
    struct LobbyPlayer
    {
        std::string name;
        std::string car;
        std::string status; // ready, menus, picking, blocked, racing
        int         pingMs = -1;
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
    void OnConnectionRefused(const std::string& title, const std::vector<std::string>& lines);
}
