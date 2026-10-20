#pragma once

// The effects of a spell, potion, ingredient or enchantment (OpenMW's ESM::EffectList)

// ---- Records

// range: 0 self, 1 touch, 2 target
struct SpellEffect { int effect, skill, attribute, min, max, duration, range; int area = 0; };   // area: feet around where it lands
