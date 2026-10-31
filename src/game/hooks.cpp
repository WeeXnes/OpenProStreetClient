#include "game/hooks.h"

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <vector>

namespace Hooks
{
    bool InstallDetour(void* target, void* detour, void** original, size_t patchSize)
    {
        if (!target || IsBadReadPtr(target, patchSize)) return false;

        DWORD oldProtect = 0;
        if (!VirtualProtect(target, patchSize, PAGE_EXECUTE_READWRITE, &oldProtect)) return false;

        BYTE* trampoline = (BYTE*)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!trampoline) {
            VirtualProtect(target, patchSize, oldProtect, &oldProtect);
            return false;
        }

        memcpy(trampoline, target, patchSize);

        trampoline[patchSize] = 0xE9;
        uintptr_t jmpBackOffset = ((uintptr_t)target + patchSize) - ((uintptr_t)trampoline + patchSize + 5);
        memcpy(&trampoline[patchSize + 1], &jmpBackOffset, 4);

        if (original) {
            *original = (void*)trampoline;
        }

        std::vector<BYTE> jmpBuf(patchSize, 0x90);
        jmpBuf[0] = 0xE9;
        uintptr_t detourOffset = (uintptr_t)detour - ((uintptr_t)target + 5);
        memcpy(&jmpBuf[1], &detourOffset, 4);
        memcpy(target, jmpBuf.data(), patchSize);

        VirtualProtect(target, patchSize, oldProtect, &oldProtect);
        return true;
    }
}
