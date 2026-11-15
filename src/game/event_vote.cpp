#include "game/event_vote.h"

#include <windows.h>

#include <cstdlib>
#include <mutex>

#include "game/input_focus.h"
#include "game/overlay.h"
#include "log.h"
#include "net/server_link.h"

namespace EventVote
{
    static std::mutex g_lock;
    static bool  g_enabled = true;
    static int   g_key = VK_F1, g_noKey = VK_F2;
    static std::string g_keyName = "F1", g_noKeyName = "F2";

    // Guarded by g_lock.
    static std::vector<std::string> g_events;
    static int    g_current = -1;
    static bool   g_open = false;
    static int    g_selected = 0;
    static bool   g_voteActive = false;
    static std::string g_caller, g_eventName;
    static int    g_yes = 0, g_no = 0, g_total = 0;
    static DWORD  g_voteEnds = 0;
    static int    g_myVote = -1;

    static int ParseKey(const char* text, int fallback, std::string* name)
    {
        if ((text[0] == 'F' || text[0] == 'f') && text[1]) {
            const int number = atoi(text + 1);
            if (number >= 1 && number <= 12) {
                *name = "F" + std::to_string(number);
                return VK_F1 + number - 1;
            }
        }
        return fallback;
    }

    void Init()
    {
        const std::string& settings = Log::SettingsPath();
        if (settings.empty()) return;
        g_enabled = GetPrivateProfileIntA("Vote", "Enabled", 1, settings.c_str()) != 0;
        char text[16] = {};
        GetPrivateProfileStringA("Vote", "Key", "F1", text, sizeof(text), settings.c_str());
        g_key = ParseKey(text, VK_F1, &g_keyName);
        GetPrivateProfileStringA("Vote", "NoKey", "F2", text, sizeof(text), settings.c_str());
        g_noKey = ParseKey(text, VK_F2, &g_noKeyName);
    }

    // The game only gets the keyboard and mouse back once the window is closed.
    static void SetOpen(bool open)
    {
        if (g_open == open) return;
        g_open = open;
        InputFocus::SetCaptured(open);
    }

    void SetEvents(const std::vector<std::string>& names, int current)
    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_events = names;
        g_current = current;
        if (g_selected >= static_cast<int>(g_events.size())) g_selected = 0;
    }

    void Reset()
    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_events.clear();
        g_current = -1;
        g_voteActive = false;
        SetOpen(false);
    }

    void OnVote(const std::string& type, const std::string& caller, int index, const std::string& eventName,
                int yes, int no, int total, int seconds, bool passed, const std::string& reason)
    {
        (void)index;
        if (type == "refused") {
            Overlay::PushNotice("not_counted", "Can't call that vote", std::vector<std::string>{reason});
            return;
        }
        std::lock_guard<std::mutex> lock(g_lock);
        if (type == "ended") {
            g_voteActive = false;
            Overlay::PushNotice(passed ? "best" : "not_counted", passed ? "Vote passed" : "Vote failed",
                                std::vector<std::string>{passed ? eventName + " is the next event" : eventName + " stays as it was",
                                                         std::to_string(yes) + " of " + std::to_string(total) + " voted yes"});
            return;
        }
        // "started", or "update" (also how a player who joins during a vote hears of it).
        if (!g_voteActive || g_eventName != eventName || g_caller != caller) g_myVote = -1;
        g_voteActive = true;
        g_caller = caller;
        g_eventName = eventName;
        g_yes = yes;
        g_no = no;
        g_total = total;
        g_voteEnds = GetTickCount() + static_cast<DWORD>(seconds) * 1000;
        SetOpen(false);
    }

    View Snapshot()
    {
        std::lock_guard<std::mutex> lock(g_lock);
        View view;
        view.windowOpen = g_open;
        view.events = g_events;
        view.current = g_current;
        view.selected = g_selected;
        view.voteActive = g_voteActive;
        view.caller = g_caller;
        view.eventName = g_eventName;
        view.yes = g_yes;
        view.no = g_no;
        view.total = g_total;
        const int left = static_cast<int>(g_voteEnds - GetTickCount());
        view.secondsLeft = left > 0 ? left / 1000.0 : 0.0;
        view.myVote = g_myVote;
        view.yesKey = g_keyName;
        view.noKey = g_noKeyName;
        return view;
    }

    void Select(int index)
    {
        std::lock_guard<std::mutex> lock(g_lock);
        if (index >= 0 && index < static_cast<int>(g_events.size())) g_selected = index;
    }

    void Close()
    {
        std::lock_guard<std::mutex> lock(g_lock);
        SetOpen(false);
    }

    void CallSelected()
    {
        int index;
        {
            std::lock_guard<std::mutex> lock(g_lock);
            if (!g_open || g_voteActive || g_events.empty()) return;
            index = g_selected;
            SetOpen(false);
        }
        Log::Event(Log::Tag::Client, "Vote: calling a vote for event %d\n", index + 1);
        ServerLink::CallVote(index);
    }

    // Edge detection: true once per press.
    static bool Pressed(int key, bool front)
    {
        static bool down[256] = {};
        const bool now = front && (GetAsyncKeyState(key) & 0x8000) != 0;
        const bool pressed = now && !down[key & 0xFF];
        down[key & 0xFF] = now;
        return pressed;
    }

    void Poll(bool front)
    {
        if (!g_enabled) return;
        const bool key = Pressed(g_key, front), noKey = Pressed(g_noKey, front);
        const bool up = Pressed(VK_UP, front), dn = Pressed(VK_DOWN, front);
        const bool enter = Pressed(VK_RETURN, front), escape = Pressed(VK_ESCAPE, front);

        bool call = false;
        int vote = -1;
        {
            std::lock_guard<std::mutex> lock(g_lock);
            if (g_voteActive && GetTickCount() - (g_voteEnds + 3000) < 0x80000000u) g_voteActive = false; // the server's end went missing
            if (g_voteActive && g_myVote < 0 && (key || noKey)) {
                g_myVote = key ? 1 : 0;
                vote = g_myVote;
            } else if (g_open) {
                const int count = static_cast<int>(g_events.size());
                if (key || escape) SetOpen(false);
                else if (up && count) g_selected = (g_selected + count - 1) % count;
                else if (dn && count) g_selected = (g_selected + 1) % count;
                else if (enter) call = true;
            } else if (key && !g_voteActive) {
                if (g_events.size() < 2) {
                    Overlay::PushNotice("not_counted", "Nothing to vote on",
                                        std::vector<std::string>{g_events.empty() ? "Join a race day to call a vote." : "This race day has only one event."});
                } else {
                    g_selected = (g_current + 1) % static_cast<int>(g_events.size());
                    SetOpen(true);
                }
            }
        }
        if (call) CallSelected();
        if (vote >= 0) {
            Log::Event(Log::Tag::Client, "Vote: voting %s\n", vote ? "yes" : "no");
            ServerLink::CastVote(vote == 1);
        }
    }
}
