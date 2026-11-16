#pragma once

#include <cstdint>

// Graphics improvements that sit between the game and Direct3D.
//
// Sharper textures: the game filters its textures with plain linear filtering, which blurs the road and
// walls at a distance. This turns that into anisotropic filtering as the game sets it.
//
// Replacement shaders: the game keeps its shaders inside nfs.exe and hands each to Direct3D when it
// starts. A shader with a replacement file is swapped as it goes in, so nothing in the game's files
// changes. Replacements are compiled shader files (fxc's /Fo output) in scripts\OpenProStreetShaders,
// named <anything>-<id>.cso, where the id is the 16-character id of the game's shader they replace.
// ProStreetOnline's shaders\tools\split-shaders.js lists every shader in the game with its id.
//
// Settings, under [Graphics] in scripts\OpenProStreetClient.ini:
//   Anisotropy=16   2 to 16; 0 leaves the game's filtering alone
//   Shaders=1       0 ignores the replacement shaders
//   Log=0           1 lists each shader the game creates, with its id, once in the detailed log
namespace Graphics
{
    void Init(uintptr_t baseModule);
}
