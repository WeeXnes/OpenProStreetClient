#ifndef OPENPROSTREET_TWEAKS_H
#define OPENPROSTREET_TWEAKS_H

#include <windows.h>
#include <cstdint>

namespace Tweaks
{
    // Initializes the hidden liveries unlocker and main menu copyright replacer
    void Init(uintptr_t baseModule);
}

#endif // OPENPROSTREET_TWEAKS_H
