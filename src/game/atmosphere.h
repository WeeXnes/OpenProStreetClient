#pragma once

#include <cstdint>

// Day, dusk and night, as far as the game allows. ProStreet's lighting is baked into its tracks, and the
// PC game ships day versions only (the engine would load a "_Sunset" copy of each track for time of day
// 1, which only exists for other platforms). What can change is how the game lights the world each
// frame: the sun, ambient, fog, sky and baked-light strength it hands its shaders as effect parameters.
// This tints those as they're set, so shadows stay where the day's sun baked them.
//
// Settings, under [Atmosphere] in scripts\OpenProStreetClient.ini:
//   Enabled=1      hooks the game's effects (on by default)
//   Start=Day      Day, Dusk or Night
//   Key=F8         switches between them in the game
//   Log=0          1 lists every lighting value the game sets, once each, for tuning
// Each preset can be tuned in [Atmosphere.Dusk] and [Atmosphere.Night], one line per effect parameter:
//   cvWorldSunColour=0.2,0.24,0.4      multiplies x, y and z (and w, when a fourth value is given)
// A float parameter uses the first value.
namespace Atmosphere
{
    enum Preset { Day = 0, Dusk = 1, Night = 2 };

    void Init(uintptr_t baseModule);

    // The look right now, and a way to change it, such as from a race day's settings later on.
    Preset Current();
    void Set(Preset preset, bool announce);
}
