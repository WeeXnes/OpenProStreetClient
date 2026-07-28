#pragma once
#include <windows.h>
#include <discord.h>

extern discord::Core* discordCore;
extern const std::string version;

DWORD WINAPI DiscordThread(LPVOID param);
