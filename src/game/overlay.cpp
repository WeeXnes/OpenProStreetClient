#include "game/overlay.h"

#include <windows.h>
#include <d3d9.h>

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>

#include "imgui.h"
#include "backends/imgui_impl_dx9.h"

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
    static Corner g_corner = TopRight;
    static float g_userScale = 1.0f;
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
        double shownAt = -1.0; // seconds on the overlay's clock, set when first drawn
    };

    static CRITICAL_SECTION g_lock;
    static bool g_racing = false;
    // Who is in the room and the car each picked, from the server. Shown while not racing.
    static std::vector<std::pair<std::string, std::string> > g_lobbyCars;
    static bool g_lobbyCarsShown = true;
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

    static void DrawStanding(const std::string& trackName, const std::string& mode, const std::vector<Board>& boards, float s)
    {
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        const float margin = 24.0f * s;
        const bool right = g_corner == TopRight || g_corner == BottomRight;
        const bool bottom = g_corner == BottomLeft || g_corner == BottomRight;
        ImGui::SetNextWindowPos(ImVec2(right ? display.x - margin : margin, bottom ? display.y - margin : margin), ImGuiCond_Always,
                                ImVec2(right ? 1.0f : 0.0f, bottom ? 1.0f : 0.0f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(300.0f * s, 0.0f), ImVec2(420.0f * s, display.y));
        if (!ImGui::Begin("##standing", nullptr, PanelFlags)) { ImGui::End(); return; }

        ImGui::PushFont(g_fontBody);
        ImGui::TextColored(Text1, "%s", trackName.c_str());
        ImGui::PopFont();
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
        ImGui::End();
    }

    // Who is in the room and which car each has picked, in the standing card's corner while not racing.
    static void DrawLobby(const std::vector<std::pair<std::string, std::string> >& cars, float s)
    {
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        const float margin = 24.0f * s;
        const bool right = g_corner == TopRight || g_corner == BottomRight;
        const bool bottom = g_corner == BottomLeft || g_corner == BottomRight;
        ImGui::SetNextWindowPos(ImVec2(right ? display.x - margin : margin, bottom ? display.y - margin : margin), ImGuiCond_Always,
                                ImVec2(right ? 1.0f : 0.0f, bottom ? 1.0f : 0.0f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(300.0f * s, 0.0f), ImVec2(460.0f * s, display.y));
        if (!ImGui::Begin("##lobby", nullptr, PanelFlags)) { ImGui::End(); return; }
        ImGui::PushFont(g_fontBody);
        ImGui::TextColored(Text1, "In this room");
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0.0f, 4.0f * s));
        if (ImGui::BeginTable("##lobbycars", 2, ImGuiTableFlags_SizingFixedFit))
        {
            ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, 140.0f * s);
            ImGui::TableSetupColumn("car", ImGuiTableColumnFlags_WidthStretch);
            ImGui::PushFont(g_fontSmall);
            for (const auto& entry : cars)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(Text1, "%s", entry.first.c_str());
                ImGui::TableSetColumnIndex(1);
                if (entry.second.empty()) ImGui::TextColored(Text3, "Picking a car");
                else ImGui::TextColored(Text2, "%s", entry.second.c_str());
            }
            ImGui::PopFont();
            ImGui::EndTable();
        }
        ImGui::End();
    }

    static ImVec4 NoticeColour(const std::string& kind)
    {
        return kind == "record" ? Gold : kind == "best" ? Success : kind == "not_counted" ? Warning : Text2;
    }

    // Short notifications, top centre, newest at the top. Each fades in, stays, and fades out.
    static void DrawNotices(std::vector<Notice>& notices, double now, float s)
    {
        const double life = 7.0, fade = 0.25;
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        float y = 24.0f * s;
        int index = 0;
        for (Notice& n : notices)
        {
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
                y += ImGui::GetWindowHeight() + 8.0f * s;
            }
            ImGui::End();
            ImGui::PopStyleVar();
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

    static void BeforePresent(IDirect3DDevice9* device, IDirect3DSwapChain9*)
    {
        if (!g_ready || g_deviceLost) return;
        UseDevice(device);
        if (!g_ready) return;

        const bool front = GameInFront();
        EventVote::Poll(front);
        const EventVote::View vote = EventVote::Snapshot();

        // Copy what to show, so the other threads aren't held up while drawing.
        bool showStanding, showLobby;
        std::string trackName, mode;
        std::vector<Board> boards;
        std::vector<Notice> notices;
        std::vector<std::pair<std::string, std::string> > lobby;
        const double now = Now();
        {
            Guard guard;
            showStanding = g_racing && !g_standingTrack.empty() && g_standingTrack == g_trackKey && g_standingMode == g_raceMode;
            showLobby = g_lobbyCarsShown && !g_racing && !g_lobbyCars.empty();
            if (showLobby) lobby = g_lobbyCars;
            trackName = g_trackName.empty() ? g_trackKey : g_trackName;
            mode = g_raceMode;
            boards = g_boards;
            while (!g_noticeQueue.empty() && g_noticeQueue.front().shownAt >= 0.0 && now - g_noticeQueue.front().shownAt > 7.0)
                g_noticeQueue.pop_front();
            for (size_t i = 0; i < g_noticeQueue.size() && i < 4; ++i)
            {
                if (g_noticeQueue[i].shownAt < 0.0) g_noticeQueue[i].shownAt = now;
                notices.push_back(g_noticeQueue[i]);
            }
        }
        if (!showStanding && !showLobby && notices.empty() && !g_test && !vote.windowOpen && !vote.voteActive) return;
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
        ImGui_ImplDX9_NewFrame();
        ImGui::NewFrame();
        if (showStanding) DrawStanding(trackName, mode, boards, s);
        if (showLobby) DrawLobby(lobby, s);
        if (g_notices) DrawNotices(notices, now, s);
        if (vote.voteActive) DrawVotePopup(vote, s);
        if (vote.windowOpen) DrawVoteWindow(vote, s);
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
        // presents. Draw on the back buffer itself, without a depth buffer of another size, and put
        // the game's surfaces back after.
        ImGui::Render();
        ImDrawData* drawData = ImGui::GetDrawData();
        IDirect3DSurface9* gameTarget = nullptr;
        IDirect3DSurface9* gameDepth = nullptr;
        IDirect3DSurface9* backBuffer = nullptr;
        device->GetRenderTarget(0, &gameTarget);
        device->GetDepthStencilSurface(&gameDepth);
        // ImGui's backend leaves some states as the game had them, and a game can leave colour
        // writes off, sRGB on, or clip planes and a second stream set, which hide the overlay. Keep
        // the game's states in a block of our own, set those too, and put everything back after.
        IDirect3DStateBlock9* gameState = nullptr;
        if (SUCCEEDED(device->CreateStateBlock(D3DSBT_ALL, &gameState)) && gameState) gameState->Capture();
        HRESULT targetResult = E_FAIL, sceneResult = E_FAIL;
        if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) && backBuffer)
        {
            targetResult = device->SetRenderTarget(0, backBuffer);
            device->SetDepthStencilSurface(nullptr);
            device->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0000000F);
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
        if (gameTarget)
        {
            device->SetRenderTarget(0, gameTarget);
            gameTarget->Release();
        }
        device->SetDepthStencilSurface(gameDepth);
        if (gameDepth) gameDepth->Release();

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
        g_version = version;
        const std::string& settings = Log::SettingsPath();
        if (!settings.empty())
        {
            g_enabled = GetPrivateProfileIntA("Overlay", "Enabled", 1, settings.c_str()) != 0;
            g_notices = GetPrivateProfileIntA("Overlay", "Notifications", 1, settings.c_str()) != 0;
            g_test = GetPrivateProfileIntA("Overlay", "Test", 0, settings.c_str()) != 0;
            g_lobbyCarsShown = GetPrivateProfileIntA("Overlay", "LobbyCars", 1, settings.c_str()) != 0;
            char corner[32] = {};
            GetPrivateProfileStringA("Overlay", "Position", "TopRight", corner, sizeof(corner), settings.c_str());
            if (_stricmp(corner, "TopLeft") == 0) g_corner = TopLeft;
            else if (_stricmp(corner, "BottomLeft") == 0) g_corner = BottomLeft;
            else if (_stricmp(corner, "BottomRight") == 0) g_corner = BottomRight;
            const int percent = GetPrivateProfileIntA("Overlay", "Scale", 100, settings.c_str());
            g_userScale = (percent < 50 ? 50 : percent > 200 ? 200 : percent) / 100.0f;
        }
        if (!g_enabled) return;
        (void)baseModule;
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

    void SetLobbyCars(const std::vector<std::pair<std::string, std::string> >& cars)
    {
        Guard guard;
        g_lobbyCars = cars;
    }

    void SetStanding(const std::string& trackKey, const std::string& raceMode, const std::vector<Board>& boards)
    {
        Guard guard;
        g_standingTrack = trackKey;
        g_standingMode = raceMode;
        g_boards = boards;
    }

    void OnConnected()
    {
        if (!g_enabled || !g_notices) return;
        std::vector<std::string> lines;
        lines.push_back("Your times and place show here during races.");
        PushNotice("best", "OpenProStreet " + g_version + " connected", lines);
        Log::Event(Log::Tag::Client, "Overlay: connected notice queued\n");
    }

    void PushNotice(const std::string& kind, const std::string& title, const std::vector<std::string>& lines)
    {
        if (!g_enabled) return;
        Guard guard;
        Notice n;
        n.kind = kind;
        n.title = title;
        n.lines = lines;
        g_noticeQueue.push_back(n);
        // The result changes the standing, so the card asks again on the next poll.
        g_requestedTrack.clear();
        g_requestedMode.clear();
    }
}
