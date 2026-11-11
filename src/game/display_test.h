#pragma once

#include <cstdint>

// A test of how the game's frames reach the screen, to find out why an overlay drawn on them didn't
// show. With [Debug] DisplayTest=1 it draws three squares in the top left corner of every frame,
// each a different way, and says on the console every 10 seconds what the game presented:
//   red:   cleared on the back buffer just before Present
//   blue:  drawn as triangles on the back buffer just before Present, as an overlay would
//   green: cleared on whatever the game draws to, at each EndScene
// Which squares show, together with the console lines, says where an overlay has to draw.
namespace DisplayTest
{
    void Init(uintptr_t baseModule);
}
