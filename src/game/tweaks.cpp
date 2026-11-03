#include "game/tweaks.h"
#include "log.h"
#include <cstdio>
#include <cstring>

namespace Tweaks
{
    // The game reads one byte per character, so the copyright sign has to be its single Latin-1 byte.
    static const char* g_customCopyrightString = "\xA9 2007 Electronic Arts Inc. All rights reserved.^OpenProStreet v7.4 by WeeXnes";
    static uintptr_t g_loc_595859 = 0;

    static void PatchMemory(uintptr_t address, const void* data, size_t size)
    {
        DWORD oldProtect;
        if (VirtualProtect((void*)address, size, PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            memcpy((void*)address, data, size);
            VirtualProtect((void*)address, size, oldProtect, &oldProtect);
        }
    }

    static void PatchNOP(uintptr_t address, size_t count)
    {
        DWORD oldProtect;
        if (VirtualProtect((void*)address, count, PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            memset((void*)address, 0x90, count);
            VirtualProtect((void*)address, count, oldProtect, &oldProtect);
        }
    }

    static void PatchByte(uintptr_t address, uint8_t byte)
    {
        PatchMemory(address, &byte, 1);
    }

    __attribute__((naked)) static void StringReplacementCodeCave()
    {
        __asm__ __volatile__(
            "movl (%%edi,%%eax,8), %%edx\n\t"
            "cmpl $0xA7EBC389, %%edx\n\t"
            "jne 1f\n\t"
            "pushl %%ecx\n\t"
            "movl %0, %%ecx\n\t"
            "movl %%ecx, 4(%%edi,%%eax,8)\n\t"
            "popl %%ecx\n\t"
            "1:\n\t"
            "cmpl %%esi, %%edx\n\t"
            "pushl %1\n\t"
            "ret\n\t"
            :
            : "m"(g_customCopyrightString), "m"(g_loc_595859)
        );
    }

    static void InstallCopyrightStringHook(uintptr_t baseModule)
    {
        uintptr_t loc_595854 = baseModule + 0x195854;
        g_loc_595859 = baseModule + 0x195859;

        DWORD oldProtect;
        if (VirtualProtect((void*)loc_595854, 5, PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            *(uint8_t*)loc_595854 = 0xE9; // JMP rel32
            *(int32_t*)(loc_595854 + 1) = (int32_t)((uintptr_t)StringReplacementCodeCave - (loc_595854 + 5));
            VirtualProtect((void*)loc_595854, 5, oldProtect, &oldProtect);
            Log::Debug("[OpenProStreet] Copyright string hook installed at 0x%p\n", (void*)loc_595854);
        }
    }

    static void UnlockSpecialLiveries(uintptr_t baseModule)
    {
        // 1. Unhide Special vinyl categories (0x13 and 0x14) in the vinyl shop
        PatchByte(baseModule + 0x3E9DCE, 0x1B);
        PatchByte(baseModule + 0x3E9DD7, 0x1C);

        // 2. Remove Vinyl - Car specifications / restrictions (all cars can use special liveries)
        PatchNOP(baseModule + 0x3E9DB5, 6);
        PatchNOP(baseModule + 0x3E9DC6, 6);

        // 3. Make all vinyls colorable
        PatchNOP(baseModule + 0x3CE600, 2);
        PatchNOP(baseModule + 0x3CE607, 2);
        PatchNOP(baseModule + 0x3CE60E, 2);
        PatchNOP(baseModule + 0x3CE615, 2);

        // 4. Make all vinyls transformable
        PatchNOP(baseModule + 0x3CE581, 2);
        PatchNOP(baseModule + 0x3CE588, 2);

        // 5. Make all vinyls editable
        PatchNOP(baseModule + 0x3CE653, 2);
        PatchNOP(baseModule + 0x3CE65A, 2);
        PatchNOP(baseModule + 0x3CE661, 2);
        PatchNOP(baseModule + 0x3CE668, 2);
        PatchByte(baseModule + 0x3CE678, 0xEB);

        Log::Debug("[OpenProStreet] Special vinyls & premade liveries unlocked successfully.\n");
    }

    // While minimized, the game's message pump (0x701250) waits in GetMessageA, which stops the
    // main loop until a window message arrives. Removing this jump makes the pump use PeekMessageA
    // there too.
    static void KeepRunningWhileMinimized(uintptr_t baseModule)
    {
        const uint8_t expected[] = { 0x85, 0xC9, 0x75, 0x0E };
        const uintptr_t test = baseModule + 0x3012DC;
        if (memcmp((const void*)test, expected, sizeof(expected)) != 0)
        {
            Log::Debug("[OpenProStreet] Message pump not recognised, leaving it alone.\n");
            return;
        }
        PatchNOP(test + 2, 2);
        Log::Debug("[OpenProStreet] The game keeps running while minimized.\n");
    }

    // On WM_ACTIVATEAPP, the game calls 0x7BB1C0, which runs an online handler (0x7B3640) when it
    // loses focus. Turning the jump into an unconditional one skips that handler, so tabbing out
    // no longer ends the online session.
    static void StayOnlineWithoutFocus(uintptr_t baseModule)
    {
        const uint8_t expected[] = { 0x80, 0x7C, 0x24, 0x04, 0x00, 0x75, 0x05, 0xE9 };
        const uintptr_t focusLost = baseModule + 0x3BB1C0;
        if (memcmp((const void*)focusLost, expected, sizeof(expected)) != 0)
        {
            Log::Debug("[OpenProStreet] Focus handler not recognised, leaving it alone.\n");
            return;
        }
        PatchByte(focusLost + 5, 0xEB);
        Log::Debug("[OpenProStreet] Tabbing out no longer ends the online session.\n");
    }

    void Init(uintptr_t baseModule)
    {
        if (!baseModule) return;
        Log::Debug("[OpenProStreet] Initializing Extra Tweaks...\n");
        UnlockSpecialLiveries(baseModule);
        InstallCopyrightStringHook(baseModule);
        KeepRunningWhileMinimized(baseModule);
        StayOnlineWithoutFocus(baseModule);
    }
}
