#pragma once

#include <cstdint>

namespace Borderless
{
    // With Borderless=1 under [Display] in the settings file, runs the game in a borderless window
    // that fills the primary monitor.
    void Init(uintptr_t baseModule);
}
