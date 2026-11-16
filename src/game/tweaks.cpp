#include "game/tweaks.h"
#include "game/hooks.h"
#include "net/server_link.h"
#include "log.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cstdlib>
#include <map>
#include <string>
#include <windows.h>

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

    typedef void (__attribute__((thiscall)) *UnpackNetworkCarStateFn)(void*, void*, uint32_t, float);
    typedef uint8_t (__attribute__((thiscall)) *UnpackExtrapStateFn)(void*, float, void*, uint32_t, int, float*);
    typedef void (__attribute__((thiscall)) *ExtrapSetTargetFn)(void*, float);

    static uintptr_t g_syncBaseModule = 0;
    static void* g_origUnpackExtrapState = nullptr;
    static bool g_hasIncomingCarId = false;
    static uint32_t g_incomingCarId = 0;

    // Car sync log (test build): why a remote car stops taking the states its owner sends.
    // 0x772420 drops a state when its timestamp isn't newer than the car's current one (+0x1100),
    // when the 32-slot history is full (head +0x11ac would reach tail +0x11b0), or in the first
    // second when the car is over 0.5 m away (+0x11c5 cleared). The log goes to
    // %LOCALAPPDATA%\OpenProStreet\carsync.log.
    struct CarSyncTrack {
        DWORD lastPacket = 0;
        DWORD lastAccepted = 0;
        int dropped = 0;       // dropped in a row
        bool silentLogged = false;
        int lastState = -1;
        int lastBit = -1;
    };
    static std::map<uint32_t, CarSyncTrack> g_carSync;
    static FILE* g_carSyncFile = nullptr;
    static bool g_carSyncTried = false;

    static void CarSyncLog(const char* format, ...)
    {
        if (!g_carSyncTried) {
            g_carSyncTried = true;
            const char* local = getenv("LOCALAPPDATA");
            if (local) {
                std::string dir = std::string(local) + "\\OpenProStreet";
                CreateDirectoryA(dir.c_str(), nullptr);
                g_carSyncFile = fopen((dir + "\\carsync.log").c_str(), "a");
                if (g_carSyncFile) fprintf(g_carSyncFile, "---- session start\n");
            }
        }
        if (!g_carSyncFile) return;
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(g_carSyncFile, "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        va_list args;
        va_start(args, format);
        vfprintf(g_carSyncFile, format, args);
        va_end(args);
        fputc('\n', g_carSyncFile);
        fflush(g_carSyncFile);
    }

    template <typename T> static T Field(void* obj, uintptr_t offset) { return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(obj) + offset); }

    static void CarSyncFields(char* out, size_t size, void* o, uint32_t flags, float timestamp)
    {
        const uintptr_t slot = Field<uintptr_t>(o, 0x11a0);
        snprintf(out, size,
                 "head=%u tail=%u cur.ts=%.3f slot.blend=%.3f state=%d lastFlags=%u bit=%u c3=%u c5=%u resetAt=%.3f clockOff=%.3f | pkt flags=0x%X t=%.3f",
                 Field<uint32_t>(o, 0x11ac), Field<uint32_t>(o, 0x11b0), Field<float>(o, 0x1100),
                 slot ? *reinterpret_cast<float*>(slot + 0x78) : -99.0f, Field<int>(o, 0x118c),
                 Field<uint8_t>(o, 0x11c1), Field<uint8_t>(o, 0x11c2), Field<uint8_t>(o, 0x11c3), Field<uint8_t>(o, 0x11c5),
                 Field<float>(o, 0x11a4), Field<float>(o, 0x11bc), flags, timestamp);
    }

    static void __attribute__((fastcall)) NetCarStateBridge(
        void* carSyncObj, uint32_t carId, void* bitStream, uint32_t flags, float timestamp)
    {
        const DWORD now = GetTickCount();
        CarSyncTrack& track = g_carSync[carId];
        // Cars that went quiet: their owner stopped sending, which the receiver can't fix.
        for (auto& entry : g_carSync) {
            if (entry.first != carId && entry.second.lastPacket && !entry.second.silentLogged && now - entry.second.lastPacket > 2000) {
                entry.second.silentLogged = true;
                CarSyncLog("car %u: no states for %lu ms", entry.first, static_cast<unsigned long>(now - entry.second.lastPacket));
            }
        }
        if (track.lastPacket && now - track.lastPacket > 1000)
            CarSyncLog("car %u: states again after %lu ms without any", carId, static_cast<unsigned long>(now - track.lastPacket));
        track.lastPacket = now;
        track.silentLogged = false;

        const float tsBefore = Field<float>(carSyncObj, 0x1100);
        const uint32_t headBefore = Field<uint32_t>(carSyncObj, 0x11ac);
        char before[400];
        CarSyncFields(before, sizeof(before), carSyncObj, flags, timestamp);

        const bool prevHas = g_hasIncomingCarId;
        const uint32_t prevId = g_incomingCarId;
        g_hasIncomingCarId = true;
        g_incomingCarId = carId;
        reinterpret_cast<UnpackNetworkCarStateFn>(g_syncBaseModule + 0x372420)(carSyncObj, bitStream, flags, timestamp);
        g_hasIncomingCarId = prevHas;
        g_incomingCarId = prevId;

        const float tsAfter = Field<float>(carSyncObj, 0x1100);
        const uint32_t headAfter = Field<uint32_t>(carSyncObj, 0x11ac);
        const bool accepted = tsAfter != tsBefore || headAfter != headBefore;
        char after[400];
        CarSyncFields(after, sizeof(after), carSyncObj, flags, timestamp);

        const int state = Field<int>(carSyncObj, 0x118c);
        const int bit = Field<uint8_t>(carSyncObj, 0x11c2);
        if (state != track.lastState || bit != track.lastBit) {
            CarSyncLog("car %u: state %d -> %d, bit %d -> %d | %s", carId, track.lastState, state, track.lastBit, bit, after);
            track.lastState = state;
            track.lastBit = bit;
        }
        if (flags & 8) CarSyncLog("car %u: reset flag in packet | %s", carId, after);

        if (accepted) {
            if (track.dropped >= 3)
                CarSyncLog("car %u: takes states again after %d dropped | %s", carId, track.dropped, after);
            track.dropped = 0;
            track.lastAccepted = now;
            return;
        }
        ++track.dropped;
        const char* why = Field<uint8_t>(carSyncObj, 0x11c5) == 0 ? "start distance check"
                        : (((headBefore + 1) & 31) == Field<uint32_t>(carSyncObj, 0x11b0) ? "history full"
                        : "not newer than the current state");
        if (track.dropped == 3 || track.dropped % 120 == 0)
            CarSyncLog("car %u: %d states dropped in a row (%s), last taken %lu ms ago | before: %s | after: %s",
                       carId, track.dropped, why, static_cast<unsigned long>(now - track.lastAccepted), before, after);
    }

    static uint8_t __attribute__((thiscall)) UnpackExtrapState_Hook(
        void* thisPtr, float senderTimestamp, void* bitStream, uint32_t flags, int racingState, float* clockOffset)
    {
        const uint8_t ret = reinterpret_cast<UnpackExtrapStateFn>(g_origUnpackExtrapState)(
            thisPtr, senderTimestamp, bitStream, flags, racingState, clockOffset);
        // racingState == 6 (live race): project unpacked state forward by one-way transit lead so remote cars
        // match where the sender actually is now, then restore [this+0x7C] so per-frame updates don't double-step.
        if (ret != 0 && g_hasIncomingCarId && racingState == 6 && thisPtr) {
            const float leadSeconds = ServerLink::GetCarLeadSeconds(g_incomingCarId);
            if (leadSeconds > 0.0f && leadSeconds <= 0.250f) {
                float* const tsPtr = reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(thisPtr) + 0x7C);
                const float origTs = *tsPtr;
                if (std::isfinite(origTs) && origTs > 0.0f && origTs < 86400.0f) {
                    reinterpret_cast<ExtrapSetTargetFn>(g_syncBaseModule + 0x34F340)(thisPtr, origTs + leadSeconds);
                    *tsPtr = origTs;
                }
            }
        }
        return ret;
    }

    static void InstallLatencyCompensation(uintptr_t baseModule)
    {
        g_syncBaseModule = baseModule;

        const uintptr_t callSite = baseModule + 0x3C2420;
        const uint8_t expectedCall[5] = { 0xE8, 0xFB, 0xFF, 0xFA, 0xFF };
        const uintptr_t unpackFn = baseModule + 0x360B30;
        const uint8_t expectedUnpack[7] = { 0x83, 0xEC, 0x48, 0x53, 0x56, 0x8B, 0xF1 };
        const uintptr_t setTargetFn = baseModule + 0x34F340;

        if (IsBadReadPtr(reinterpret_cast<void*>(callSite), sizeof(expectedCall)) ||
            memcmp(reinterpret_cast<void*>(callSite), expectedCall, sizeof(expectedCall)) != 0 ||
            IsBadReadPtr(reinterpret_cast<void*>(unpackFn), sizeof(expectedUnpack)) ||
            memcmp(reinterpret_cast<void*>(unpackFn), expectedUnpack, sizeof(expectedUnpack)) != 0 ||
            IsBadReadPtr(reinterpret_cast<void*>(setTargetFn), 1))
        {
            Log::Warn("[OpenProStreet] Latency compensation target bytes mismatch; skipping.\n");
            return;
        }

        uint8_t* const stub = static_cast<uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!stub) return;

        // Pass ServerCarID ([msg + 0x830], with edi = msg + 0x0C) in edx to __fastcall NetCarStateBridge.
        uint8_t* p = stub;
        *p++ = 0x8B; *p++ = 0x97; *p++ = 0x24; *p++ = 0x08; *p++ = 0x00; *p++ = 0x00; // mov edx, [edi + 0x824]
        *p++ = 0xE9;                                                                   // jmp NetCarStateBridge
        const int32_t jmpRel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&NetCarStateBridge) - (reinterpret_cast<uintptr_t>(p) + 4));
        memcpy(p, &jmpRel, 4);
        p += 4;
        FlushInstructionCache(GetCurrentProcess(), stub, static_cast<SIZE_T>(p - stub));

        if (!Hooks::InstallDetour(reinterpret_cast<void*>(unpackFn), reinterpret_cast<void*>(&UnpackExtrapState_Hook), &g_origUnpackExtrapState, 7)) {
            VirtualFree(stub, 0, MEM_RELEASE);
            Log::Warn("[OpenProStreet] Latency compensation unpack hook failed.\n");
            return;
        }

        uint8_t callPatch[5];
        callPatch[0] = 0xE8;
        const int32_t callRel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(stub) - (callSite + 5));
        memcpy(&callPatch[1], &callRel, 4);
        PatchMemory(callSite, callPatch, sizeof(callPatch));
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(callSite), sizeof(callPatch));
        Log::Debug("[OpenProStreet] Latency compensation enabled (one-way lead extrapolation).\n");
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
        InstallLatencyCompensation(baseModule);
    }
}
