// Summoned creatures: their time running out, and released
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "mwsound/soundmanagerimp.hpp"
#include "components/debug/debuglog.hpp"
#include "mwbase/session.hpp"
#include "mwdialogue/dialoguemanagerimp.hpp"
#include "mwmechanics/combathelpers.hpp"

// The summoned creature comes in a flash beside its caster and goes for the player, for the spell's duration; a ward's
// effects land on the caster, with their hit
void Session::npcSummonRelease(int ri, const SpellDef& sp)
{
	const Ref& r = w.refs[ri];
	bool self = false;
	for (const SpellEffect& e : sp.effects)
		if (e.range == 0 && !summonCreatureId(e.effect))
		{
			applyEffectToActor(ri, e, false, (float)sp.cost, 100.0f, sp.name);
			self = true;
		}
	if (self)
	{
		spellVfx(sp, 1, r.pos, ri, r.rot[2]);
		logf("combat: %s wards itself with %s", r.id.c_str(), sp.name.c_str());
	}
	for (const SpellEffect& e : sp.effects)
	{
		const char* id = summonCreatureId(e.effect);
		if (!id)
			continue;
		float pos[3] = { r.pos[0] + sinf(r.rot[2]) * 128.0f, r.pos[1] + cosf(r.rot[2]) * 128.0f, r.pos[2] };
		int ci = w.spawnActor(id, pos, r.rot[2]);
		if (ci < 0)
			return;
		makeHostile(ci);
		w.refs[ci].aggressor = true;                     // (killing it is no crime)
		npcSummons.push_back({ ri, ci, w.time + (float)(e.duration > 0 ? e.duration : 30) });
		spawnVfx("vfx_summon_start", w.refs[ci].pos, 1.2f, ci, w.refs[ci].rot[2]);
		logf("combat: %s summons %s for %d s", r.id.c_str(), id, e.duration);
		return;
	}
}

// NPCs' summons go when their time is up, when their caster dies or leaves (OpenMW: CasterLinked), or a moment after
// they fall (their corpse isn't kept: OpenMW disposes of summoned corpses)
void Session::npcSummonsUpdate()
{
	for (size_t k = 0; k < npcSummons.size();)
	{
		NpcSummon& n = npcSummons[k];
		const bool valid = n.creature >= 0 && n.creature < (int)w.refs.size() && n.caster >= 0 && n.caster < (int)w.refs.size();
		const Ref* c = valid ? &w.refs[n.creature] : nullptr;
		const bool casterGone = !valid || w.refs[n.caster].dead || !w.active(n.caster);
		if (c && c->dead && n.ends > w.time + 3.0f)
			n.ends = w.time + 3.0f;                       // (fallen: gone shortly)
		if (!valid || w.time >= n.ends || casterGone)
		{
			if (c && c->visible() && !c->dead)
				spawnVfx("vfx_summon_end", c->pos, 1.2f, -2, c->rot[2]);
			if (valid)
				w.despawn(n.creature);
			npcSummons.erase(npcSummons.begin() + k);
		}
		else
			k++;
	}
}
