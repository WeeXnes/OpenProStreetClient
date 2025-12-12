#include "library.h"  // If you have custom headers; otherwise remove

#include <windows.h>
#include <stdio.h>
#include <thread>
#include <discord.h>  // From SDK

discord::Core* discordCore = nullptr;
discord::Activity activity{};

DWORD WINAPI DiscordThread(LPVOID param) {
    while (true) {
        if (discordCore) {
            discordCore->RunCallbacks();
        }
        Sleep(16);  // Run ~60 times per second; adjust to balance CPU usage
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
        case DLL_PROCESS_ATTACH: {
            printf("initialized prostreet discord rpc\n");

            // Initialize Discord Core
            discord::Result result = discord::Core::Create(656565390411038731, DiscordCreateFlags_Default, &discordCore);
            if (result != discord::Result::Ok) {
                printf("Failed to create Discord core: %d\n", static_cast<int>(result));
                return FALSE;
            }

            // Optional: Set log hook for SDK debugging
            discordCore->SetLogHook(discord::LogLevel::Debug, [](discord::LogLevel level, const char* message) {
                printf("[Discord SDK] %s\n", message);
            });

            // Set up basic "Hello World" Rich Presence activity
            activity.SetType(discord::ActivityType::Playing);
            activity.SetApplicationId(656565390411038731);
            activity.SetName("Hello World");
            activity.SetDetails("Just testing RPC");
            activity.SetState("Hello from NFS ProStreet!");
            activity.GetTimestamps().SetStart(time(nullptr));

            // Update the activity
            discordCore->ActivityManager().UpdateActivity(activity, [](discord::Result res) {
                if (res == discord::Result::Ok) {
                    printf("Hello World Rich Presence updated successfully\n");
                } else {
                    printf("Failed to update activity: %d\n", static_cast<int>(res));
                }
            });

            // Start the callback thread
            CreateThread(NULL, 0, DiscordThread, NULL, 0, NULL);
            break;
        }

        case DLL_PROCESS_DETACH: {
            // Cleanup
            discordCore = nullptr;  // SDK handles destruction automatically
            break;
        }
    }

    return TRUE;
}