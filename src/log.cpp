#include "log.h"

#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif

namespace Log
{
    // Bytes that stand for colours until a line reaches the console. Text can't contain them,
    // because Clean replaces every control character first.
    static const char MARK_BOLD = '\x01';
    static const char MARK_RESET = '\x02';
    static const char MARK_DIM = '\x03';
    static const char MARK_TAG = '\x04'; // followed by the tag's number, as '0' + number

    static const char* const PREFIX = "[OpenProStreet] ";
    static const size_t TAG_WIDTH = 7;
    static const size_t MAX_LINE = 1024;
    static const size_t MAX_PENDING = 64 * 1024;

    struct TagStyle {
        const char* name; // at most TAG_WIDTH characters
        const char* sgr;  // colour codes, without the ESC [ and m
    };

    // Indexed by Tag.
    static const TagStyle TAGS[] = {
        {"CLIENT", "1;36"},  // Client: bold cyan
        {"SERVER", "36"},    // Server: cyan
        {"GAME", "32"},      // Game: green
        {"MATCH", "35"},     // Match: magenta
        {"RACE", "1;34"},    // Race: bold blue
        {"DISCORD", "94"},   // Discord: bright blue
        {"WARN", "1;33"},    // Warn: bold yellow
        {"ERROR", "1;31"},   // Error: bold red
    };

    static bool g_verbose = false;
    static bool g_colourSetting = true;
    static CRITICAL_SECTION g_lock;
    static HANDLE g_console = INVALID_HANDLE_VALUE;
    static bool g_consoleColour = false;
    static std::string g_pending;

    // Replaces control characters, and the C1 controls that UTF-8 terminals read as escape codes.
    static std::string Clean(const std::string& text, bool keepLayout)
    {
        std::string out;
        out.reserve(text.size());
        for (size_t i = 0; i < text.size(); ++i)
        {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            if (keepLayout && (c == '\n' || c == '\t'))
            {
                out.push_back(static_cast<char>(c));
            }
            else if (c < 0x20 || c == 0x7F)
            {
                out.push_back('?');
            }
            else if (c == 0xC2 && i + 1 < text.size() && static_cast<unsigned char>(text[i + 1]) >= 0x80 &&
                     static_cast<unsigned char>(text[i + 1]) < 0xA0)
            {
                out.push_back('?');
                ++i;
            }
            else
            {
                out.push_back(static_cast<char>(c));
            }
        }
        return out;
    }

    // Like Clean, but keeps the marks Highlight added to the text.
    static std::string CleanKeepingMarks(const std::string& text)
    {
        std::string out;
        size_t start = 0;
        for (size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] != MARK_BOLD && text[i] != MARK_RESET) continue;
            out += Clean(text.substr(start, i - start), true);
            out.push_back(text[i]);
            start = i + 1;
        }
        return out + Clean(text.substr(start), true);
    }

    static std::string Render(const std::string& text, bool colour)
    {
        std::string out;
        out.reserve(text.size() + 32);
        for (size_t i = 0; i < text.size(); ++i)
        {
            const char c = text[i];
            if (c == MARK_BOLD) { if (colour) out += "\x1b[1m"; }
            else if (c == MARK_RESET) { if (colour) out += "\x1b[0m"; }
            else if (c == MARK_DIM) { if (colour) out += "\x1b[2m"; }
            else if (c == MARK_TAG && i + 1 < text.size())
            {
                ++i;
                if (colour)
                {
                    out += "\x1b[";
                    out += TAGS[text[i] - '0'].sgr;
                    out += 'm';
                }
            }
            else out.push_back(c);
        }
        return out;
    }

    // The console can appear after this plugin loads: another plugin creates it, so this looks again
    // until there is one. It writes to the console directly, which works whichever C runtime that
    // plugin used.
    static bool FindConsole()
    {
        if (g_console != INVALID_HANDLE_VALUE) return true;
        if (!GetConsoleWindow()) return false;
        HANDLE console = CreateFileA("CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     nullptr, OPEN_EXISTING, 0, nullptr);
        if (console == INVALID_HANDLE_VALUE) return false;
        DWORD mode = 0;
        const bool canSetMode = GetConsoleMode(console, &mode) != 0;
        g_consoleColour = g_colourSetting && canSetMode &&
                          SetConsoleMode(console, mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
        g_console = console;
        return true;
    }

    static void WriteToConsole(const std::string& text)
    {
        const std::string rendered = Render(text, g_consoleColour);
        DWORD written = 0;
        WriteFile(g_console, rendered.data(), static_cast<DWORD>(rendered.size()), &written, nullptr);
    }

    // One write per line, so lines from different threads don't mix.
    static void Output(const std::string& text)
    {
        EnterCriticalSection(&g_lock);
        if (FindConsole())
        {
            if (!g_pending.empty())
            {
                WriteToConsole(g_pending);
                g_pending.clear();
            }
            WriteToConsole(text);
        }
        else
        {
            const std::string plain = Render(text, false);
            fwrite(plain.data(), 1, plain.size(), stdout);
            fflush(stdout);
            // Kept for the console, so it still shows what happened before it opened.
            if (g_pending.size() + text.size() <= MAX_PENDING) g_pending += text;
        }
        LeaveCriticalSection(&g_lock);
    }

    static void WriteTagged(Tag tag, const std::string& message)
    {
        const TagStyle& style = TAGS[static_cast<int>(tag)];
        SYSTEMTIME now;
        GetLocalTime(&now);
        char clock[16];
        snprintf(clock, sizeof(clock), "%02d:%02d:%02d", now.wHour, now.wMinute, now.wSecond);

        std::string line;
        line += MARK_DIM;
        line += clock;
        line += MARK_RESET;
        line += ' ';
        line += MARK_TAG;
        line += static_cast<char>('0' + static_cast<int>(tag));
        line += style.name;
        line += MARK_RESET;
        line.append(TAG_WIDTH - strlen(style.name) + 2, ' ');
        line += CleanKeepingMarks(message);
        Output(line);
    }

    static std::string Format(const char* format, va_list args)
    {
        char buffer[MAX_LINE];
        const int size = vsnprintf(buffer, sizeof(buffer), format, args);
        if (size < 0) return std::string();
        std::string text(buffer, std::min(static_cast<size_t>(size), sizeof(buffer) - 1));
        // A line cut off at the limit still ends its line.
        if (static_cast<size_t>(size) >= sizeof(buffer)) text.back() = '\n';
        return text;
    }

    static void EventV(Tag tag, const char* format, va_list args)
    {
        const std::string message = Format(format, args);
        if (g_verbose) Output(PREFIX + Clean(message, true));
        else WriteTagged(tag, message);
    }

    static std::string g_settingsPath;

    void Init(void* module)
    {
        InitializeCriticalSection(&g_lock);

        char path[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameA(static_cast<HMODULE>(module), path, MAX_PATH);
        if (length > 0 && length < MAX_PATH)
        {
            std::string settings(path);
            const size_t dot = settings.find_last_of('.');
            if (dot != std::string::npos) settings.erase(dot);
            settings += ".ini";
            g_settingsPath = settings;
            g_verbose = GetPrivateProfileIntA("Log", "Verbose", 0, settings.c_str()) != 0;
            g_colourSetting = GetPrivateProfileIntA("Log", "Colour", 1, settings.c_str()) != 0;
        }
        const char* noColour = getenv("NO_COLOR");
        if (g_verbose || (noColour && *noColour)) g_colourSetting = false;
    }

    bool IsVerbose() { return g_verbose; }

    const std::string& SettingsPath() { return g_settingsPath; }

    std::string Highlight(const std::string& text)
    {
        const std::string safe = Clean(text, false);
        if (g_verbose) return safe;
        return MARK_BOLD + safe + MARK_RESET;
    }

    void Debug(const char* format, ...)
    {
        if (!g_verbose) return;
        va_list args;
        va_start(args, format);
        const std::string message = Format(format, args);
        va_end(args);
        Output(Clean(message, true));
    }

    void Summary(Tag tag, const char* format, ...)
    {
        if (g_verbose) return;
        va_list args;
        va_start(args, format);
        const std::string message = Format(format, args);
        va_end(args);
        WriteTagged(tag, message);
    }

    void Event(Tag tag, const char* format, ...)
    {
        va_list args;
        va_start(args, format);
        EventV(tag, format, args);
        va_end(args);
    }

    void Warn(const char* format, ...)
    {
        va_list args;
        va_start(args, format);
        EventV(Tag::Warn, format, args);
        va_end(args);
    }

    void Error(const char* format, ...)
    {
        va_list args;
        va_start(args, format);
        EventV(Tag::Error, format, args);
        va_end(args);
    }

    void Banner(const std::string& title, const std::vector<std::pair<std::string, std::string> >& rows)
    {
        if (g_verbose) return;
        size_t labelWidth = 0;
        for (const auto& row : rows) labelWidth = std::max(labelWidth, row.first.size());

        std::string text;
        text += MARK_TAG;
        text += static_cast<char>('0' + static_cast<int>(Tag::Client));
        text += Clean(title, false);
        text += MARK_RESET;
        text += '\n';
        for (const auto& row : rows)
        {
            text += "  ";
            text += MARK_DIM;
            text += Clean(row.first, false);
            text += MARK_RESET;
            text.append(labelWidth - row.first.size() + 2, ' ');
            text += Clean(row.second, false);
            text += '\n';
        }
        Output(text);
    }
}
