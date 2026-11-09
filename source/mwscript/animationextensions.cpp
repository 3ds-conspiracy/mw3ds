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

// The groups Morrowind always plays round and round, whatever their keys (OpenMW's Animation::isLoopingAnimation)
static bool loopingGroup(const std::string& g)
{
	static const std::unordered_set<std::string> kLooping = { "walkforward", "walkback", "walkleft", "walkright",
		"swimwalkforward", "swimwalkback", "swimwalkleft", "swimwalkright", "runforward", "runback", "runleft",
		"runright", "swimrunforward", "swimrunback", "swimrunleft", "swimrunright", "sneakforward", "sneakback",
		"sneakleft", "sneakright", "turnleft", "turnright", "swimturnleft", "swimturnright", "spellturnleft",
		"spellturnright", "torch", "idle", "idle2", "idle3", "idle4", "idle5", "idle6", "idle7", "idle8", "idle9",
		"idlesneak", "idlestorm", "idleswim", "jump", "inventoryhandtohand", "inventoryweapononehand",
		"inventoryweapontwohand", "inventoryweapontwowide" };
	return kLooping.count(g) > 0;
}

std::optional<float> animationExtensions(Ctx& c, const Node& n, World& w, bool onPlayer, int ref, Ref* r)
{
	switch (n.func)
	{
	// Animation groups on actors (OpenMW's CharacterController::playGroup and updateAnimQueue). LoopGroup group count:
	// that many more times round. PlayGroup plays with no end to its count: a looping group (the idles, walking ...:
	// Animation::isLoopingAnimation) goes round for ever, any other plays once and holds its last frame, until the next
	// group; PlayGroup Idle is the way back to idling. It played once and went back to idling: the Heart of Lorkhan,
	// struck, was whole and still again moments later, after its Death1 too
	case F_PLAYGROUP: case F_LOOPGROUP:
	{
		// (an animated object, an activator: its own animation, the world's visual of it)
		if (r && r->enabled && !w.actorOf(ref))
			c.host.playObjectGroup(ref, lower(argStr(c, n, 0)), n.func == F_LOOPGROUP);
		Actor* a = r && r->enabled ? w.actorOf(ref) : nullptr;
		if (!a)
			return 0.0f;
		std::string group = lower(argStr(c, n, 0));
		bool loop = n.func == F_LOOPGROUP;
		ActorSet& set = *w.actorsOf(ref);
		const Skeleton& sk = actorSkeleton(set, a->skeleton);
		std::string asked = argStr(c, n, 0);
		const char* name = !loop && group == "idle" ? "Idle" : asked.c_str();
		int g = actorFindGroup(sk, name);
		AnimMode play = loop ? ANIM_LOOP : group == "idle" ? ANIM_IDLE : loopingGroup(group) ? ANIM_LOOP : ANIM_HOLD;
		s8 loops = loop ? (s8)std::max(0, std::min(120, (int)argNum(c, n, 1))) : -1;
		// A looping group asked for again while a script plays it, short of its Loop Stop, goes on as it is with its
		// count, and nothing waits behind it (OpenMW's playGroup: the banners of OutsideBanner)
		if (g >= 0 && a->scripted && g == a->group && loopingGroup(group) && a->time < sk.groups[g].loopStop
			&& actorGroupPlaying(sk, *a))
		{
			a->queued = -1;
			return 0.0f;
		}
		// The mode, after the count: 0, the default, waits while a script's group still plays, which goes round no more
		// and plays on to its Stop before this one starts (actorAnimate); 1 plays at once, 2 at once from its Loop Start
		// (OpenMW's playGroup and updateAnimQueue). Azura, the ending's movie over: AzuraEnd's PlayGroup Idle, 0 lets her
		// Idle2 play out, her parting gesture, before she idles. She went to Idle at once
		int mode = (int)argNum(c, n, loop ? 2 : 1);
		if (mode == 0 && g >= 0 && a->scripted && actorGroupPlaying(sk, *a))
		{
			a->queued = g;
			a->queuedMode = play;
			a->queuedLoops = loops;
			return 0.0f;
		}
		// (a script's group takes over from the one before, scripted or not; nothing else starts over it)
		// (a group the model hasn't changes nothing)
		if (g < 0)
			return 0.0f;
		a->queued = -1;
		a->scripted = false;
		if (play == ANIM_IDLE)
			actorPlay(set, *a, "Idle", ANIM_IDLE);
		else
		{
			if (g == a->group && !a->frozen)
				a->mode = ANIM_ONCE;           // (the same group again: from its start)
			a->scripted = actorPlay(set, *a, name, play);
			if (a->scripted && loop)
				a->loopsLeft = loops;
			if (mode == 2 && a->group == g)
				a->time = sk.groups[g].loopStart;
		}
		return 0.0f;
	}
	default:
		return std::nullopt;
	}
}
