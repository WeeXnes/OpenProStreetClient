#include "game/tweaks.h"
#include "log.h"
#include <cstdio>
#include <cstring>

namespace Tweaks
{
    // The game reads one byte per character, so the copyright sign has to be its single Latin-1 byte.
    // SetVersion fills in the client's version before the hook is installed.
    static char g_copyrightBuffer[128] = "\xA9 2007 Electronic Arts Inc. All rights reserved.^OpenProStreet by WeeXnes";

    // Game texts shown differently, by their ID in the language files (the same in every language).
    // A zero ID ends the list.
    struct TextReplacement { uint32_t id; const char* text; };
    static TextReplacement g_textReplacements[] = {
        { 0xA7EBC389, g_copyrightBuffer },                              // the main menu's copyright line
        { 0xF6BC1F49, "OpenProStreet" },                                // LAN_LAN_PLAY, the main menu's "LAN Play"
        { 0xEDD0BB02, "CREATE OPENPROSTREET ACCOUNT" },                 // "Create new LAN account"
        { 0xEBF92CFC, "LOG IN TO OPENPROSTREET ACCOUNT" },              // "Use existing LAN account"
        { 0x11B2D25A, "CREATE OPENPROSTREET ACCOUNT" },                 // "Create LAN account"
        { 0x540D2A12, "Creating your OpenProStreet account..." },       // "Creating new LAN account..."
        { 0x177BF6B1, "OpenProStreet account created." },               // "LAN account created successfully."
        { 0x26CE18DD, "Couldn't create the OpenProStreet account." },   // "LAN account creation failed."
        { 0x3D9D33D7, "OPENPROSTREET" },                                // "Select LAN login", the account screen's title
        { 0xAD965973, "OPENPROSTREET" },                                // "LAN login"
        { 0, nullptr },
    };
    static TextReplacement* g_textReplacementList = g_textReplacements;
    static uintptr_t g_loc_595859 = 0;

    // 60 Hz OLMSG_CarState transmit cadence (replaces the game's 15 Hz = 0.06666667 s cap in 0x7C1340).
    static const float  g_carStateIntervalFloat  = 1.0f / 60.0f;
    static const double g_carStateIntervalDouble = 1.0 / 60.0;

    // Hermite interpolation window in ExtrapolatedCar (0x74F340 / 0x74F590): 25 ms .. 100 ms for 60 Hz
    // (replaces the 15 Hz window of 75 ms .. 200 ms, and 1 / 0.025 = 40.0f fallback rate).
    static const double g_carBlendMaxDouble    = 0.10;
    static const float  g_carBlendMaxFloat     = 0.10f;
    static const double g_carBlendMinDouble    = 0.025;
    static const float  g_carBlendMinFloat     = 0.025f;
    static const float  g_carBlendInvMinFloat  = 1.0f / 0.025f;

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
            // Each entry of the game's text table is {ID, text pointer}. An ID in our list gets our text.
            "pushl %%ecx\n\t"
            "pushl %%ebx\n\t"
            "movl %0, %%ecx\n\t"
            "1:\n\t"
            "movl (%%ecx), %%ebx\n\t"
            "testl %%ebx, %%ebx\n\t"
            "jz 3f\n\t"
            "cmpl %%ebx, %%edx\n\t"
            "jne 2f\n\t"
            "movl 4(%%ecx), %%ebx\n\t"
            "movl %%ebx, 4(%%edi,%%eax,8)\n\t"
            "jmp 3f\n\t"
            "2:\n\t"
            "addl $8, %%ecx\n\t"
            "jmp 1b\n\t"
            "3:\n\t"
            "popl %%ebx\n\t"
            "popl %%ecx\n\t"
            // The instruction the jump here replaced, with the flags the game's code goes on to use.
            "cmpl %%esi, %%edx\n\t"
            "pushl %1\n\t"
            "ret\n\t"
            :
            : "m"(g_textReplacementList), "m"(g_loc_595859)
        );
    }

    void SetVersion(const char* version)
    {
        snprintf(g_copyrightBuffer, sizeof(g_copyrightBuffer),
                 "\xA9 2007 Electronic Arts Inc. All rights reserved.^OpenProStreet v%s by WeeXnes", version);
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
            Log::Debug("[OpenProStreet] Text replacement hook installed at 0x%p\n", (void*)loc_595854);
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

    // When the main menu opens (FEMainStateManager, vtable slot 21 at 0x7D7E20), the game asks
    // 0x7B9460 whether to log in to EA automatically, once per session. If so, it opens the EA login
    // screen (FEPCOnlineLoginStateManager), which shows "Connecting to EA server..." until the player
    // cancels it, since EA's servers are gone. 0x7B9460 already has a "no" path that also marks the
    // attempt as made (0x7B9483), so its first instruction becomes a jump there. Logging in to EA
    // Nation from the menu still works as before; OpenProStreet's LAN and Quick Play don't use it.
    static void NoEaAutoLogin(uintptr_t baseModule)
    {
        // cmp byte [auto login flag], 0 ; je 0x7B9483
        const uintptr_t check = baseModule + 0x3B9460;
        const uint8_t* code = reinterpret_cast<const uint8_t*>(check);
        if (code[0] != 0x80 || code[1] != 0x3D || code[6] != 0x00 || code[7] != 0x74 || code[8] != 0x1A ||
            code[0x23] != 0xC6 || code[0x24] != 0x05)
        {
            Log::Debug("[OpenProStreet] EA auto login check not recognised, leaving it alone.\n");
            return;
        }
        const uint8_t jump[] = { 0xEB, 0x21 }; // jmp short 0x7B9483
        PatchMemory(check, jump, sizeof(jump));
        Log::Debug("[OpenProStreet] The main menu no longer tries to log in to EA.\n");
    }

    static void PatchPointerOperand(uintptr_t instrAddr, size_t ptrOffset, const void* targetPtr)
    {
        uintptr_t addr = reinterpret_cast<uintptr_t>(targetPtr);
        PatchMemory(instrAddr + ptrOffset, &addr, sizeof(addr));
    }

    // Raises OLMSG_CarState send rate in 0x7C1340 from 15 Hz (66.7 ms) to 60 Hz (16.7 ms),
    // tightens the ExtrapolatedCar Hermite blend window (0x74F340 / 0x74F590) from 75..200 ms
    // to 25..100 ms, and requests 1 ms Windows timer resolution for timeGetTime().
    // Instruction pointer operands are redirected to our own constants so shared .rdata values
    // (0x9713D8, 0x98D674, 0x9FAA18) remain untouched for the rest of the engine.
    static void ImproveMultiplayerCarSync(uintptr_t baseModule)
    {
        HMODULE winmm = GetModuleHandleA("winmm.dll");
        if (!winmm) winmm = LoadLibraryA("winmm.dll");
        if (winmm)
        {
            typedef UINT (WINAPI *TimeBeginPeriodFn)(UINT);
            TimeBeginPeriodFn beginPeriod = reinterpret_cast<TimeBeginPeriodFn>(GetProcAddress(winmm, "timeBeginPeriod"));
            if (beginPeriod) beginPeriod(1);
        }

        // 0x7C13C3: 0F 2F 05 [74 D6 98 00]  comiss xmm0, dword ptr [0x98D674]
        // 0x7C13E7: DC 05    [D8 13 97 00]  fadd   qword ptr [0x9713D8]
        const uintptr_t sendCmp = baseModule + 0x3C13C3;
        const uintptr_t sendAdd = baseModule + 0x3C13E7;
        const uint8_t expectedSendCmp[] = { 0x0F, 0x2F, 0x05 };
        const uint8_t expectedSendAdd[] = { 0xDC, 0x05 };
        if (memcmp((const void*)sendCmp, expectedSendCmp, sizeof(expectedSendCmp)) != 0 ||
            memcmp((const void*)sendAdd, expectedSendAdd, sizeof(expectedSendAdd)) != 0)
        {
            Log::Debug("[OpenProStreet] CarState send timer not recognised, leaving it alone.\n");
            return;
        }
        PatchPointerOperand(sendCmp, 3, &g_carStateIntervalFloat);
        PatchPointerOperand(sendAdd, 2, &g_carStateIntervalDouble);

        // 0x74F37F: DD 05       [18 AA 9F 00]  fld   qword ptr [0x9FAA18] (0.2 max blend compare)
        // 0x74F393: F3 0F 10 05 [78 67 99 00]  movss xmm0, dword ptr [0x996778] (0.2f max blend clamp)
        // 0x74F3A5: DD 05       [50 6F 99 00]  fld   qword ptr [0x996F50] (0.075 min blend compare)
        // 0x74F3B1: F3 0F 10 05 [74 67 99 00]  movss xmm0, dword ptr [0x996774] (0.075f min blend clamp)
        // 0x7726B3: D9 05       [78 67 99 00]  fld   dword ptr [0x996778] (0.2f fallback blend)
        // 0x74F59D: F3 0F 10 05 [58 6F 99 00]  movss xmm0, dword ptr [0x996F58] (1/0.075f fallback inv blend)
        // 0x7713E1: F3 0F 10 05 [58 6F 99 00]  movss xmm0, dword ptr [0x996F58] (1/0.075f init inv blend)
        const uintptr_t blendMaxCmp   = baseModule + 0x34F37F;
        const uintptr_t blendMaxClamp = baseModule + 0x34F393;
        const uintptr_t blendMinCmp   = baseModule + 0x34F3A5;
        const uintptr_t blendMinClamp = baseModule + 0x34F3B1;
        const uintptr_t blendFallback = baseModule + 0x3726B3;
        const uintptr_t invBlendUse   = baseModule + 0x34F59D;
        const uintptr_t invBlendInit  = baseModule + 0x3713E1;
        const uint8_t expectedFldQword[] = { 0xDD, 0x05 };
        const uint8_t expectedFldDword[] = { 0xD9, 0x05 };
        const uint8_t expectedMovss[]    = { 0xF3, 0x0F, 0x10, 0x05 };
        if (memcmp((const void*)blendMaxCmp,   expectedFldQword, sizeof(expectedFldQword)) == 0 &&
            memcmp((const void*)blendMaxClamp, expectedMovss,    sizeof(expectedMovss))    == 0 &&
            memcmp((const void*)blendMinCmp,   expectedFldQword, sizeof(expectedFldQword)) == 0 &&
            memcmp((const void*)blendMinClamp, expectedMovss,    sizeof(expectedMovss))    == 0 &&
            memcmp((const void*)blendFallback, expectedFldDword, sizeof(expectedFldDword)) == 0 &&
            memcmp((const void*)invBlendUse,   expectedMovss,    sizeof(expectedMovss))    == 0 &&
            memcmp((const void*)invBlendInit,  expectedMovss,    sizeof(expectedMovss))    == 0)
        {
            PatchPointerOperand(blendMaxCmp,   2, &g_carBlendMaxDouble);
            PatchPointerOperand(blendMaxClamp, 4, &g_carBlendMaxFloat);
            PatchPointerOperand(blendMinCmp,   2, &g_carBlendMinDouble);
            PatchPointerOperand(blendMinClamp, 4, &g_carBlendMinFloat);
            PatchPointerOperand(blendFallback, 2, &g_carBlendMaxFloat);
            PatchPointerOperand(invBlendUse,   4, &g_carBlendInvMinFloat);
            PatchPointerOperand(invBlendInit,  4, &g_carBlendInvMinFloat);
        }

        Log::Debug("[OpenProStreet] 60 Hz CarState sync & smoother active.\n");
    }

    void Init(uintptr_t baseModule)
    {
        if (!baseModule) return;
        Log::Debug("[OpenProStreet] Initializing Extra Tweaks...\n");
        UnlockSpecialLiveries(baseModule);
        InstallCopyrightStringHook(baseModule);
        KeepRunningWhileMinimized(baseModule);
        StayOnlineWithoutFocus(baseModule);
        NoEaAutoLogin(baseModule);
        ImproveMultiplayerCarSync(baseModule);
    }
}
