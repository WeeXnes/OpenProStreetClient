#include "game/overlay.h"
#include "game/race_results.h"
#include "game/reshifted.h"

#include <windows.h>
#include <d3d9.h>

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <deque>

#include "imgui.h"
#include "backends/imgui_impl_dx9.h"

#include "game/car.h"
#include "game/car_lock.h"
#include "game/d3d_hooks.h"
#include "game/event_vote.h"
#include "game/vault_fixes.h"
#include "log.h"
#include "net/server_link.h"

namespace Overlay
{
    // ---- Settings, from [Overlay] in the client's ini ----

    enum Corner { TopLeft, TopRight, BottomLeft, BottomRight };
    static bool g_enabled = true;
    static bool g_notices = true;
    static bool g_hudVisible = true;
    static int  g_toggleKey = VK_F3;
    static int  g_scoreboardKey = VK_TAB;
    static std::string g_toggleKeyName = "F3";
    static std::string g_scoreboardKeyName = "TAB";
    static Corner g_corner = TopRight;
    static float g_userScale = 1.0f;
    static uintptr_t g_baseModule = 0;
    static std::string g_version;
    static bool g_drawnOnce = false;
    static bool g_test = false;                  // [Overlay] Test=1: always show a small panel
    static IDirect3DDevice9* g_device = nullptr; // the device ImGui draws with
    static int g_devicesCreated = 0;
    static double g_firstDrawAt = 0.0;
    static int g_frames = 0, g_failedFrames = 0;
    static bool g_reported = false;

    // ---- What to show, written by the game state and network threads, read when drawing ----

    struct Notice
    {
        std::string kind, title;
        std::vector<std::string> lines;
        XpGain xp; // gained is 0 when the notice has no level bar
        double duration = 7.0;
        double shownAt = -1.0; // seconds on the overlay's clock, set when first drawn
    };

    static CRITICAL_SECTION g_lock;
    static bool g_racing = false;
    // Room and lobby state, from the server. Shown while not racing.
    static std::string g_lobbyRoom;
    static std::string g_lobbyAtmosphere;
    static int g_lobbyEventIndex = 0;
    static int g_lobbyEventCount = 0;
    static std::vector<LobbyPlayer> g_lobbyPlayers;
    static LobbyTimerState g_lobbyTimer;
    static bool g_lobbyCarsShown = true;
    static bool g_paceBarShown = true;
    static std::string g_trackKey, g_trackName, g_raceMode;
    static std::string g_requestedTrack, g_requestedMode;
    static std::string g_standingTrack, g_standingMode;
    static std::vector<Board> g_boards;
    static std::deque<Notice> g_noticeQueue;

    struct Guard
    {
        Guard() { EnterCriticalSection(&g_lock); }
        ~Guard() { LeaveCriticalSection(&g_lock); }
    };

    // ---- Drawing ----

    static bool g_ready = false;      // ImGui is set up on the device
    static bool g_deviceLost = false; // between a reset's start and its end
    static ImFont* g_fontBody = nullptr;
    static ImFont* g_fontSmall = nullptr;
    static ImFont* g_fontMono = nullptr;
    static ImFont* g_fontMicro = nullptr;
    static float g_fontScaleBase = 1.0f; // the scale the fonts were made at
    static LARGE_INTEGER g_frequency, g_start, g_last;

    // DESIGN.md's tokens.
    static const ImVec4 Surface(0.039f, 0.039f, 0.039f, 0.90f); // --surface #0a0a0a
    static const ImVec4 Line(0.122f, 0.122f, 0.122f, 1.0f);      // --line #1f1f1f
    static const ImVec4 Text1(0.980f, 0.980f, 0.980f, 1.0f);     // --text #fafafa
    static const ImVec4 Text2(0.639f, 0.639f, 0.639f, 1.0f);     // --text-2 #a3a3a3
    static const ImVec4 Text3(0.502f, 0.502f, 0.502f, 1.0f);     // --text-3 #808080
    static const ImVec4 Success(0.290f, 0.871f, 0.502f, 1.0f);   // #4ade80
    static const ImVec4 Warning(0.984f, 0.749f, 0.141f, 1.0f);   // #fbbf24
    static const ImVec4 Gold(0.910f, 0.773f, 0.278f, 1.0f);      // #e8c547
    static const ImVec4 Silver(0.769f, 0.769f, 0.769f, 1.0f);    // #c4c4c4
    static const ImVec4 Bronze(0.788f, 0.553f, 0.369f, 1.0f);    // #c98d5e
    static const ImVec4 Danger(0.937f, 0.267f, 0.267f, 1.0f);    // #ef4444
    static const ImVec4 Info(0.376f, 0.647f, 0.980f, 1.0f);      // #60a5fa

    static double Now()
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return static_cast<double>(now.QuadPart - g_start.QuadPart) / static_cast<double>(g_frequency.QuadPart);
    }

    static std::string FontPath(const char* file)
    {
        char windows[MAX_PATH] = {};
        GetWindowsDirectoryA(windows, MAX_PATH);
        std::string path = std::string(windows) + "\\Fonts\\" + file;
        return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES ? path : "";
    }

    // Segoe UI and Consolas on Windows. Proton has Liberation fonts under the Arial and Courier
    // names, and if none is there ImGui's own font still works.
    static ImFont* LoadFont(const char* const* files, float size)
    {
        ImGuiIO& io = ImGui::GetIO();
        for (const char* const* f = files; *f; ++f)
        {
            std::string path = FontPath(*f);
            if (!path.empty())
            {
                ImFont* font = io.Fonts->AddFontFromFileTTF(path.c_str(), size);
                if (font) return font;
            }
        }
        ImFontConfig config;
        config.SizePixels = size;
        return io.Fonts->AddFontDefault(&config);
    }

    static float ScaleFor(IDirect3DDevice9* device)
    {
        IDirect3DSurface9* back = nullptr;
        float height = 1080.0f;
        if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) && back)
        {
            D3DSURFACE_DESC desc;
            if (SUCCEEDED(back->GetDesc(&desc))) height = static_cast<float>(desc.Height);
            back->Release();
        }
        float scale = height / 1080.0f;
        if (scale < 0.6f) scale = 0.6f;
        return scale * g_userScale;
    }

    static void Style(float s)
    {
        ImGuiStyle& style = ImGui::GetStyle();
        style = ImGuiStyle();
        style.WindowRounding = 12.0f * s;
        style.WindowBorderSize = 1.0f;
        style.WindowPadding = ImVec2(16.0f * s, 14.0f * s);
        style.ItemSpacing = ImVec2(8.0f * s, 4.0f * s);
        style.CellPadding = ImVec2(0.0f, 1.0f * s);
        style.Colors[ImGuiCol_WindowBg] = Surface;
        style.Colors[ImGuiCol_Border] = Line;
        style.Colors[ImGuiCol_Text] = Text1;
        style.Colors[ImGuiCol_Separator] = Line;
        style.Colors[ImGuiCol_TableBorderLight] = Line;
    }

    // The game can make more than one device, and only the one it presents with reaches the screen.
    // The overlay follows whichever device presents.
    static void UseDevice(IDirect3DDevice9* device)
    {
        if (device == g_device) return;
        ImGui_ImplDX9_Shutdown();
        g_deviceLost = false;
        if (!ImGui_ImplDX9_Init(device))
        {
            Log::Warn("The overlay couldn't start on the game's new display device, so it's off.\n");
            g_enabled = false;
            g_ready = false;
            return;
        }
        g_device = device;
        Log::Event(Log::Tag::Client, "Overlay moved to the game's new display device\n");
    }

    static void DeviceCreated(IDirect3DDevice9* device, HWND, const D3DPRESENT_PARAMETERS*)
    {
        ++g_devicesCreated;
        Log::Event(Log::Tag::Client, "The game made display device %d\n", g_devicesCreated);
        if (!g_enabled) return;
        if (g_ready)
        {
            UseDevice(device);
            return;
        }
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange;

        const float s = ScaleFor(device);
        g_fontScaleBase = s;
        static const char* const body[] = { "segoeui.ttf", "arial.ttf", "tahoma.ttf", nullptr };
        static const char* const mono[] = { "consola.ttf", "cour.ttf", "lucon.ttf", nullptr };
        g_fontBody = LoadFont(body, 16.0f * s);
        g_fontSmall = LoadFont(body, 13.5f * s);
        g_fontMono = LoadFont(mono, 15.0f * s);
        g_fontMicro = LoadFont(mono, 11.0f * s);
        Style(s);

        if (!ImGui_ImplDX9_Init(device))
        {
            Log::Warn("The overlay couldn't start on this display, so it's off.\n");
            ImGui::DestroyContext();
            g_enabled = false;
            return;
        }
        QueryPerformanceFrequency(&g_frequency);
        QueryPerformanceCounter(&g_start);
        g_last = g_start;
        g_device = device;
        g_ready = true;
        Log::Event(Log::Tag::Client, "Overlay ready\n");
    }

    static void BeforeReset(IDirect3DDevice9*)
    {
        if (!g_ready) return;
        g_deviceLost = true;
        ImGui_ImplDX9_InvalidateDeviceObjects();
    }

    static void AfterReset(IDirect3DDevice9* device, bool succeeded)
    {
        if (!g_ready) return;
        if (succeeded)
        {
            ImGui_ImplDX9_CreateDeviceObjects();
            // The fonts keep their size; a new resolution scales them from it.
            ImGui::GetIO().FontGlobalScale = ScaleFor(device) / g_fontScaleBase;
            g_deviceLost = false;
        }
    }

    static std::string FormatValue(const std::string& metric, double value)
    {
        char buffer[48];
        if (metric == "best_lap" || metric == "race_time")
        {
            const int minutes = static_cast<int>(value / 60.0);
            snprintf(buffer, sizeof(buffer), "%d:%06.3f", minutes, value - minutes * 60.0);
        }
        else if (metric == "score")
        {
            snprintf(buffer, sizeof(buffer), "%.0f pts", value);
        }
        else if (metric == "wheelie")
        {
            snprintf(buffer, sizeof(buffer), "%.0f m", value); // the longest wheelie, as the game shows it
        }
        else
        {
            snprintf(buffer, sizeof(buffer), "%.1f km/h", value);
        }
        return buffer;
    }

    static std::string BoardLabel(const Board& b)
    {
        if (b.metric == "best_lap") return "BEST LAP";
        if (b.metric == "race_time") return b.laps == 1 ? "RACE TIME \xC2\xB7 1 LAP" : "RACE TIME \xC2\xB7 " + std::to_string(b.laps) + " LAPS";
        if (b.metric == "score") return "SCORE";
        if (b.metric == "wheelie") return "LONGEST WHEELIE";
        return "TOP SPEED";
    }

    static ImVec4 PlaceColour(int place)
    {
        return place == 1 ? Gold : place == 2 ? Silver : place == 3 ? Bronze : Text1;
    }

    static void Row(const char* label, const std::string& value, const ImVec4& valueColour, const std::string& note, const ImVec4& noteColour)
    {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::PushFont(g_fontSmall);
        ImGui::TextColored(Text2, "%s", label);
        ImGui::PopFont();
        ImGui::TableSetColumnIndex(1);
        ImGui::PushFont(g_fontMono);
        ImGui::TextColored(valueColour, "%s", value.c_str());
        ImGui::PopFont();
        ImGui::TableSetColumnIndex(2);
        ImGui::PushFont(g_fontSmall);
        ImGui::TextColored(noteColour, "%s", note.c_str());
        ImGui::PopFont();
    }

    static const ImGuiWindowFlags PanelFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus;

    static std::string FormatClock(int seconds)
    {
        if (seconds < 0) seconds = 0;
        char buf[16];
        snprintf(buf, sizeof(buf), "%d:%02d", seconds / 60, seconds % 60);
        return buf;
    }

    static std::string FormatSignedDelta(const std::string& metric, double delta)
    {
        const char* sign = delta >= 0.0 ? "+" : "-";
        const double mag = delta < 0.0 ? -delta : delta;
        char buf[48];
        if (metric == "best_lap" || metric == "race_time")
        {
            if (mag >= 60.0)
            {
                const int m = static_cast<int>(mag / 60.0);
                snprintf(buf, sizeof(buf), "%s%d:%06.3f", sign, m, mag - m * 60.0);
            }
            else
            {
                snprintf(buf, sizeof(buf), "%s%.3f s", sign, mag);
            }
            return buf;
        }
        if (metric == "top_speed")
        {
            snprintf(buf, sizeof(buf), "%s%.1f km/h", sign, mag);
            return buf;
        }
        if (metric == "wheelie")
        {
            snprintf(buf, sizeof(buf), "%s%.1f m", sign, mag);
            return buf;
        }
        snprintf(buf, sizeof(buf), "%s%.0f pts", sign, mag);
        return buf;
    }

    static const Board* PickPaceBoard(const std::vector<Board>& boards, bool* oneLapRace)
    {
        *oneLapRace = false;
        const Board* bestLap = nullptr;
        const Board* raceTime = nullptr;
        const Board* topSpeed = nullptr;
        for (const Board& b : boards)
        {
            if (b.metric == "score") return &b;
            if (b.metric == "wheelie") return &b;
            if (b.metric == "best_lap") bestLap = &b;
            else if (b.metric == "race_time") raceTime = &b;
            else if (b.metric == "top_speed") topSpeed = &b;
        }
        if (raceTime && raceTime->laps == 1)
        {
            *oneLapRace = true;
            return raceTime;
        }
        if (bestLap) return bestLap;
        if (raceTime) return raceTime;
        return topSpeed;
    }

    static void DrawPaceBar(const std::string& mode, const std::vector<Board>& boards, const RaceResults::LivePace& pace, float s)
    {
        if (!pace.active) return;

        bool oneLapRace = false;
        const Board* board = PickPaceBoard(boards, &oneLapRace);
        std::string metric = board ? board->metric : (mode.find("Drift") != std::string::npos ? "score" : "best_lap");
        double target = 0.0;
        std::string targetLabel;
        if (board && board->metric != "wheelie")
        {
            if (board->best > 0.0) { target = board->best; targetLabel = board->place == 1 ? "PB \xC2\xB7 #1" : "PB"; }
            else if (board->beat > 0.0) { target = board->beat; targetLabel = "To beat"; }
            else if (board->record > 0.0) { target = board->record; targetLabel = "#1 Record"; }
        }
        // A lap in this race already beat it: the next lap chases that one.
        if (metric == "best_lap" && !oneLapRace && pace.bestLap > 0.0f && (target <= 0.0 || pace.bestLap < target))
        {
            target = pace.bestLap;
            targetLabel = "New PB";
        }

        double current = 0.0;
        const char* heading = "LIVE PACE";
        if (metric == "score") { current = pace.score; heading = "LIVE SCORE"; }
        else if (metric == "wheelie") { current = pace.raceElapsed; metric = "race_time"; heading = "LIVE RUN"; }
        else if (metric == "top_speed") { current = pace.topSpeedKmh; heading = "PEAK SPEED"; }
        else if (metric == "race_time" || oneLapRace) { current = pace.raceElapsed; heading = "LIVE RUN"; }
        else { current = pace.lapElapsed; heading = "CURRENT LAP"; }

        ImGui::Dummy(ImVec2(0.0f, 8.0f * s));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0.0f, 6.0f * s));

        ImGui::PushFont(g_fontMicro);
        if (metric == "best_lap" && pace.lapsCompleted >= 0)
            ImGui::TextColored(Text3, "%s \xC2\xB7 LAP %d", heading, pace.lapsCompleted + 1);
        else
            ImGui::TextColored(Text3, "%s", heading);
        ImGui::PopFont();

        const bool timed = metric == "best_lap" || metric == "race_time";
        const std::string currentStr = FormatValue(metric, current);
        ImVec4 barColour = Info;
        ImVec4 deltaColour = Text2;
        std::string deltaStr;
        float fill = 0.0f;

        if (target > 0.0)
        {
            const double delta = current - target;
            if (timed)
            {
                const float ratio = static_cast<float>(current / target);
                fill = ratio < 0.0f ? 0.0f : ratio > 1.0f ? 1.0f : ratio;
                if (delta <= 0.0)
                {
                    barColour = ratio >= 0.85f ? Warning : Success;
                    deltaColour = barColour;
                }
                else
                {
                    barColour = Danger;
                    deltaColour = Danger;
                }
            }
            else
            {
                const float ratio = static_cast<float>(current / target);
                fill = ratio < 0.0f ? 0.0f : ratio > 1.0f ? 1.0f : ratio;
                if (delta >= 0.0)
                {
                    barColour = Success;
                    deltaColour = Success;
                }
                else
                {
                    barColour = Info;
                    deltaColour = Text2;
                }
            }
            deltaStr = FormatSignedDelta(metric, delta);
        }

        ImGui::PushFont(g_fontMono);
        ImGui::TextColored(Text1, "%s", currentStr.c_str());
        if (!deltaStr.empty())
        {
            ImGui::SameLine(0.0f, 12.0f * s);
            ImGui::TextColored(deltaColour, "%s", deltaStr.c_str());
        }
        ImGui::PopFont();

        if (target > 0.0)
        {
            ImGui::Dummy(ImVec2(0.0f, 2.0f * s));
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const float width = ImGui::GetContentRegionAvail().x;
            const float height = 5.0f * s;
            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(at, ImVec2(at.x + width, at.y + height), ImGui::GetColorU32(Line), height);
            if (fill > 0.0f)
                draw->AddRectFilled(at, ImVec2(at.x + width * fill, at.y + height), ImGui::GetColorU32(barColour), height);
            ImGui::Dummy(ImVec2(width, height));

            ImGui::PushFont(g_fontSmall);
            ImGui::TextColored(Text3, "vs %s %s", targetLabel.c_str(), FormatValue(metric, target).c_str());
            if (metric == "best_lap" && pace.bestLap > 0.0f)
            {
                ImGui::SameLine(0.0f, 12.0f * s);
                ImGui::TextColored(Text2, "Session %s", FormatValue("best_lap", pace.bestLap).c_str());
            }
            ImGui::PopFont();
        }

        if (pace.lastSplitLap > 0 && pace.lastSplitTime > 0.0f && (metric == "best_lap" || metric == "race_time"))
        {
            ImGui::PushFont(g_fontSmall);
            if (target > 0.0 && metric == "best_lap")
            {
                const double splitDelta = pace.lastSplitTime - target;
                ImGui::TextColored(splitDelta <= 0.0 ? Success : Danger, "Last lap #%d: %s (%s)",
                                   pace.lastSplitLap, FormatValue("best_lap", pace.lastSplitTime).c_str(),
                                   FormatSignedDelta("best_lap", splitDelta).c_str());
            }
            else
            {
                ImGui::TextColored(Text2, "Last lap #%d: %s", pace.lastSplitLap, FormatValue("best_lap", pace.lastSplitTime).c_str());
            }
            ImGui::PopFont();
        }
    }

    static ImVec4 PingColour(int pingMs)
    {
        return pingMs < 60 ? Success : pingMs < 120 ? Warning : Danger;
    }

    static void DrawPingRight(int pingMs, float s)
    {
        if (pingMs < 0) return;
        char text[24];
        snprintf(text, sizeof(text), "%d ms", pingMs);
        ImGui::PushFont(g_fontMono);
        const float textW = ImGui::CalcTextSize(text).x;
        const float dotGap = 9.0f * s;
        const float totalW = dotGap + textW;
        ImGui::SameLine(0.0f, 0.0f);
        const float targetX = (std::max)(ImGui::GetCursorPosX() + 14.0f * s,
                                         (ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x) - totalW);
        ImGui::SetCursorPosX(targetX);
        const ImVec2 dotAt = ImGui::GetCursorScreenPos();
        const float lineH = ImGui::GetTextLineHeight();
        const ImVec4 col = PingColour(pingMs);
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(dotAt.x + 3.0f * s, dotAt.y + lineH * 0.5f), 2.5f * s, ImGui::GetColorU32(col));
        ImGui::SetCursorPosX(targetX + dotGap);
        ImGui::TextColored(col, "%s", text);
        ImGui::PopFont();
    }

    static void DrawAllowedCars(bool blockedWarning, float s, size_t maxShown)
    {
        const std::vector<std::string> allowed = CarLock::GetAllowedCarNames();
        ImGui::PushFont(g_fontSmall);
        if (allowed.empty())
        {
            ImGui::TextColored(Text3, "Cars: Any");
        }
        else
        {
            std::string list;
            const size_t limit = maxShown > 0 ? (std::min)(maxShown, allowed.size()) : allowed.size();
            for (size_t i = 0; i < limit; ++i)
            {
                if (!list.empty()) list += ", ";
                list += allowed[i];
            }
            if (allowed.size() > limit)
                list += ", +" + std::to_string(allowed.size() - limit) + " more";

            const float wrapAt = ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
            ImGui::PushTextWrapPos(wrapAt);
            ImGui::TextColored(blockedWarning ? Warning : Info, "Allowed (%u):", static_cast<unsigned>(allowed.size()));
            ImGui::SameLine(0.0f, 6.0f * s);
            ImGui::TextColored(blockedWarning ? Warning : Text2, "%s", list.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::PopFont();
    }

    static void DrawStanding(const std::string& trackName, const std::string& mode, const std::vector<Board>& boards,
                             bool paceBarShown, const RaceResults::LivePace& pace, int pingMs, float s)
    {
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        const float margin = 24.0f * s;
        const bool right = g_corner == TopRight || g_corner == BottomRight;
        const bool bottom = g_corner == BottomLeft || g_corner == BottomRight;
        ImGui::SetNextWindowPos(ImVec2(right ? display.x - margin : margin, bottom ? display.y - margin : margin), ImGuiCond_Always,
                                ImVec2(right ? 1.0f : 0.0f, bottom ? 1.0f : 0.0f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(310.0f * s, 0.0f), ImVec2(430.0f * s, display.y));
        if (!ImGui::Begin("##standing", nullptr, PanelFlags)) { ImGui::End(); return; }

        ImGui::PushFont(g_fontBody);
        ImGui::TextColored(Text1, "%s", trackName.c_str());
        ImGui::PopFont();
        DrawPingRight(pingMs, s);
        ImGui::PushFont(g_fontSmall);
        // One leaderboard for both; this only says what the player's own cars have.
        const std::string speed = std::string(VaultFixes::SpeedLimit()) == "limited" ? "your cars: speed limiter on" : "your cars: no speed limiter";
        ImGui::TextColored(Text2, "%s \xC2\xB7 %s", mode.c_str(), speed.c_str());
        ImGui::PopFont();

        if (boards.empty())
        {
            ImGui::Dummy(ImVec2(0.0f, 6.0f * s));
            ImGui::PushFont(g_fontSmall);
            ImGui::TextColored(Text3, "No times here yet. Set the first one.");
            ImGui::PopFont();
        }
        for (const Board& b : boards)
        {
            ImGui::Dummy(ImVec2(0.0f, 8.0f * s));
            ImGui::PushFont(g_fontMicro);
            ImGui::TextColored(Text3, "%s", BoardLabel(b).c_str());
            ImGui::PopFont();
            if (!ImGui::BeginTable(("##" + b.metric).c_str(), 3, ImGuiTableFlags_SizingFixedFit)) continue;
            ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, 72.0f * s);
            ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthFixed, 104.0f * s);
            ImGui::TableSetupColumn("note", ImGuiTableColumnFlags_WidthStretch);
            Row("Record", FormatValue(b.metric, b.record), Text1, b.recordHolder, Text3);
            if (b.best > 0.0)
            {
                char place[48];
                snprintf(place, sizeof(place), "#%d of %d", b.place, b.players);
                Row("You", FormatValue(b.metric, b.best), PlaceColour(b.place), place, PlaceColour(b.place));
                if (b.beat > 0.0) Row("To beat", FormatValue(b.metric, b.beat), Text1, b.beatHolder, Text3);
                else Row("To beat", "-", Text3, "You hold the record", Gold);
            }
            else
            {
                Row("You", "-", Text3, "No time yet", Text3);
            }
            ImGui::EndTable();
        }
        if (paceBarShown) DrawPaceBar(mode, boards, pace, s);
        ImGui::End();
    }

    static std::string FormatAtmosphere(const std::string& atm)
    {
        if (atm.empty()) return std::string();
        std::string out = atm;
        if (out[0] >= 'a' && out[0] <= 'z') out[0] = static_cast<char>(out[0] - ('a' - 'A'));
        return out;
    }

    static ImVec4 StatusColour(const std::string& status)
    {
        if (status == "ready") return Success;
        if (status == "picking") return Warning;
        if (status == "blocked") return Danger;
        if (status == "racing") return Info;
        return Text2;
    }

    static const char* StatusLabel(const std::string& status)
    {
        if (status == "ready") return "Ready";
        if (status == "menus") return "In menus";
        if (status == "picking") return "Picking";
        if (status == "blocked") return "Blocked";
        if (status == "racing") return "Racing";
        return "";
    }

    static void DrawCardTitle(const char* title, const ImVec4& dot, float s);

    // Who is in the room, race day context, live timer state, and each player's status.
    static void DrawLobby(const std::string& room, const std::string& atmosphere, int eventIndex, int eventCount,
                          const std::vector<LobbyPlayer>& players, const LobbyTimerState& timer,
                          const EventVote::View& vote, int pingMs, float s)
    {
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        const float margin = 24.0f * s;
        const bool right = g_corner == TopRight || g_corner == BottomRight;
        const bool bottom = g_corner == BottomLeft || g_corner == BottomRight;
        ImGui::SetNextWindowPos(ImVec2(right ? display.x - margin : margin, bottom ? display.y - margin : margin), ImGuiCond_Always,
                                ImVec2(right ? 1.0f : 0.0f, bottom ? 1.0f : 0.0f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(360.0f * s, 0.0f), ImVec2(510.0f * s, display.y));
        if (!ImGui::Begin("##lobby", nullptr, PanelFlags)) { ImGui::End(); return; }

        bool anyBlocked = false;
        for (const auto& p : players) if (p.status == "blocked") anyBlocked = true;
        const ImVec4 dot = timer.paused || anyBlocked ? Warning
                         : (timer.ready > 0 && timer.ready == timer.total) ? Success : Info;
        DrawCardTitle(room.empty() ? "In this room" : room.c_str(), dot, s);
        DrawPingRight(pingMs, s);

        const int totalEvents = eventCount > 0 ? eventCount : static_cast<int>(vote.events.size());
        const int activeEvent = eventCount > 0 ? eventIndex : vote.current;
        const std::string atm = FormatAtmosphere(atmosphere);
        std::string context;
        if (totalEvents > 0 && activeEvent >= 0) context = "Event " + std::to_string(activeEvent + 1) + "/" + std::to_string(totalEvents);
        if (!atm.empty()) context += (context.empty() ? "" : " \xC2\xB7 ") + atm;
        if (!context.empty())
        {
            ImGui::PushFont(g_fontSmall);
            ImGui::TextColored(Text2, "%s", context.c_str());
            if (activeEvent >= 0 && static_cast<size_t>(activeEvent) < vote.events.size())
            {
                ImGui::TextColored(Text1, "%s", vote.events[static_cast<size_t>(activeEvent)].c_str());
            }
            ImGui::PopFont();
        }
        DrawAllowedCars(anyBlocked, s, 6);

        ImGui::Dummy(ImVec2(0.0f, 3.0f * s));
        ImGui::PushFont(g_fontSmall);
        const int readyCount = timer.total > 0 ? timer.ready : [&]() {
            int r = 0; for (const auto& p : players) if (p.status == "ready") ++r; return r;
        }();
        const int totalCount = timer.total > 0 ? timer.total : static_cast<int>(players.size());

        if (timer.seconds >= 0)
        {
            const std::string clk = FormatClock(timer.seconds);
            if (timer.paused)
            {
                std::string label = timer.reason == "vote" ? "Paused \xC2\xB7 Event vote"
                                  : (timer.reason == "car" && !timer.who.empty()) ? "Waiting for " + timer.who + " to pick a car"
                                  : timer.reason == "car" ? "Paused \xC2\xB7 Picking car"
                                  : "Paused";
                ImGui::TextColored(Warning, "%s", label.c_str());
                ImGui::SameLine(0.0f, 8.0f * s);
                ImGui::PushFont(g_fontMono);
                ImGui::TextColored(Warning, "%s", clk.c_str());
                ImGui::PopFont();
            }
            else if (timer.reason == "ready" || (timer.seconds <= 10 && readyCount == totalCount && totalCount > 0))
            {
                ImGui::TextColored(Success, "Everyone ready \xC2\xB7 starting in");
                ImGui::SameLine(0.0f, 6.0f * s);
                ImGui::PushFont(g_fontMono);
                ImGui::TextColored(Success, "%s", clk.c_str());
                ImGui::PopFont();
            }
            else
            {
                ImGui::TextColored(Text2, "Starting in");
                ImGui::SameLine(0.0f, 6.0f * s);
                ImGui::PushFont(g_fontMono);
                ImGui::TextColored(Text1, "%s", clk.c_str());
                ImGui::PopFont();
                ImGui::SameLine(0.0f, 8.0f * s);
                if (timer.allReadyIn > 0)
                    ImGui::TextColored(Success, "\xC2\xB7 skip in %ds", timer.allReadyIn);
                else
                    ImGui::TextColored(Text3, "\xC2\xB7 %d/%d ready", readyCount, totalCount);
            }
        }
        else if (totalCount > 0)
        {
            ImGui::TextColored(Text3, "Waiting for players \xC2\xB7 %d/%d ready", readyCount, totalCount);
        }
        ImGui::PopFont();

        ImGui::Dummy(ImVec2(0.0f, 4.0f * s));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0.0f, 4.0f * s));

        if (ImGui::BeginTable("##lobbycars", 4, ImGuiTableFlags_SizingFixedFit))
        {
            ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, 110.0f * s);
            ImGui::TableSetupColumn("car", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("state", ImGuiTableColumnFlags_WidthFixed, 68.0f * s);
            ImGui::TableSetupColumn("ping", ImGuiTableColumnFlags_WidthFixed, 54.0f * s);
            ImGui::PushFont(g_fontSmall);
            for (const auto& entry : players)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(Text1, "%s", entry.name.c_str());
                ImGui::TableSetColumnIndex(1);
                if (entry.car.empty()) ImGui::TextColored(Text3, "Picking a car");
                else ImGui::TextColored(Text2, "%s", entry.car.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextColored(StatusColour(entry.status), "%s", StatusLabel(entry.status));
                ImGui::TableSetColumnIndex(3);
                const int pMs = entry.pingMs >= 0 ? entry.pingMs : ServerLink::GetPlayerPingMs(entry.name);
                if (pMs >= 0)
                {
                    ImGui::PushFont(g_fontMono);
                    ImGui::TextColored(PingColour(pMs), "%d ms", pMs);
                    ImGui::PopFont();
                }
                else
                {
                    ImGui::TextColored(Text3, "-");
                }
            }
            ImGui::PopFont();
            ImGui::EndTable();
        }
        ImGui::End();
    }

    static ImVec4 NoticeColour(const std::string& kind)
    {
        return kind == "record" ? Gold : kind == "best" ? Success : kind == "not_counted" ? Warning : kind == "refused" ? Danger : Text2;
    }

    // ---- The level bar ----
    //
    // Everything about it follows from the notice's age, so it needs no state of its own: the bar
    // waits a moment, then fills from the XP the player had to the XP they have now, easing out, and
    // passes through every level on the way. The popup opens when the bar passes into a new level.

    static const double XP_FILL_DELAY = 0.8;    // after the notice appears
    static const double XP_FILL_SECONDS = 1.8;  // for the first level, plus XP_LEVEL_SECONDS per level passed
    static const double XP_LEVEL_SECONDS = 0.5;
    static const double LEVEL_UP_SECONDS = 4.5; // how long the popup stays

    static double XpFillSeconds(const XpGain& xp)
    {
        return XP_FILL_SECONDS + XP_LEVEL_SECONDS * static_cast<double>(xp.levelStarts.size() - 2);
    }

    static double XpFillEnd(const XpGain& xp) { return XP_FILL_DELAY + XpFillSeconds(xp); }

    static double EaseOut(double p) { return 1.0 - (1.0 - p) * (1.0 - p) * (1.0 - p); }

    // The XP the bar shows at this age of the notice.
    static double XpShownAt(const XpGain& xp, double age)
    {
        double p = (age - XP_FILL_DELAY) / XpFillSeconds(xp);
        if (p < 0.0) p = 0.0;
        if (p > 1.0) p = 1.0;
        return static_cast<double>(xp.total - xp.gained) + xp.gained * EaseOut(p);
    }

    // Which of levelStarts the shown XP is in: 0 for the level the player had before the race.
    static size_t XpLevelIndex(const XpGain& xp, double shown)
    {
        size_t i = 0;
        while (i + 2 < xp.levelStarts.size() && shown >= static_cast<double>(xp.levelStarts[i + 1])) ++i;
        return i;
    }

    // The age at which the bar passes into the first new level, or -1 when the race earned none.
    static double LevelUpAge(const XpGain& xp)
    {
        if (xp.gained <= 0 || xp.levelStarts.size() <= 2) return -1.0;
        double share = static_cast<double>(xp.levelStarts[1] - (xp.total - xp.gained)) / xp.gained;
        if (share < 0.0) share = 0.0;
        if (share > 1.0) share = 1.0;
        return XP_FILL_DELAY + (1.0 - std::cbrt(1.0 - share)) * XpFillSeconds(xp);
    }

    // The racing teams, one per race type, in the colours the stats site gives them.
    struct TeamStyle
    {
        const char* family;
        const char* label; // "Drift", for "+180 Drift XP"
        const char* team;
        const char* king;
        ImVec4      colour;
    };

    static const TeamStyle* TeamStyleOf(const std::string& family)
    {
        static const TeamStyle styles[] = {
            {"grip", "Grip", "G-Effect", "Grip King", Info},
            {"drift", "Drift", "Noise Bomb", "Drift King", Success},
            {"drag", "Drag", "Rogue Speed", "Drag King", ImVec4(0.831f, 0.831f, 0.831f, 1.0f)},
            {"speed", "Speed", "Nitrocide", "Speed King", ImVec4(0.973f, 0.443f, 0.443f, 1.0f)},
        };
        for (const TeamStyle& style : styles)
            if (family == style.family) return &style;
        return nullptr;
    }

    static std::string FormatXp(long long value)
    {
        std::string digits = std::to_string(value < 0 ? -value : value);
        for (int at = static_cast<int>(digits.size()) - 3; at > 0; at -= 3) digits.insert(static_cast<size_t>(at), ",");
        return value < 0 ? "-" + digits : digits;
    }

    // Under a notice: the level, the XP counting up, and the bar filling.
    static void DrawXpBar(const XpGain& xp, double age, float alpha, float s)
    {
        const double shown = XpShownAt(xp, age);
        const size_t index = XpLevelIndex(xp, shown);
        const int level = xp.levelBefore + static_cast<int>(index);
        const double start = static_cast<double>(xp.levelStarts[index]);
        const double next = static_cast<double>(xp.levelStarts[index + 1]);
        float fill = next > start ? static_cast<float>((shown - start) / (next - start)) : 1.0f;
        if (fill < 0.0f) fill = 0.0f;
        if (fill > 1.0f) fill = 1.0f;
        const TeamStyle* team = TeamStyleOf(xp.family);
        const bool levelledUp = index > 0;
        const ImVec4 barColour = levelledUp ? Gold : (team ? team->colour : Info);
        const auto faded = [alpha](const ImVec4& c) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alpha)); };

        ImGui::Dummy(ImVec2(0.0f, 6.0f * s));
        ImGui::PushFont(g_fontSmall);
        const float left = ImGui::GetCursorPosX() + 14.0f * s;
        ImGui::SetCursorPosX(left);
        ImGui::TextColored(levelledUp ? Gold : Text1, "Level %d", level);
        const std::string gained = "+" + FormatXp(static_cast<long long>(shown - (xp.total - xp.gained) + 0.5)) + " XP";
        const float gainedWidth = ImGui::CalcTextSize(gained.c_str()).x;
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX() + 12.0f * s, (ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x) - gainedWidth));
        ImGui::TextColored(Success, "%s", gained.c_str());

        ImGui::SetCursorPosX(left);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float width = (std::max)(160.0f * s, (ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x) - left);
        const float height = 6.0f * s;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(at, ImVec2(at.x + width, at.y + height), faded(Line), height);
        if (fill > 0.0f) draw->AddRectFilled(at, ImVec2(at.x + width * fill, at.y + height), faded(barColour), height);
        // A bright edge on the moving end while it fills.
        if (fill > 0.02f && fill < 1.0f && age < XpFillEnd(xp))
            draw->AddCircleFilled(ImVec2(at.x + width * fill, at.y + height * 0.5f), height * 0.75f, faded(Text1));
        ImGui::Dummy(ImVec2(width, height));

        ImGui::SetCursorPosX(left);
        if (next > start)
            ImGui::TextColored(Text3, "%s / %s XP   %s to level %d", FormatXp(static_cast<long long>(shown + 0.5)).c_str(),
                               FormatXp(static_cast<long long>(next)).c_str(), FormatXp(static_cast<long long>(next - shown + 0.5)).c_str(), level + 1);
        else
            ImGui::TextColored(Text3, "%s XP   top level", FormatXp(static_cast<long long>(shown + 0.5)).c_str());

        // The race type's pool: the same XP, counting up with the bar, and the place in it on the server.
        if (team)
        {
            const double earned = shown - static_cast<double>(xp.total - xp.gained);
            const long long poolShown = xp.pool - xp.gained + static_cast<long long>(earned + 0.5);
            ImGui::Dummy(ImVec2(0.0f, 4.0f * s));
            ImGui::SetCursorPosX(left);
            const ImVec2 dotAt = ImGui::GetCursorScreenPos();
            const float lineHeight = ImGui::GetTextLineHeight();
            draw->AddCircleFilled(ImVec2(dotAt.x + 3.0f * s, dotAt.y + lineHeight * 0.5f), 3.0f * s, faded(team->colour));
            ImGui::SetCursorPosX(left + 12.0f * s);
            ImGui::TextColored(team->colour, "+%s %s XP", FormatXp(static_cast<long long>(earned + 0.5)).c_str(), team->label);
            ImGui::SameLine(0.0f, 10.0f * s);
            if (xp.poolRank > 0)
                ImGui::TextColored(Text3, "%s   #%d on the server", FormatXp(poolShown).c_str(), xp.poolRank);
            else
                ImGui::TextColored(Text3, "%s", FormatXp(poolShown).c_str());

            // Once the bar is full: a new team, or a new crown.
            if (age >= XpFillEnd(xp))
            {
                const TeamStyle* joined = xp.joinedTeam ? TeamStyleOf(xp.team) : nullptr;
                if (joined)
                {
                    ImGui::SetCursorPosX(left + 12.0f * s);
                    ImGui::TextColored(joined->colour, "You joined %s", joined->team);
                }
                if (xp.crowned)
                {
                    ImGui::SetCursorPosX(left + 12.0f * s);
                    ImGui::TextColored(Gold, "You're the new %s", team->king);
                }
            }
        }
        ImGui::PopFont();
    }

    // A level up: a card in the middle of the screen, from when the bar passes into the new level.
    static void DrawLevelUp(const XpGain& xp, double age, float s)
    {
        const double since = age - LevelUpAge(xp);
        if (since < 0.0 || since > LEVEL_UP_SECONDS) return;
        float alpha = 1.0f;
        if (since < 0.2) alpha = static_cast<float>(since / 0.2);
        else if (since > LEVEL_UP_SECONDS - 0.5) alpha = static_cast<float>((LEVEL_UP_SECONDS - since) / 0.5);
        // It counts on with the bar when the race passed more than one level.
        const int level = xp.levelBefore + static_cast<int>(XpLevelIndex(xp, XpShownAt(xp, age)));
        const auto faded = [alpha](const ImVec4& c, float a = 1.0f) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alpha * a)); };

        const ImVec2 display = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.30f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowBgAlpha(Surface.w * alpha);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(Gold.x, Gold.y, Gold.z, 0.6f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(280.0f * s, 0.0f), ImVec2(420.0f * s, display.y));
        if (ImGui::Begin("##levelup", nullptr, PanelFlags))
        {
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const float width = ImGui::GetContentRegionAvail().x;

            ImGui::PushFont(g_fontSmall);
            const char* heading = "LEVEL UP";
            const float headingWidth = ImGui::CalcTextSize(heading).x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (width - headingWidth) * 0.5f);
            ImGui::TextColored(Gold, "%s", heading);
            ImGui::PopFont();

            // The new level, large, with a ring that spreads out from it as it arrives.
            ImGui::PushFont(g_fontBody);
            const float size = ImGui::GetFontSize() * 2.8f;
            const std::string number = std::to_string(level);
            const ImVec2 numberSize = g_fontBody->CalcTextSizeA(size, FLT_MAX, 0.0f, number.c_str());
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const ImVec2 centre(at.x + width * 0.5f, at.y + numberSize.y * 0.5f);
            const float ring = static_cast<float>(since < 0.8 ? since / 0.8 : 1.0);
            if (ring < 1.0f)
                draw->AddCircle(centre, numberSize.y * (0.5f + ring * 0.9f), faded(Gold, 1.0f - ring), 48, 2.0f * s);
            draw->AddCircleFilled(centre, numberSize.y * 0.62f, faded(Gold, 0.12f), 48);
            draw->AddText(g_fontBody, size, ImVec2(centre.x - numberSize.x * 0.5f, at.y), faded(Text1), number.c_str());
            ImGui::Dummy(ImVec2(width, numberSize.y));
            ImGui::PopFont();

            ImGui::PushFont(g_fontSmall);
            const std::string line = "You reached level " + number;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (width - ImGui::CalcTextSize(line.c_str()).x) * 0.5f);
            ImGui::TextColored(Text2, "%s", line.c_str());
            ImGui::PopFont();
        }
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }

    // Short notifications, top centre, newest at the top. Each fades in, stays, and fades out.
    static void DrawNotices(std::vector<Notice>& notices, double now, float s)
    {
        const double fade = 0.25;
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        float y = 24.0f * s;
        int index = 0;
        for (Notice& n : notices)
        {
            const double life = n.duration;
            const double age = now - n.shownAt;
            float alpha = 1.0f;
            if (age < fade) alpha = static_cast<float>(age / fade);
            else if (age > life - fade) alpha = static_cast<float>((life - age) / fade);
            if (alpha < 0.0f) alpha = 0.0f;
            ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, y), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
            ImGui::SetNextWindowBgAlpha(Surface.w * alpha);
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
            ImGui::SetNextWindowSizeConstraints(ImVec2(280.0f * s, 0.0f), ImVec2(520.0f * s, display.y));
            const std::string id = "##notice" + std::to_string(index++);
            if (ImGui::Begin(id.c_str(), nullptr, PanelFlags))
            {
                // A 6px status dot next to the title, as on the website.
                const ImVec2 at = ImGui::GetCursorScreenPos();
                ImGui::PushFont(g_fontBody);
                const float lineHeight = ImGui::GetTextLineHeight();
                const ImVec4 dot = NoticeColour(n.kind);
                ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(at.x + 3.0f * s, at.y + lineHeight * 0.5f), 3.5f * s,
                                                            ImGui::GetColorU32(ImVec4(dot.x, dot.y, dot.z, dot.w * alpha)));
                ImGui::SetCursorScreenPos(ImVec2(at.x + 14.0f * s, at.y));
                ImGui::TextColored(Text1, "%s", n.title.c_str());
                ImGui::PopFont();
                ImGui::PushFont(g_fontSmall);
                for (const std::string& line : n.lines)
                {
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 14.0f * s);
                    ImGui::TextColored(Text2, "%s", line.c_str());
                }
                ImGui::PopFont();
                if (n.xp.gained > 0) DrawXpBar(n.xp, age, alpha, s);
                y += ImGui::GetWindowHeight() + 8.0f * s;
            }
            ImGui::End();
            ImGui::PopStyleVar();
        }
        // One popup at a time: the newest level up.
        for (const Notice& n : notices)
        {
            if (n.xp.gained <= 0 || LevelUpAge(n.xp) < 0.0) continue;
            const double age = now - n.shownAt;
            if (age >= LevelUpAge(n.xp) && age <= LevelUpAge(n.xp) + LEVEL_UP_SECONDS) { DrawLevelUp(n.xp, age, s); break; }
        }
    }

    static void DrawCardTitle(const char* title, const ImVec4& dot, float s);
    static void DrawKeyHint(const char* key, const char* action, float s);

    // The event vote window, in the middle: the race day's events, the next one marked. It takes the
    // keyboard and mouse while it's open.
    static void DrawVoteWindow(const EventVote::View& vote, float s)
    {
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(420.0f * s, 0.0f), ImVec2(620.0f * s, display.y));
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoNav;
        if (!ImGui::Begin("##vote", nullptr, flags)) { ImGui::End(); return; }
        ImDrawList* draw = ImGui::GetWindowDrawList();
        DrawCardTitle("Call a vote", Text2, s);
        ImGui::PushFont(g_fontSmall);
        ImGui::TextColored(Text2, "Pick the event to race next. More than half the room has to agree.");
        ImGui::Dummy(ImVec2(0.0f, 8.0f * s));

        // The events: rounded rows, the picked one lit, the next one tagged.
        const float rowHeight = 30.0f * s, width = ImGui::GetContentRegionAvail().x;
        for (int i = 0; i < static_cast<int>(vote.events.size()); ++i)
        {
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("##event", ImVec2(width, rowHeight))) EventVote::Select(i);
            const bool hovered = ImGui::IsItemHovered();
            if (hovered && ImGui::IsMouseDoubleClicked(0)) EventVote::CallSelected();
            ImGui::PopID();
            const bool picked = i == vote.selected;
            if (picked || hovered)
                draw->AddRectFilled(at, ImVec2(at.x + width, at.y + rowHeight), ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, picked ? 0.08f : 0.04f)),
                                    8.0f * s);
            if (picked) draw->AddRect(at, ImVec2(at.x + width, at.y + rowHeight), ImGui::GetColorU32(Line), 8.0f * s);
            const float textY = at.y + (rowHeight - ImGui::GetTextLineHeight()) * 0.5f;
            const std::string number = std::to_string(i + 1);
            draw->AddText(ImVec2(at.x + 12.0f * s, textY), ImGui::GetColorU32(Text3), number.c_str());
            draw->AddText(ImVec2(at.x + 34.0f * s, textY), ImGui::GetColorU32(picked ? Text1 : Text2), vote.events[i].c_str());
            if (i == vote.current)
            {
                const char* tag = "NEXT";
                const ImVec2 size = ImGui::CalcTextSize(tag);
                draw->AddText(ImVec2(at.x + width - size.x - 12.0f * s, textY), ImGui::GetColorU32(Text3), tag);
            }
        }
        ImGui::Dummy(ImVec2(0.0f, 10.0f * s));

        // Call vote (white, like the website's primary button) and Close (quiet).
        const bool canCall = vote.selected != vote.current && !vote.voteActive;
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f * s);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14.0f * s, 7.0f * s));
        ImGui::PushStyleColor(ImGuiCol_Button, Text1);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Text2);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.039f, 0.039f, 0.039f, 1.0f));
        if (!canCall) ImGui::BeginDisabled();
        if (ImGui::Button("Call vote")) EventVote::CallSelected();
        if (!canCall) ImGui::EndDisabled();
        ImGui::PopStyleColor(4);
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.06f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 1.0f, 1.0f, 0.10f));
        ImGui::PushStyleColor(ImGuiCol_Text, Text2);
        ImGui::PushStyleColor(ImGuiCol_Border, Line);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        if (ImGui::Button("Close")) EventVote::Close();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(5);

        // The keys, as small chips.
        ImGui::Dummy(ImVec2(0.0f, 6.0f * s));
        DrawKeyHint("Up/Down", "pick", s);
        ImGui::SameLine(0.0f, 14.0f * s);
        DrawKeyHint("Enter", "call", s);
        ImGui::SameLine(0.0f, 14.0f * s);
        DrawKeyHint((vote.yesKey + "/Esc").c_str(), "close", s);
        ImGui::PopFont();
        ImGui::End();
    }

    // A running vote, top centre under the notices' place: what it's for, the keys, and the count.
    static void DrawVotePopup(const EventVote::View& vote, float s)
    {
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.22f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(360.0f * s, 0.0f), ImVec2(560.0f * s, display.y));
        if (!ImGui::Begin("##votepopup", nullptr, PanelFlags)) { ImGui::End(); return; }
        DrawCardTitle("Vote for the next event", Warning, s);
        ImGui::PushFont(g_fontSmall);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 14.0f * s);
        ImGui::TextColored(Text1, "%s", vote.eventName.c_str());
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 14.0f * s);
        ImGui::TextColored(Text3, "Called by %s", vote.caller.c_str());
        ImGui::Dummy(ImVec2(0.0f, 6.0f * s));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 14.0f * s);
        if (vote.myVote < 0)
        {
            DrawKeyHint(vote.yesKey.c_str(), "yes", s);
            ImGui::SameLine(0.0f, 16.0f * s);
            DrawKeyHint(vote.noKey.c_str(), "no", s);
        }
        else
        {
            ImGui::TextColored(vote.myVote ? Success : Text2, "You voted %s", vote.myVote ? "yes" : "no");
        }
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 14.0f * s);
        ImGui::TextColored(Text3, "%d yes  %d no  of %d   %d s", vote.yes, vote.no, vote.total, static_cast<int>(vote.secondsLeft + 0.999));

        // The time left, as a thin bar.
        ImGui::Dummy(ImVec2(0.0f, 4.0f * s));
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x, height = 3.0f * s;
        float share = static_cast<float>(vote.secondsLeft / 20.0);
        if (share < 0.0f) share = 0.0f;
        if (share > 1.0f) share = 1.0f;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(at, ImVec2(at.x + width, at.y + height), ImGui::GetColorU32(Line), height);
        draw->AddRectFilled(at, ImVec2(at.x + width * share, at.y + height), ImGui::GetColorU32(Warning), height);
        ImGui::Dummy(ImVec2(width, height));
        ImGui::PopFont();
        ImGui::End();
    }

    // A card's title with the 6px status dot in front, as the notices have.
    // A small pill at the bottom centre: which way the ReShifted gearbox is shifting right now.
    static void DrawShiftIndicator(const ReShifted::Indicator& shift, float s)
    {
        const ImGuiIO& io = ImGui::GetIO();
        const ImVec4& colour = shift.sport ? Warning : Success;
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y - 18.0f * s), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f * s, 5.0f * s));
        if (ImGui::Begin("##shift", nullptr, PanelFlags))
        {
            ImGui::PushFont(g_fontMicro);
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const float lineHeight = ImGui::GetTextLineHeight();
            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->AddCircleFilled(ImVec2(at.x + 3.0f * s, at.y + lineHeight * 0.5f), 3.0f * s, ImGui::GetColorU32(colour));
            ImGui::SetCursorScreenPos(ImVec2(at.x + 12.0f * s, at.y));
            ImGui::TextColored(colour, "%s", shift.sport ? "SPORT" : "ECO");
            // A thin bar under the label: how far between eco and sport the shift point sits.
            const ImVec2 end = ImGui::GetCursorScreenPos();
            const float width = ImGui::GetWindowContentRegionMax().x - ImGui::GetWindowContentRegionMin().x;
            const float barWidth = width > 44.0f * s ? width : 44.0f * s;
            draw->AddRectFilled(ImVec2(at.x, end.y), ImVec2(at.x + barWidth, end.y + 2.0f * s), ImGui::GetColorU32(Line), 1.0f * s);
            draw->AddRectFilled(ImVec2(at.x, end.y), ImVec2(at.x + barWidth * shift.level, end.y + 2.0f * s), ImGui::GetColorU32(colour), 1.0f * s);
            ImGui::Dummy(ImVec2(barWidth, 3.0f * s));
            ImGui::PopFont();
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }

    static void DrawCardTitle(const char* title, const ImVec4& dot, float s)
    {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::PushFont(g_fontBody);
        const float lineHeight = ImGui::GetTextLineHeight();
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(at.x + 3.0f * s, at.y + lineHeight * 0.5f), 3.5f * s, ImGui::GetColorU32(dot));
        ImGui::SetCursorScreenPos(ImVec2(at.x + 14.0f * s, at.y));
        ImGui::TextColored(Text1, "%s", title);
        ImGui::PopFont();
    }

    // A key as a small outlined chip, then what it does: [F1] yes.
    static void DrawKeyHint(const char* key, const char* action, float s)
    {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const ImVec2 size = ImGui::CalcTextSize(key);
        const ImVec2 pad(6.0f * s, 2.0f * s);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(at, ImVec2(at.x + size.x + pad.x * 2.0f, at.y + size.y + pad.y * 2.0f), ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.06f)),
                            5.0f * s);
        draw->AddRect(at, ImVec2(at.x + size.x + pad.x * 2.0f, at.y + size.y + pad.y * 2.0f), ImGui::GetColorU32(Line), 5.0f * s);
        draw->AddText(ImVec2(at.x + pad.x, at.y + pad.y), ImGui::GetColorU32(Text1), key);
        ImGui::Dummy(ImVec2(size.x + pad.x * 2.0f, size.y + pad.y * 2.0f));
        ImGui::SameLine(0.0f, 6.0f * s);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + pad.y);
        ImGui::TextColored(Text2, "%s", action);
    }

    // True while one of this process's windows is in front.
    static bool GameInFront()
    {
        DWORD process = 0;
        HWND front = GetForegroundWindow();
        if (front) GetWindowThreadProcessId(front, &process);
        return process == GetCurrentProcessId();
    }

    static int ParseOverlayKey(const char* text, int fallback, std::string* outName)
    {
        if (!text || !text[0]) return fallback;
        if (_stricmp(text, "Tab") == 0) { if (outName) *outName = "TAB"; return VK_TAB; }
        if (_stricmp(text, "Insert") == 0) { if (outName) *outName = "INS"; return VK_INSERT; }
        if (_stricmp(text, "Home") == 0) { if (outName) *outName = "HOME"; return VK_HOME; }
        if (_stricmp(text, "End") == 0) { if (outName) *outName = "END"; return VK_END; }
        if (_stricmp(text, "Delete") == 0) { if (outName) *outName = "DEL"; return VK_DELETE; }
        if (_stricmp(text, "PageUp") == 0) { if (outName) *outName = "PGUP"; return VK_PRIOR; }
        if (_stricmp(text, "PageDown") == 0) { if (outName) *outName = "PGDN"; return VK_NEXT; }
        if (_stricmp(text, "Space") == 0) { if (outName) *outName = "SPACE"; return VK_SPACE; }
        if ((text[0] == 'F' || text[0] == 'f') && text[1])
        {
            const int number = atoi(text + 1);
            if (number >= 1 && number <= 12)
            {
                if (outName) *outName = "F" + std::to_string(number);
                return VK_F1 + number - 1;
            }
        }
        if (text[1] == '\0')
        {
            const char ch = text[0];
            if (ch >= 'a' && ch <= 'z')
            {
                const char upper = static_cast<char>(ch - ('a' - 'A'));
                if (outName) *outName = std::string(1, upper);
                return static_cast<int>(upper);
            }
            if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))
            {
                if (outName) *outName = std::string(1, ch);
                return static_cast<int>(ch);
            }
        }
        return fallback;
    }

    static bool ToggleKeyPressed(int key, bool front)
    {
        static bool wasDown = false;
        const bool down = front && key > 0 && (GetAsyncKeyState(key) & 0x8000) != 0;
        const bool pressed = down && !wasDown;
        wasDown = down;
        return pressed;
    }

    static const LobbyPlayer* FindLobbyPlayer(const std::vector<LobbyPlayer>& players, const std::string& name)
    {
        for (const LobbyPlayer& p : players)
        {
            if (_stricmp(p.name.c_str(), name.c_str()) == 0) return &p;
        }
        return nullptr;
    }

    static void DrawPingCell(int pMs)
    {
        if (pMs >= 0)
        {
            ImGui::PushFont(g_fontMono);
            ImGui::TextColored(PingColour(pMs), "%d ms", pMs);
            ImGui::PopFont();
        }
        else
        {
            ImGui::TextColored(Text3, "-");
        }
    }

    // Rainbow Six Siege-style centered scoreboard shown while holding Tab (or ScoreboardKey).
    static void DrawScoreboard(const std::string& trackName, const std::string& mode, const std::vector<Board>& boards,
                               const std::string& room, const std::string& atmosphere, int eventIndex, int eventCount,
                               const std::vector<LobbyPlayer>& lobbyPlayers, const LobbyTimerState& timer,
                               const EventVote::View& vote, const std::vector<RaceResults::LiveRacer>& racers,
                               int pingMs, float s)
    {
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        const float maxW = (std::max)(360.0f * s, display.x - 48.0f * s);
        const float minW = (std::min)(maxW, 680.0f * s);
        const float capW = (std::min)(maxW, 920.0f * s);
        ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(minW, 0.0f), ImVec2(capW, display.y - 48.0f * s));
        ImGui::SetNextWindowBgAlpha(0.95f);
        ImGui::PushStyleColor(ImGuiCol_Border, Line);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f * s, 16.0f * s));
        if (!ImGui::Begin("##scoreboard", nullptr, PanelFlags))
        {
            ImGui::End();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
            return;
        }

        const bool inRace = !racers.empty();
        const ImVec4 titleDot = inRace ? Success : Info;
        std::string title = inRace ? trackName : (room.empty() ? "Lobby Scoreboard" : room);
        if (title.empty()) title = "OpenProStreet";
        DrawCardTitle(title.c_str(), titleDot, s);
        DrawPingRight(pingMs, s);

        const int totalEvents = eventCount > 0 ? eventCount : static_cast<int>(vote.events.size());
        const int activeEvent = eventCount > 0 ? eventIndex : vote.current;
        const std::string atm = FormatAtmosphere(atmosphere);

        ImGui::PushFont(g_fontSmall);
        std::string subtitle;
        if (inRace)
        {
            if (!mode.empty()) subtitle = mode;
            if (!room.empty()) subtitle += (subtitle.empty() ? "" : " \xC2\xB7 ") + room;
            if (totalEvents > 0 && activeEvent >= 0)
                subtitle += (subtitle.empty() ? "" : " \xC2\xB7 ") + std::string("Event ") + std::to_string(activeEvent + 1) + "/" + std::to_string(totalEvents);
            if (!atm.empty()) subtitle += (subtitle.empty() ? "" : " \xC2\xB7 ") + atm;
            const std::string speed = std::string(VaultFixes::SpeedLimit()) == "limited" ? "speed limiter on" : "no speed limiter";
            subtitle += (subtitle.empty() ? "" : " \xC2\xB7 ") + speed;
        }
        else
        {
            if (totalEvents > 0 && activeEvent >= 0)
                subtitle = "Event " + std::to_string(activeEvent + 1) + "/" + std::to_string(totalEvents);
            if (activeEvent >= 0 && static_cast<size_t>(activeEvent) < vote.events.size())
                subtitle += (subtitle.empty() ? "" : " \xC2\xB7 ") + vote.events[static_cast<size_t>(activeEvent)];
            if (!atm.empty()) subtitle += (subtitle.empty() ? "" : " \xC2\xB7 ") + atm;
            if (timer.seconds >= 0)
                subtitle += (subtitle.empty() ? "" : " \xC2\xB7 ") + std::string(timer.paused ? "Paused " : "Starting in ") + FormatClock(timer.seconds);
        }
        if (!subtitle.empty()) ImGui::TextColored(Text2, "%s", subtitle.c_str());
        ImGui::PopFont();

        ImGui::Dummy(ImVec2(0.0f, 8.0f * s));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0.0f, 8.0f * s));

        if (inRace)
        {
            const bool isDrift = mode.find("Drift") != std::string::npos;
            const bool isWheelie = mode.find("Wheelie") != std::string::npos;
            const bool isDrag = mode.find("Drag") != std::string::npos;
            const bool isSpeed = mode.find("Speed") != std::string::npos;
            const int cols = isDrift ? 7 : isWheelie ? 7 : isSpeed ? 7 : isDrag ? 8 : 8;

            const std::string localCar = g_baseModule ? TranslateCarIdentifier(ReadRawCarIdentifier(g_baseModule)) : std::string();

            if (ImGui::BeginTable("##scoreboard_race", cols, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX))
            {
                ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 32.0f * s);
                ImGui::TableSetupColumn("Driver", ImGuiTableColumnFlags_WidthFixed, 155.0f * s);
                ImGui::TableSetupColumn("Car", ImGuiTableColumnFlags_WidthStretch);
                if (isDrift)
                {
                    ImGui::TableSetupColumn("Score", ImGuiTableColumnFlags_WidthFixed, 108.0f * s);
                    ImGui::TableSetupColumn("Top Speed", ImGuiTableColumnFlags_WidthFixed, 96.0f * s);
                    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 78.0f * s);
                }
                else if (isWheelie)
                {
                    ImGui::TableSetupColumn("Wheelie", ImGuiTableColumnFlags_WidthFixed, 96.0f * s);
                    ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 96.0f * s);
                    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 78.0f * s);
                }
                else if (isSpeed)
                {
                    ImGui::TableSetupColumn("Top Speed", ImGuiTableColumnFlags_WidthFixed, 108.0f * s);
                    ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 96.0f * s);
                    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 78.0f * s);
                }
                else if (isDrag)
                {
                    ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 96.0f * s);
                    ImGui::TableSetupColumn("Best Run", ImGuiTableColumnFlags_WidthFixed, 96.0f * s);
                    ImGui::TableSetupColumn("Top Speed", ImGuiTableColumnFlags_WidthFixed, 92.0f * s);
                    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 74.0f * s);
                }
                else
                {
                    ImGui::TableSetupColumn("Laps", ImGuiTableColumnFlags_WidthFixed, 48.0f * s);
                    ImGui::TableSetupColumn("Best Lap", ImGuiTableColumnFlags_WidthFixed, 96.0f * s);
                    ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 96.0f * s);
                    ImGui::TableSetupColumn("Top Speed", ImGuiTableColumnFlags_WidthFixed, 92.0f * s);
                }
                ImGui::TableSetupColumn("Ping", ImGuiTableColumnFlags_WidthFixed, 62.0f * s);

                ImGui::PushFont(g_fontMicro);
                ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
                for (int c = 0; c < cols; ++c)
                {
                    ImGui::TableSetColumnIndex(c);
                    ImGui::TextColored(Text3, "%s", ImGui::TableGetColumnName(c));
                }
                ImGui::PopFont();

                const ImU32 selfBg = ImGui::GetColorU32(ImVec4(Info.x, Info.y, Info.z, 0.14f));
                const ImU32 altBg = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.025f));
                for (size_t i = 0; i < racers.size(); ++i)
                {
                    const RaceResults::LiveRacer& r = racers[i];
                    ImGui::TableNextRow();
                    if (r.isLocal) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, selfBg);
                    else if (i % 2 == 1) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, altBg);

                    ImGui::PushFont(g_fontSmall);
                    ImGui::TableSetColumnIndex(0);
                    if (r.rank > 0)
                    {
                        ImGui::PushFont(g_fontMono);
                        ImGui::TextColored(PlaceColour(r.rank), "%d", r.rank);
                        ImGui::PopFont();
                    }
                    else
                    {
                        ImGui::TextColored(Text3, "-");
                    }

                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextColored(r.isLocal ? Text1 : r.isAI ? Text3 : Text2, "%s", r.name.c_str());
                    if (r.isLocal)
                    {
                        ImGui::SameLine(0.0f, 6.0f * s);
                        ImGui::PushFont(g_fontMicro);
                        ImGui::TextColored(Info, "YOU");
                        ImGui::PopFont();
                    }
                    else if (r.isAI)
                    {
                        ImGui::SameLine(0.0f, 6.0f * s);
                        ImGui::PushFont(g_fontMicro);
                        ImGui::TextColored(Text3, "AI");
                        ImGui::PopFont();
                    }

                    const LobbyPlayer* lp = r.isAI ? nullptr : FindLobbyPlayer(lobbyPlayers, r.name);
                    std::string carName = lp ? lp->car : std::string();
                    if (carName.empty() && r.isLocal) carName = localCar;

                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextColored(carName.empty() ? Text3 : Text2, "%s", carName.empty() ? "-" : carName.c_str());

                    int colIdx = 3;
                    if (isDrift)
                    {
                        ImGui::TableSetColumnIndex(colIdx++);
                        ImGui::PushFont(g_fontMono);
                        ImGui::TextColored(r.score > 0.0f ? Text1 : Text3, "%s", r.score > 0.0f ? FormatValue("score", r.score).c_str() : "-");
                        ImGui::PopFont();

                        ImGui::TableSetColumnIndex(colIdx++);
                        ImGui::PushFont(g_fontMono);
                        ImGui::TextColored(r.topSpeedKmh > 0.0f ? Text2 : Text3, "%s", r.topSpeedKmh > 0.0f ? FormatValue("top_speed", r.topSpeedKmh).c_str() : "-");
                        ImGui::PopFont();

                        ImGui::TableSetColumnIndex(colIdx++);
                        ImGui::TextColored(r.finished ? Success : Info, "%s", r.finished ? "Finished" : "Racing");
                    }
                    else if (isWheelie)
                    {
                        ImGui::TableSetColumnIndex(colIdx++);
                        ImGui::PushFont(g_fontMono);
                        ImGui::TextColored(r.wheelieM > 0.0f ? Text1 : Text3, "%s", r.wheelieM > 0.0f ? FormatValue("wheelie", r.wheelieM).c_str() : "-");
                        ImGui::PopFont();

                        ImGui::TableSetColumnIndex(colIdx++);
                        ImGui::PushFont(g_fontMono);
                        ImGui::TextColored(r.raceElapsed > 0.0f ? Text2 : Text3, "%s", r.raceElapsed > 0.0f ? FormatValue("race_time", r.raceElapsed).c_str() : "-");
                        ImGui::PopFont();

                        ImGui::TableSetColumnIndex(colIdx++);
                        ImGui::TextColored(r.finished ? Success : Info, "%s", r.finished ? "Finished" : "Racing");
                    }
                    else if (isSpeed)
                    {
                        ImGui::TableSetColumnIndex(colIdx++);
                        ImGui::PushFont(g_fontMono);
                        ImGui::TextColored(r.topSpeedKmh > 0.0f ? Text1 : Text3, "%s", r.topSpeedKmh > 0.0f ? FormatValue("top_speed", r.topSpeedKmh).c_str() : "-");
                        ImGui::PopFont();

                        ImGui::TableSetColumnIndex(colIdx++);
                        ImGui::PushFont(g_fontMono);
                        ImGui::TextColored(r.raceElapsed > 0.0f ? Text2 : Text3, "%s", r.raceElapsed > 0.0f ? FormatValue("race_time", r.raceElapsed).c_str() : "-");
                        ImGui::PopFont();

                        ImGui::TableSetColumnIndex(colIdx++);
                        ImGui::TextColored(r.finished ? Success : Info, "%s", r.finished ? "Finished" : "Racing");
                    }
                    else if (isDrag)
                    {
                        ImGui::TableSetColumnIndex(colIdx++);
                        ImGui::PushFont(g_fontMono);
                        ImGui::TextColored(r.raceElapsed > 0.0f ? Text1 : Text3, "%s", r.raceElapsed > 0.0f ? FormatValue("race_time", r.raceElapsed).c_str() : "-");
                        ImGui::PopFont();

                        ImGui::TableSetColumnIndex(colIdx++);
                        ImGui::PushFont(g_fontMono);
                        ImGui::TextColored(r.bestLap > 0.0f ? Text2 : Text3, "%s", r.bestLap > 0.0f ? FormatValue("best_lap", r.bestLap).c_str() : "-");
                        ImGui::PopFont();

                        ImGui::TableSetColumnIndex(colIdx++);
                        ImGui::PushFont(g_fontMono);
                        ImGui::TextColored(r.topSpeedKmh > 0.0f ? Text2 : Text3, "%s", r.topSpeedKmh > 0.0f ? FormatValue("top_speed", r.topSpeedKmh).c_str() : "-");
                        ImGui::PopFont();

                        ImGui::TableSetColumnIndex(colIdx++);
                        ImGui::TextColored(r.finished ? Success : Info, "%s", r.finished ? "Finished" : "Racing");
                    }
                    else
                    {
                        ImGui::TableSetColumnIndex(colIdx++);
                        ImGui::PushFont(g_fontMono);
                        ImGui::TextColored(Text2, "%d", r.lapsCompleted);
                        ImGui::PopFont();

                        ImGui::TableSetColumnIndex(colIdx++);
                        ImGui::PushFont(g_fontMono);
                        ImGui::TextColored(r.bestLap > 0.0f ? Text2 : Text3, "%s", r.bestLap > 0.0f ? FormatValue("best_lap", r.bestLap).c_str() : "-");
                        ImGui::PopFont();

                        ImGui::TableSetColumnIndex(colIdx++);
                        ImGui::PushFont(g_fontMono);
                        ImGui::TextColored(r.finished ? Success : r.raceElapsed > 0.0f ? Text1 : Text3, "%s",
                                           r.raceElapsed > 0.0f ? FormatValue("race_time", r.raceElapsed).c_str() : "-");
                        ImGui::PopFont();

                        ImGui::TableSetColumnIndex(colIdx++);
                        ImGui::PushFont(g_fontMono);
                        ImGui::TextColored(r.topSpeedKmh > 0.0f ? Text2 : Text3, "%s", r.topSpeedKmh > 0.0f ? FormatValue("top_speed", r.topSpeedKmh).c_str() : "-");
                        ImGui::PopFont();
                    }

                    ImGui::TableSetColumnIndex(colIdx);
                    if (r.isAI)
                    {
                        ImGui::TextColored(Text3, "-");
                    }
                    else
                    {
                        int pMs = lp ? lp->pingMs : -1;
                        if (pMs < 0) pMs = ServerLink::GetPlayerPingMs(r.name);
                        if (pMs < 0 && r.isLocal) pMs = pingMs;
                        DrawPingCell(pMs);
                    }
                    ImGui::PopFont();
                }
                ImGui::EndTable();
            }

            bool dummyUnused = false;
            const Board* paceBoard = PickPaceBoard(boards, &dummyUnused);
            if (paceBoard && (paceBoard->record > 0.0 || paceBoard->best > 0.0))
            {
                ImGui::Dummy(ImVec2(0.0f, 6.0f * s));
                ImGui::Separator();
                ImGui::Dummy(ImVec2(0.0f, 6.0f * s));
                ImGui::PushFont(g_fontSmall);
                if (paceBoard->record > 0.0)
                {
                    ImGui::TextColored(Text3, "%s Record:", BoardLabel(*paceBoard).c_str());
                    ImGui::SameLine(0.0f, 6.0f * s);
                    ImGui::PushFont(g_fontMono);
                    ImGui::TextColored(Gold, "%s", FormatValue(paceBoard->metric, paceBoard->record).c_str());
                    ImGui::PopFont();
                    if (!paceBoard->recordHolder.empty())
                    {
                        ImGui::SameLine(0.0f, 6.0f * s);
                        ImGui::TextColored(Text2, "(%s)", paceBoard->recordHolder.c_str());
                    }
                }
                if (paceBoard->best > 0.0)
                {
                    if (paceBoard->record > 0.0) ImGui::SameLine(0.0f, 18.0f * s);
                    ImGui::TextColored(Text3, "Your PB:");
                    ImGui::SameLine(0.0f, 6.0f * s);
                    ImGui::PushFont(g_fontMono);
                    ImGui::TextColored(PlaceColour(paceBoard->place), "%s", FormatValue(paceBoard->metric, paceBoard->best).c_str());
                    ImGui::PopFont();
                    if (paceBoard->place > 0)
                    {
                        ImGui::SameLine(0.0f, 6.0f * s);
                        ImGui::TextColored(PlaceColour(paceBoard->place), "(#%d of %d)", paceBoard->place, paceBoard->players);
                    }
                }
                ImGui::PopFont();
            }
        }
        else
        {
            bool anyBlocked = false;
            for (const auto& p : lobbyPlayers) if (p.status == "blocked") anyBlocked = true;
            DrawAllowedCars(anyBlocked, s, 0);
            ImGui::Dummy(ImVec2(0.0f, 6.0f * s));
            if (ImGui::BeginTable("##scoreboard_lobby", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX))
            {
                ImGui::TableSetupColumn("Driver", ImGuiTableColumnFlags_WidthFixed, 170.0f * s);
                ImGui::TableSetupColumn("Car", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 92.0f * s);
                ImGui::TableSetupColumn("Ping", ImGuiTableColumnFlags_WidthFixed, 64.0f * s);

                ImGui::PushFont(g_fontMicro);
                ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
                for (int c = 0; c < 4; ++c)
                {
                    ImGui::TableSetColumnIndex(c);
                    ImGui::TextColored(Text3, "%s", ImGui::TableGetColumnName(c));
                }
                ImGui::PopFont();

                const ImU32 altBg = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.025f));
                for (size_t i = 0; i < lobbyPlayers.size(); ++i)
                {
                    const LobbyPlayer& p = lobbyPlayers[i];
                    ImGui::TableNextRow();
                    if (i % 2 == 1) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, altBg);
                    ImGui::PushFont(g_fontSmall);
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextColored(Text1, "%s", p.name.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextColored(p.car.empty() ? Text3 : Text2, "%s", p.car.empty() ? "Picking a car" : p.car.c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextColored(StatusColour(p.status), "%s", StatusLabel(p.status));
                    ImGui::TableSetColumnIndex(3);
                    const int pMs = p.pingMs >= 0 ? p.pingMs : ServerLink::GetPlayerPingMs(p.name);
                    DrawPingCell(pMs);
                    ImGui::PopFont();
                }
                ImGui::EndTable();
            }
        }

        ImGui::Dummy(ImVec2(0.0f, 6.0f * s));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0.0f, 6.0f * s));
        ImGui::PushFont(g_fontSmall);
        DrawKeyHint(g_scoreboardKeyName.c_str(), "hold to view", s);
        ImGui::SameLine(0.0f, 14.0f * s);
        DrawKeyHint(g_toggleKeyName.c_str(), g_hudVisible ? "hide HUD" : "show HUD", s);
        ImGui::PopFont();

        ImGui::End();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }

    static void BeforePresent(IDirect3DDevice9* device, IDirect3DSwapChain9*)
    {
        if (!g_ready || g_deviceLost) return;
        UseDevice(device);
        if (!g_ready) return;

        const bool front = GameInFront();
        EventVote::Poll(front);
        const EventVote::View vote = EventVote::Snapshot();
        if (ToggleKeyPressed(g_toggleKey, front && !vote.windowOpen))
        {
            g_hudVisible = !g_hudVisible;
        }
        const bool scoreboardHeld = front && !vote.windowOpen && g_scoreboardKey > 0 &&
                                    (GetAsyncKeyState(g_scoreboardKey) & 0x8000) != 0;

        // Copy what to show, so the other threads aren't held up while drawing.
        bool showStanding, showLobby, paceBarShown;
        std::string trackName, mode, lobbyRoom, lobbyAtmosphere;
        int lobbyEvent = 0, lobbyEvents = 0;
        std::vector<Board> boards;
        std::vector<Notice> notices;
        std::vector<LobbyPlayer> lobbyPlayers;
        LobbyTimerState lobbyTimer;
        const double now = Now();
        {
            Guard guard;
            showStanding = g_hudVisible && g_racing && !g_standingTrack.empty() && g_standingTrack == g_trackKey && g_standingMode == g_raceMode;
            showLobby = g_hudVisible && g_lobbyCarsShown && !g_racing && !g_lobbyPlayers.empty();
            if (showLobby || scoreboardHeld)
            {
                lobbyRoom = g_lobbyRoom;
                lobbyAtmosphere = g_lobbyAtmosphere;
                lobbyEvent = g_lobbyEventIndex;
                lobbyEvents = g_lobbyEventCount;
                lobbyPlayers = g_lobbyPlayers;
                lobbyTimer = g_lobbyTimer;
            }
            paceBarShown = g_paceBarShown;
            trackName = g_trackName.empty() ? g_trackKey : g_trackName;
            mode = g_raceMode;
            boards = g_boards;
            for (std::deque<Notice>::iterator it = g_noticeQueue.begin(); it != g_noticeQueue.end(); )
            {
                if (it->shownAt >= 0.0 && now - it->shownAt > it->duration) it = g_noticeQueue.erase(it);
                else ++it;
            }
            for (size_t i = 0; i < g_noticeQueue.size() && notices.size() < 4; ++i)
            {
                if (g_noticeQueue[i].shownAt < 0.0) g_noticeQueue[i].shownAt = now;
                if ((g_hudVisible && g_notices) || g_noticeQueue[i].kind == "refused") notices.push_back(g_noticeQueue[i]);
            }
        }
        const std::vector<RaceResults::LiveRacer> liveRacers = scoreboardHeld ? RaceResults::GetLiveRacers() : std::vector<RaceResults::LiveRacer>();
        const bool showScoreboard = scoreboardHeld && (!liveRacers.empty() || !lobbyPlayers.empty());
        const ReShifted::Indicator shift = g_hudVisible ? ReShifted::GetIndicator() : ReShifted::Indicator();
        if (!showStanding && !showLobby && !showScoreboard && notices.empty() && !g_test && !vote.windowOpen && !vote.voteActive && !shift.shown) return;
        if (!g_drawnOnce)
        {
            g_drawnOnce = true;
            g_firstDrawAt = now;
            Log::Event(Log::Tag::Client, "Overlay is drawing\n");
        }

        IDirect3DSurface9* back = nullptr;
        ImGuiIO& io = ImGui::GetIO();
        if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) && back)
        {
            D3DSURFACE_DESC desc;
            if (SUCCEEDED(back->GetDesc(&desc))) io.DisplaySize = ImVec2(static_cast<float>(desc.Width), static_cast<float>(desc.Height));
            back->Release();
        }
        LARGE_INTEGER tick;
        QueryPerformanceCounter(&tick);
        io.DeltaTime = static_cast<float>(tick.QuadPart - g_last.QuadPart) / static_cast<float>(g_frequency.QuadPart);
        if (io.DeltaTime <= 0.0f) io.DeltaTime = 1.0f / 60.0f;
        g_last = tick;

        // The vote window takes the mouse, read straight from Windows (the overlay has no window
        // procedure of its own), scaled from the window's client area to the back buffer.
        io.MouseDrawCursor = vote.windowOpen;
        io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);
        io.MouseDown[0] = false;
        if (vote.windowOpen)
        {
            D3DDEVICE_CREATION_PARAMETERS params = {};
            HWND window = SUCCEEDED(device->GetCreationParameters(&params)) && params.hFocusWindow ? params.hFocusWindow : GetForegroundWindow();
            POINT cursor;
            RECT client;
            if (window && GetCursorPos(&cursor) && ScreenToClient(window, &cursor) && GetClientRect(window, &client) && client.right > 0 &&
                client.bottom > 0)
                io.MousePos = ImVec2(cursor.x * io.DisplaySize.x / client.right, cursor.y * io.DisplaySize.y / client.bottom);
            io.MouseDown[0] = front && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        }

        const float s = g_fontScaleBase * io.FontGlobalScale;
        const int pingMs = ServerLink::GetPingMs();
        ImGui_ImplDX9_NewFrame();
        ImGui::NewFrame();
        const RaceResults::LivePace pace = (showStanding && paceBarShown) ? RaceResults::GetLivePace() : RaceResults::LivePace();
        if (showStanding) DrawStanding(trackName, mode, boards, paceBarShown, pace, pingMs, s);
        if (showLobby) DrawLobby(lobbyRoom, lobbyAtmosphere, lobbyEvent, lobbyEvents, lobbyPlayers, lobbyTimer, vote, pingMs, s);
        if (!notices.empty()) DrawNotices(notices, now, s);
        if (shift.shown) DrawShiftIndicator(shift, s);
        if (vote.voteActive) DrawVotePopup(vote, s);
        if (vote.windowOpen) DrawVoteWindow(vote, s);
        if (showScoreboard) DrawScoreboard(trackName, mode, boards, lobbyRoom, lobbyAtmosphere, lobbyEvent, lobbyEvents,
                                           lobbyPlayers, lobbyTimer, vote, liveRacers, pingMs, s);
        if (g_test)
        {
            // A fixed panel bottom left, to check the overlay reaches the screen at all.
            ImGui::SetNextWindowPos(ImVec2(24.0f * s, io.DisplaySize.y - 24.0f * s), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
            if (ImGui::Begin("##test", nullptr, PanelFlags))
            {
                ImGui::PushFont(g_fontBody);
                ImGui::TextColored(Success, "OpenProStreet overlay test");
                ImGui::PopFont();
                ImGui::PushFont(g_fontSmall);
                ImGui::TextColored(Text2, "%.0fx%.0f, %d device(s), %s", io.DisplaySize.x, io.DisplaySize.y, g_devicesCreated,
                                   showStanding ? "standing shown" : "no standing");
                ImGui::PopFont();
            }
            ImGui::End();
        }
        ImGui::EndFrame();
        // The game renders its effects through other surfaces, which can still be bound when it
        // presents. Draw on the back buffer itself, keeping a compatible depth surface bound (only
        // unbinding when sizes or MSAA differ) so overlays/wrappers like ReShade don't lose depth.
        ImGui::Render();
        ImDrawData* drawData = ImGui::GetDrawData();
        IDirect3DSurface9* gameTarget = nullptr;
        IDirect3DSurface9* gameDepth = nullptr;
        IDirect3DSurface9* backBuffer = nullptr;
        D3DVIEWPORT9 gameViewport = {};
        const bool haveViewport = SUCCEEDED(device->GetViewport(&gameViewport));
        device->GetRenderTarget(0, &gameTarget);
        device->GetDepthStencilSurface(&gameDepth);
        // ImGui's backend leaves some states as the game had them, and a game can leave colour
        // writes off, sRGB on, or clip planes and a second stream set, which hide the overlay. Keep
        // the game's states in a block of our own, set those too, and put everything back after.
        IDirect3DStateBlock9* gameState = nullptr;
        if (SUCCEEDED(device->CreateStateBlock(D3DSBT_ALL, &gameState)) && gameState) gameState->Capture();
        HRESULT targetResult = E_FAIL, sceneResult = E_FAIL;
        bool switchedTarget = false;
        bool switchedDepth = false;
        if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) && backBuffer)
        {
            if (gameTarget == backBuffer)
            {
                targetResult = S_OK;
            }
            else
            {
                targetResult = device->SetRenderTarget(0, backBuffer);
                switchedTarget = SUCCEEDED(targetResult);
            }

            if (gameDepth)
            {
                D3DSURFACE_DESC bbDesc = {};
                D3DSURFACE_DESC dsDesc = {};
                const bool compat = SUCCEEDED(backBuffer->GetDesc(&bbDesc)) &&
                                    SUCCEEDED(gameDepth->GetDesc(&dsDesc)) &&
                                    dsDesc.Width >= bbDesc.Width &&
                                    dsDesc.Height >= bbDesc.Height &&
                                    dsDesc.MultiSampleType == bbDesc.MultiSampleType &&
                                    dsDesc.MultiSampleQuality == bbDesc.MultiSampleQuality;
                if (!compat)
                {
                    device->SetDepthStencilSurface(nullptr);
                    switchedDepth = true;
                }
            }

            // Write RGB only so back-buffer alpha stays unchanged for downstream ReShade shaders.
            device->SetRenderState(D3DRS_COLORWRITEENABLE, 0x00000007);
            device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
            device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
            device->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, FALSE);
            device->SetRenderState(D3DRS_MULTISAMPLEMASK, 0xFFFFFFFF);
            device->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE, FALSE);
            device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
            device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
            device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
            device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
            device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            for (DWORD stream = 0; stream < 4; ++stream) device->SetStreamSourceFreq(stream, 1);
            device->SetVertexDeclaration(nullptr);
            sceneResult = device->BeginScene();
            if (SUCCEEDED(sceneResult))
            {
                ImGui_ImplDX9_RenderDrawData(drawData);
                device->EndScene();
            }
            backBuffer->Release();
        }
        if (gameState)
        {
            gameState->Apply();
            gameState->Release();
        }
        if (switchedTarget && gameTarget) device->SetRenderTarget(0, gameTarget);
        if (gameTarget) gameTarget->Release();
        if (switchedDepth) device->SetDepthStencilSurface(gameDepth);
        if (gameDepth) gameDepth->Release();
        if (haveViewport) device->SetViewport(&gameViewport);

        // What happened, logged once a while after drawing starts, so a missing overlay can be traced.
        ++g_frames;
        if (FAILED(targetResult) || FAILED(sceneResult)) ++g_failedFrames;
        if (!g_reported && now - g_firstDrawAt > 10.0)
        {
            g_reported = true;
            Log::Event(Log::Tag::Client, "Overlay: %d frames in 10 s, %d failed (target 0x%08lX, scene 0x%08lX), %d shapes, %d vertices, state block %s\n",
                       g_frames, g_failedFrames, static_cast<unsigned long>(targetResult), static_cast<unsigned long>(sceneResult),
                       drawData ? drawData->CmdListsCount : 0, drawData ? drawData->TotalVtxCount : 0, gameState ? "ok" : "missing");
        }
    }

    // ---- Public ----

    void Init(uintptr_t baseModule, const std::string& version)
    {
        InitializeCriticalSection(&g_lock);
        g_baseModule = baseModule;
        g_version = version;
        const std::string& settings = Log::SettingsPath();
        if (!settings.empty())
        {
            g_enabled = GetPrivateProfileIntA("Overlay", "Enabled", 1, settings.c_str()) != 0;
            g_notices = GetPrivateProfileIntA("Overlay", "Notifications", 1, settings.c_str()) != 0;
            g_test = GetPrivateProfileIntA("Overlay", "Test", 0, settings.c_str()) != 0;
            g_lobbyCarsShown = GetPrivateProfileIntA("Overlay", "LobbyCars", 1, settings.c_str()) != 0;
            g_paceBarShown = GetPrivateProfileIntA("Overlay", "PaceBar", 1, settings.c_str()) != 0;
            char keyText[32] = {};
            GetPrivateProfileStringA("Overlay", "ToggleKey", "F3", keyText, sizeof(keyText), settings.c_str());
            g_toggleKey = ParseOverlayKey(keyText, VK_F3, &g_toggleKeyName);
            GetPrivateProfileStringA("Overlay", "ScoreboardKey", "Tab", keyText, sizeof(keyText), settings.c_str());
            g_scoreboardKey = ParseOverlayKey(keyText, VK_TAB, &g_scoreboardKeyName);
            char corner[32] = {};
            GetPrivateProfileStringA("Overlay", "Position", "TopRight", corner, sizeof(corner), settings.c_str());
            if (_stricmp(corner, "TopLeft") == 0) g_corner = TopLeft;
            else if (_stricmp(corner, "BottomLeft") == 0) g_corner = BottomLeft;
            else if (_stricmp(corner, "BottomRight") == 0) g_corner = BottomRight;
            const int percent = GetPrivateProfileIntA("Overlay", "Scale", 100, settings.c_str());
            g_userScale = (percent < 50 ? 50 : percent > 200 ? 200 : percent) / 100.0f;
        }
        if (!g_enabled) return;
        if (!D3DHooks::Install())
        {
            Log::Warn("The overlay is off: the game's display couldn't be hooked.\n");
            g_enabled = false;
            return;
        }
        D3DHooks::Listener listener;
        listener.deviceCreated = DeviceCreated;
        listener.beforeReset = BeforeReset;
        listener.afterReset = AfterReset;
        listener.beforePresent = BeforePresent;
        D3DHooks::AddListener(listener);
    }

    void OnGameState(bool racing, const std::string& rawTrack, const std::string& trackName, const std::string& raceMode)
    {
        if (!g_enabled) return;
        bool ask = false;
        {
            Guard guard;
            g_racing = racing && !rawTrack.empty() && !raceMode.empty();
            g_trackKey = rawTrack;
            g_trackName = trackName;
            g_raceMode = raceMode;
            if (!g_racing)
            {
                // The next race asks again, as times may have changed.
                g_requestedTrack.clear();
                g_requestedMode.clear();
            }
            else if (rawTrack != g_requestedTrack || raceMode != g_requestedMode)
            {
                g_requestedTrack = rawTrack;
                g_requestedMode = raceMode;
                ask = true;
            }
        }
        if (ask) ServerLink::RequestStanding(rawTrack, raceMode, VaultFixes::SpeedLimit());
    }

    void SetLobbyState(const std::string& room, const std::string& atmosphere, int eventIndex, int eventCount,
                       const std::vector<LobbyPlayer>& players)
    {
        Guard guard;
        g_lobbyRoom = room;
        g_lobbyAtmosphere = atmosphere;
        g_lobbyEventIndex = eventIndex;
        g_lobbyEventCount = eventCount;
        g_lobbyPlayers = players;
    }

    void SetLobbyTimer(const LobbyTimerState& timer)
    {
        Guard guard;
        g_lobbyTimer = timer;
    }

    bool GetPausedTimerSeconds(int* outSeconds)
    {
        Guard guard;
        if (g_racing || !g_lobbyTimer.paused || g_lobbyTimer.seconds < 0) return false;
        if (outSeconds) *outSeconds = g_lobbyTimer.seconds;
        return true;
    }

    void ResetRoomState()
    {
        Guard guard;
        g_lobbyRoom.clear();
        g_lobbyAtmosphere.clear();
        g_lobbyEventIndex = 0;
        g_lobbyEventCount = 0;
        g_lobbyPlayers.clear();
        g_lobbyTimer = LobbyTimerState();
    }

    void SetStanding(const std::string& trackKey, const std::string& raceMode, const std::vector<Board>& boards)
    {
        Guard guard;
        g_standingTrack = trackKey;
        g_standingMode = raceMode;
        g_boards = boards;
    }

    void ApplyLocalResult(const std::string& trackKey, const std::string& raceMode, float raceTime, float bestLap,
                          int laps, float score, float topSpeedKmh)
    {
        Guard guard;
        if (trackKey.empty() || trackKey != g_standingTrack || raceMode != g_standingMode) return;
        for (Board& b : g_boards)
        {
            double value = 0.0;
            bool higher = false;
            if (b.metric == "best_lap") value = bestLap;
            else if (b.metric == "race_time" && b.laps == laps) value = raceTime;
            else if (b.metric == "score") { value = score; higher = true; }
            else if (b.metric == "top_speed") { value = topSpeedKmh; higher = true; }
            if (!(value > 0.0) || !std::isfinite(value)) continue;
            if (b.best > 0.0 && (higher ? value <= b.best : value >= b.best)) continue;
            b.best = value;
            if (b.record <= 0.0 || (higher ? value > b.record : value < b.record))
            {
                b.record = value;
                b.recordHolder = "You";
                b.place = 1;
                b.beat = 0.0;
                b.beatHolder.clear();
            }
        }
    }

    void RefreshStanding()
    {
        Guard guard;
        g_requestedTrack.clear();
        g_requestedMode.clear();
    }

    void OnConnected()
    {
        {
            Guard guard;
            for (std::deque<Notice>::iterator it = g_noticeQueue.begin(); it != g_noticeQueue.end(); )
            {
                if (it->kind == "refused") it = g_noticeQueue.erase(it);
                else ++it;
            }
        }
        if (!g_enabled || !g_notices) return;
        std::vector<std::string> lines;
        lines.push_back("Your times and place show here during races.");
        PushNotice("best", "OpenProStreet " + g_version + " connected", lines);
        Log::Event(Log::Tag::Client, "Overlay: connected notice queued\n");
    }

    void OnConnectionRefused(const std::string& title, const std::vector<std::string>& lines)
    {
        if (!g_enabled) return;
        Guard guard;
        for (std::deque<Notice>::iterator it = g_noticeQueue.begin(); it != g_noticeQueue.end(); )
        {
            if (it->kind == "refused") it = g_noticeQueue.erase(it);
            else ++it;
        }
        Notice n;
        n.kind = "refused";
        n.title = title;
        n.lines = lines;
        n.duration = 20.0;
        g_noticeQueue.push_front(n);
    }

    void PushNotice(const std::string& kind, const std::string& title, const std::vector<std::string>& lines, const XpGain& xp)
    {
        if (!g_enabled) return;
        Guard guard;
        Notice n;
        n.kind = kind;
        n.title = title;
        n.lines = lines;
        n.xp = xp;
        // Long enough to watch the bar fill, and the level up popup that goes with it.
        if (xp.gained > 0) n.duration = XpFillEnd(xp) + (xp.levelStarts.size() > 2 ? LEVEL_UP_SECONDS : 3.0);
        g_noticeQueue.push_back(n);
        // The result changes the standing, so the card asks again on the next poll.
        g_requestedTrack.clear();
        g_requestedMode.clear();
    }
}
