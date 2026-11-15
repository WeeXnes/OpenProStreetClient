#pragma once

#include <string>
#include <vector>

// Event votes in a race day's lobby. F1 opens a window listing the race day's events (from the
// server); pick one and call a vote. Everyone in the room then sees a popup and votes with F1 (yes)
// or F2 (no). The server runs the vote: it passes with more than half of the room, holds the lobby
// countdown for up to 20 seconds, and makes the event the next one. [Vote] in the ini: Enabled,
// Key (F1), NoKey (F2).
namespace EventVote
{
    void Init();

    // From the server link's thread.
    void SetEvents(const std::vector<std::string>& names, int current);
    void OnVote(const std::string& type, const std::string& caller, int index, const std::string& eventName,
                int yes, int no, int total, int seconds, bool passed, const std::string& reason);
    // Not in a room any more, or a new connection.
    void Reset();

    // What the overlay draws.
    struct View
    {
        bool                     windowOpen = false;
        std::vector<std::string> events;
        int                      current = -1;  // the next event
        int                      selected = 0;
        bool                     voteActive = false;
        std::string              caller, eventName;
        int                      yes = 0, no = 0, total = 0;
        double                   secondsLeft = 0;
        int                      myVote = -1;   // -1 none yet, 0 no, 1 yes
        std::string              yesKey, noKey;
    };
    View Snapshot();

    // On the render thread, once a frame: reads the keys while the game is in front.
    void Poll(bool gameInFront);

    // From the overlay's window.
    void Select(int index);
    void CallSelected();
    void Close();
}
