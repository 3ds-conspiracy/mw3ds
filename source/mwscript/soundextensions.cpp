// Script functions: sounds, voices, music
#include "components/interpreter/interpreter.hpp"

#include <optional>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_set>

#include "mwworld/worldimp.hpp"
#include "components/interpreter/interpreterhelpers.hpp"

std::optional<float> soundExtensions(Ctx& c, const Node& n, World& w, bool onPlayer, int ref, Ref* r)
{
	switch (n.func)
	{
	case F_PLAYLOOPSOUND3D: case F_PLAYLOOPSOUND3DVP:
		c.host.loopSound(ref, argStr(c, n, 0), true, argNum(c, n, 1, 1.0f), argNum(c, n, 2, 1.0f));
		return 0.0f;
	case F_SAY: c.host.say(ref, argStr(c, n, 0), argStr(c, n, 1)); return 0.0f;
	case F_SAYDONE: return c.host.sayDone(ref) ? 1.0f : 0.0f;
	case F_PLAYSOUND: case F_PLAYSOUNDVP:
		c.host.playSound(-1, argStr(c, n, 0), argNum(c, n, 1, 1.0f), argNum(c, n, 2, 1.0f));
		return 0.0f;
	case F_PLAYSOUND3D: case F_PLAYSOUND3DVP:
		c.host.playSound(ref, argStr(c, n, 0), argNum(c, n, 1, 1.0f), argNum(c, n, 2, 1.0f), true);
		return 0.0f;
	case F_STOPSOUND: c.host.loopSound(ref, argStr(c, n, 0), false); return 0.0f;
	case F_STREAMMUSIC:
		c.host.streamMusic(argStr(c, n, 0));
		return 0.0f;
	case F_GETSOUNDPLAYING:
		return c.host.soundPlaying(ref, argStr(c, n, 0)) ? 1.0f : 0.0f;
	default:
		return std::nullopt;
	}
}
