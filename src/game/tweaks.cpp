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

    void Init(uintptr_t baseModule)
    {
        if (!baseModule) return;
        Log::Debug("[OpenProStreet] Initializing Extra Tweaks...\n");
        UnlockSpecialLiveries(baseModule);
        InstallCopyrightStringHook(baseModule);
        KeepRunningWhileMinimized(baseModule);
        StayOnlineWithoutFocus(baseModule);
        NoEaAutoLogin(baseModule);
    }
}
