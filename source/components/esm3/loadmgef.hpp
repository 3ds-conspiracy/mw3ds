#pragma once

// Magic effects (OpenMW's ESM::MagicEffect)
#include <3ds/types.h>
#include <string>

struct MagicEffectDef { int school; float cost; int flags = 0; std::string name; std::string cvfx, bvfx, hvfx, avfx; std::string snd[4]; u32 rgb = 0xFFFFFF; int iconIx = -1; };   // snd: cast, bolt, hit, area (empty: the school's)   // flags: 0x4 no duration, 0x8 no magnitude, 0x10 harmful      // school: 0 alteration, 1 conjuration, 2 destruction,
