#include "library.h"

#include <windows.h>
#include <stdio.h>
#include <discord.h>

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    FILE* console;
    switch (ul_reason_for_call) {
        case DLL_PROCESS_ATTACH:
            printf("initialized prostreet discord rpc\n");
            break;
    }
    return TRUE;
}