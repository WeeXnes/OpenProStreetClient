#pragma once

#include <cstdint>

namespace QuickMatch
{
    // Sends "Schnelles Spiel" to the OpenProStreet server through the LAN flow, then joins the
    // room the server lists first.
    void Init(uintptr_t baseModule);
}
