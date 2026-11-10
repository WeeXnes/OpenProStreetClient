#include "game/overlay.h"

#include <windows.h>
#include <d3d9.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>

#include "imgui.h"
#include "backends/imgui_impl_dx9.h"

#include "game/d3d_hooks.h"
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

    // ---- What to show, written by the game state and network threads, read when drawing ----

    struct Notice
    {
        std::string kind, title;
        std::vector<std::string> lines;
        double shownAt = -1.0; // seconds on the overlay's clock, set when first drawn
    };

    static CRITICAL_SECTION g_lock;
    static bool g_racing = false;
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

    static void DeviceCreated(IDirect3DDevice9* device, HWND)
    {
        if (!g_enabled || g_ready) return;
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
        g_ready = true;
        Log::Event(Log::Tag::Client, "Overlay ready\n");
        // A short hello on the first frames, so it's clear the overlay works before any race.
        Guard guard;
        Notice hello;
        hello.kind = "result";
        hello.title = "OpenProStreet " + g_version;
        hello.lines.push_back("The overlay is on. Your times and place show here during races.");
        g_noticeQueue.push_back(hello);
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
        const std::string speed = std::string(VaultFixes::SpeedLimit()) == "limited" ? "speed limiter" : "no speed limiter";
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

    static void BeforePresent(IDirect3DDevice9* device)
    {
        if (!g_ready || g_deviceLost) return;

        // Copy what to show, so the other threads aren't held up while drawing.
        bool showStanding;
        std::string trackName, mode;
        std::vector<Board> boards;
        std::vector<Notice> notices;
        const double now = Now();
        {
            Guard guard;
            showStanding = g_racing && !g_standingTrack.empty() && g_standingTrack == g_trackKey && g_standingMode == g_raceMode;
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
        if (!showStanding && notices.empty()) return;
        if (!g_drawnOnce)
        {
            g_drawnOnce = true;
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

        const float s = g_fontScaleBase * io.FontGlobalScale;
        ImGui_ImplDX9_NewFrame();
        ImGui::NewFrame();
        if (showStanding) DrawStanding(trackName, mode, boards, s);
        if (g_notices) DrawNotices(notices, now, s);
        ImGui::EndFrame();
        if (SUCCEEDED(device->BeginScene()))
        {
            ImGui::Render();
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
            device->EndScene();
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
            char corner[32] = {};
            GetPrivateProfileStringA("Overlay", "Position", "TopRight", corner, sizeof(corner), settings.c_str());
            if (_stricmp(corner, "TopLeft") == 0) g_corner = TopLeft;
            else if (_stricmp(corner, "BottomLeft") == 0) g_corner = BottomLeft;
            else if (_stricmp(corner, "BottomRight") == 0) g_corner = BottomRight;
            const int percent = GetPrivateProfileIntA("Overlay", "Scale", 100, settings.c_str());
            g_userScale = (percent < 50 ? 50 : percent > 200 ? 200 : percent) / 100.0f;
        }
        if (!g_enabled) return;
        if (!D3DHooks::Install(baseModule))
        {
            Log::Warn("The overlay is off: another plugin may already be handling the game's graphics.\n");
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

    void SetStanding(const std::string& trackKey, const std::string& raceMode, const std::vector<Board>& boards)
    {
        Guard guard;
        g_standingTrack = trackKey;
        g_standingMode = raceMode;
        g_boards = boards;
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
