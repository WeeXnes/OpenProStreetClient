#pragma once

#include <stdio.h>

#include <string>
#include <utility>
#include <vector>

// The plugin's console output. By default it's one short, coloured line per thing that happens.
// With Verbose=1 in OpenProStreetClient.ini it's the detailed log instead, without colour.
namespace Log
{
    enum class Tag { Client, Server, Game, Match, Race, Discord, Warn, Error };

    // Reads the settings file next to the plugin. Call this before anything else logs.
    void Init(void* module);

    bool IsVerbose();

    // The settings file next to the plugin. Empty if its path couldn't be worked out.
    const std::string& SettingsPath();

    // Shows a name or value in bold on the default console.
    std::string Highlight(const std::string& text);

    // The detailed log only.
    void Debug(const char* format, ...) __attribute__((format(__MINGW_PRINTF_FORMAT, 1, 2)));
    // The default console only, for a line the detailed log says in its own words.
    void Summary(Tag tag, const char* format, ...) __attribute__((format(__MINGW_PRINTF_FORMAT, 2, 3)));
    // Both: tagged by default, the same text after "[OpenProStreet] " in the detailed log.
    void Event(Tag tag, const char* format, ...) __attribute__((format(__MINGW_PRINTF_FORMAT, 2, 3)));
    void Warn(const char* format, ...) __attribute__((format(__MINGW_PRINTF_FORMAT, 1, 2)));
    void Error(const char* format, ...) __attribute__((format(__MINGW_PRINTF_FORMAT, 1, 2)));

    // The startup block on the default console: a title, then label and value rows.
    void Banner(const std::string& title, const std::vector<std::pair<std::string, std::string> >& rows);
}
