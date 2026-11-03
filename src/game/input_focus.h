#ifndef OPENPROSTREET_INPUT_FOCUS_H
#define OPENPROSTREET_INPUT_FOCUS_H

#include <windows.h>
#include <cstdint>

// The game keeps running when it loses focus, so it can stay online and, in borderless mode, keep
// drawing. That also kept it reading the keyboard and mouse. This hides their input from the game
// while another window is in front. Gamepads are left alone.
namespace InputFocus
{
    void Init(uintptr_t baseModule);
}

#endif // OPENPROSTREET_INPUT_FOCUS_H
