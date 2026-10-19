// Script functions: PlayGroup, LoopGroup
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

std::optional<float> animationExtensions(Ctx& c, const Node& n, World& w, bool onPlayer, int ref, Ref* r)
{
	switch (n.func)
	{
	// Animation groups on actors: once (then back to idling) or looping (LoopGroup group count: that many
	// more times round)
	case F_PLAYGROUP: case F_LOOPGROUP:
		// (an animated object, an activator: its own animation, the world's visual of it)
		if (r && r->enabled && !w.actorOf(ref))
			c.host.playObjectGroup(ref, lower(argStr(c, n, 0)), n.func == F_LOOPGROUP);
		if (Actor* a = r && r->enabled ? w.actorOf(ref) : nullptr)
			if (actorPlay(*w.actorsOf(ref), *a, argStr(c, n, 0).c_str(), n.func == F_LOOPGROUP ? ANIM_LOOP : ANIM_ONCE)
				&& n.func == F_LOOPGROUP)
				a->loopsLeft = (s8)std::max(0, std::min(120, (int)argNum(c, n, 1)));
		return 0.0f;
	default:
		return std::nullopt;
	}
}
