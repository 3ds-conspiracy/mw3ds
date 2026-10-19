// Script functions: the player's controls, and forced sneak / run / jump
#include "components/interpreter/interpreter.hpp"

#include <optional>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_set>

#include "mwworld/esmstore.hpp"
#include "mwrender/animation.hpp"
#include "components/debug/debuglog.hpp"
#include "mwworld/worldimp.hpp"
#include "components/interpreter/interpreterhelpers.hpp"

std::optional<float> controlExtensions(Ctx& c, const Node& n, World& w, bool onPlayer, int ref, Ref* r)
{
	switch (n.func)
	{
	// Force flags are the referenced actor's own (the player's, with no reference): only the player's sneak acts
	case F_FORCESNEAK: case F_CLEARFORCESNEAK: case F_GETFORCESNEAK:
	case F_FORCERUN: case F_CLEARFORCERUN: case F_GETFORCERUN:
	case F_FORCEJUMP: case F_CLEARFORCEJUMP: case F_GETFORCEJUMP:
	case F_FORCEMOVEJUMP: case F_CLEARFORCEMOVEJUMP: case F_GETFORCEMOVEJUMP:
	{
		int f = n.func;
		unsigned bit = f >= F_FORCEMOVEJUMP ? 4 : f >= F_FORCEJUMP ? 2 : f >= F_FORCERUN ? 1 : 8;
		if (f == F_FORCESNEAK || f == F_CLEARFORCESNEAK || f == F_GETFORCESNEAK)
			bit = 8;
		bool get = f == F_GETFORCESNEAK || f == F_GETFORCERUN || f == F_GETFORCEJUMP || f == F_GETFORCEMOVEJUMP;
		bool clear = f == F_CLEARFORCESNEAK || f == F_CLEARFORCERUN || f == F_CLEARFORCEJUMP || f == F_CLEARFORCEMOVEJUMP;
		int key = onPlayer || !r ? -1 : ref;
		unsigned& bits = w.moveFlags[key];
		if (get)
			return (bits & bit) ? 1.0f : 0.0f;
		bits = clear ? bits & ~bit : bits | bit;
		if (key < 0 && bit == 8)
			w.forceSneak = !clear;
		return 0.0f;
	}
	case F_DISABLEPLAYERCONTROLS: w.controlsEnabled = false; return 0.0f;
	case F_ENABLEPLAYERCONTROLS: w.controlsEnabled = true; return 0.0f;
	case F_DISABLEPLAYERJUMPING: w.jumpingEnabled = false; return 0.0f;
	case F_ENABLEPLAYERJUMPING: w.jumpingEnabled = true; return 0.0f;
	case F_ENABLEPLAYERFIGHTING: w.fightingEnabled = true; return 0.0f;
	case F_DISABLEPLAYERFIGHTING: w.fightingEnabled = false; return 0.0f;
	case F_ENABLEPLAYERMAGIC: w.magicEnabled = true; return 0.0f;
	case F_DISABLEPLAYERMAGIC: w.magicEnabled = false; return 0.0f;
	// Looking, view switching and vanity mode: kept so the Get...Disabled questions answer (no third person here)
	case F_ENABLEPLAYERLOOKING: w.controlsOff &= ~1u; return 0.0f;
	case F_DISABLEPLAYERLOOKING: w.controlsOff |= 1u; return 0.0f;
	case F_ENABLEPLAYERVIEWSWITCH: w.controlsOff &= ~2u; return 0.0f;
	case F_DISABLEPLAYERVIEWSWITCH: w.controlsOff |= 2u; return 0.0f;
	case F_ENABLEVANITYMODE: w.controlsOff &= ~4u; return 0.0f;
	case F_DISABLEVANITYMODE: w.controlsOff |= 4u; return 0.0f;
	case F_GETPLAYERCONTROLSDISABLED: return w.controlsEnabled ? 0.0f : 1.0f;
	case F_GETPLAYERFIGHTINGDISABLED: return w.fightingEnabled ? 0.0f : 1.0f;
	case F_GETPLAYERJUMPINGDISABLED: return w.jumpingEnabled ? 0.0f : 1.0f;
	case F_GETPLAYERLOOKINGDISABLED: return w.controlsOff & 1u ? 1.0f : 0.0f;
	case F_GETPLAYERMAGICDISABLED: return w.magicEnabled ? 0.0f : 1.0f;
	case F_GETPLAYERVIEWSWITCHDISABLED: return w.controlsOff & 2u ? 1.0f : 0.0f;
	case F_GETVANITYMODEDISABLED: return w.controlsOff & 4u ? 1.0f : 0.0f;
	default:
		return std::nullopt;
	}
}
