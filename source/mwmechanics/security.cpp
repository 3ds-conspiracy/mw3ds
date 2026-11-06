// Lockpicks and probes: locks and traps
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "components/debug/debuglog.hpp"
#include "mwbase/session.hpp"
#include "mwmechanics/combathelpers.hpp"

InventoryItem* Session::playerToolItem()
{
	for (auto& it : w.inventory)
		if (it.equipped)
			if (const Object* o = w.game.object(it.id))
				if (o->recType == REC_LOCK || o->recType == REC_PROB)
					return &it;
	return nullptr;
}

std::string Session::playerTorchItem()
{
	for (auto& it : w.inventory)
		if (it.equipped)
			if (const Object* o = w.game.object(it.id))
				if (o->recType == REC_LIGH)
					return it.id;
	return "";
}

// Security: a lockpick on a locked door / container, a probe on a trapped one (Morrowind's formula:
// (Security + Agility / 5 + Luck / 10) x quality x fatigue, less the lock level for picks). Each try
// uses the tool up a little; picking someone else's lock where an NPC sees it is trespassing
void Session::useTool(InventoryItem& tool)
{
	const Object* o = w.game.object(tool.id);
	if (!o || target < 0)
		return;
	Ref& r = w.refs[target];
	bool pick = o->recType == REC_LOCK;
	if (r.recType != REC_DOOR && r.recType != REC_CONT)
		return;
	if (pick ? r.lockLevel <= 0 : (r.trap.empty() || r.disarmed))
		return;
	float x;
	if (pick)
		x = lockChance(r.lockLevel, o->quality);
	else
	{
		auto trapSpell = w.game.spells.find(r.trap);
		x = trapChance(trapSpell != w.game.spells.end() ? (float)trapSpell->second.cost : 0.0f, o->quality);
	}
	if (x <= 0.0f)
	{
		notify(w.game.gmst(pick ? "slockimpossible" : "strapimpossible", "Too complex."));
		return;
	}
	// A use of the tool
	if (tool.condition < 0)
		tool.condition = o->uses > 0 ? o->uses : 25;
	bool ok = (float)(persuadeRoll >= 0 ? persuadeRoll : rand() % 100) <= x;      // (tests fix the die)
	logf("security: %s on %s (level %d), chance %d%%, %s", o->id.c_str(), r.id.c_str(), r.lockLevel, (int)x,
		ok ? "done" : "failed");
	if (ok)
	{
		if (pick)
		{
			r.lockLevel = 0;
			playSound(target, "Open Lock");
		}
		else
		{
			r.disarmed = true;
			playSound(target, "Disarm Trap");
		}
		notify(w.game.gmst(pick ? "slocksuccess" : "strapsuccess", "Success!"));
		useSkill(SKILL_SECURITY, pick ? 1 : 0);      // SKDT: 0 disarming a trap, 1 picking a lock
	}
	else
	{
		playSound(target, pick ? "Open Lock Fail" : "Disarm Trap Fail");
		notify(w.game.gmst(pick ? "slockfail" : "strapfail", "Failed."));
	}
	if (pick && w.ownedByOther(target))
	{
		int witness = crimeWitness();
		if (witness >= 0)
			crimeSeen(CRIME_TRESPASS, 0, -1), reportCrime(witness, crimeBounty(CRIME_TRESPASS, 0));
	}
	if (--tool.condition <= 0)
	{
		notify(o->name + " is used up.");
		w.removeItem(tool.id, 1);
	}
}

bool Session::springTrap(int ref)
{
	Ref& r = w.refs[ref];
	if (r.trap.empty() || r.disarmed)
		return false;
	r.disarmed = true;
	auto sp = w.game.spells.find(r.trap);
	logf("trap: %s springs %s", r.id.c_str(), r.trap.c_str());
	if (sp == w.game.spells.end())
		return false;
	playSound(ref, "Disarm Trap Fail");          // (OpenMW's sound for the trap going off)
	// Opened from beyond iMaxActivateDist (Telekinesis' reach): the trap goes off on the object itself, out of the
	// player's way (OpenMW's ActionTrap: the player's distance to the focused object over the activation distance)
	float eye[3] = { w.player.feet[0], w.player.feet[1], playerEyeZ(w.player) };
	float cp = cosf(w.player.pitch);
	float dir[3] = { sinf(w.player.yaw) * cp, cosf(w.player.yaw) * cp, sinf(w.player.pitch) };
	if (w.effectTotal(59) > 0.0f && worldPick(w, eye, dir, w.game.gmstf("imaxactivatedist", 192.0f)) != ref)
	{
		logf("trap: %s went off on itself (opened from beyond reach)", r.id.c_str());
		return true;
	}
	for (auto& e : sp->second.effects)
		applyEffectToPlayer(e, sp->second.name);
	return true;
}
