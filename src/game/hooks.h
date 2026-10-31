#pragma once

#include <cstddef>

namespace Hooks
{
    // Jumps from target to detour. The first patchSize bytes of target must be whole instructions
    // that don't depend on where they run, because they move to a trampoline. original receives
    // the trampoline, which continues the function.
    bool InstallDetour(void* target, void* detour, void** original, size_t patchSize = 7);
}
