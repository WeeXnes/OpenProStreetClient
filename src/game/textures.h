#pragma once

#include <cstdint>

// Texture packs: swaps the game's textures for replacement files as the game loads them.
//
// A texture is known by the id TexMod gave it, a checksum of its largest image, so textures taken from
// TexMod packs (.tpf) fit. The launcher unpacks a pack into scripts\OpenProStreetTextures\<pack>, one
// file per texture, named <8-character id>.dds. Folders are read in order of their names, and a later
// folder's texture wins over an earlier one's. Files straight in OpenProStreetTextures win over all.
//
// Settings, under [Textures] in scripts\OpenProStreetClient.ini:
//   Enabled=1   0 turns texture packs off
//   Log=0       1 lists each texture the game loads, with its id, once
namespace Textures
{
    void Init(uintptr_t baseModule);
}
