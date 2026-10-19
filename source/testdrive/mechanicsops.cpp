// Setup and action tokens (SETSKILL, BUY, CASTAT ...): the parts (ops*.cpp) in order
#include <optional>
#include <3ds.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <queue>
#include <tuple>
#include "testdrive/testdrive.hpp"
#include "mwbase/session.hpp"
#include "components/debug/debuglog.hpp"

extern const char* g_playerBlock;
#include <malloc.h>
#include "testdrive/testhelpers.hpp"
#include "testdrive/opshelpers.hpp"

// Harness operations for the mechanics tests (at once, never blocking). True if the token was one of them
// A spell the test makes: as MAKESPELL, registered as a made spell so a checkpoint carries it (mwstate/statemanagerimp.cpp)
bool makeTestSpell(Session& s, const std::string& name, const std::vector<SpellEffect>& fx)
{
	World& w = s.w;
	SpellDef sp;
	sp.type = 0;
	sp.effects = fx;
	sp.cost = std::max(1, (int)s.madeSpellCost(fx));
	sp.id = "mw3ds_test_" + lower(name);
	sp.name = name;
	w.game.spells[sp.id] = sp;
	if (std::find(w.stats.spells.begin(), w.stats.spells.end(), sp.id) == w.stats.spells.end())
		w.stats.spells.push_back(sp.id);
	if (std::find(w.madeSpells.begin(), w.madeSpells.end(), sp.id) == w.madeSpells.end())
		w.madeSpells.push_back(sp.id);
	logf("test: spell %s, %zu effects, cost %d", sp.id.c_str(), sp.effects.size(), sp.cost);
	return true;
}

// KILL:<id>:unseen, v2 kit: the spot 2600 to 4200 away, on a floor near our height, farthest from every other living NPC
// loaded (ties: the nearer); false when even the best has someone within fAlarmRadius and a margin
bool leadSpot(Session& s, int victim, float* out)
{
	World& w = s.w;
	const Player& p = w.player;
	Scene sc = w.scene();
	float radius = w.game.gmstf("falarmradius", 2000.0f), best = -1.0f;
	for (float rad = 2600.0f; rad <= 4200.0f; rad += 800.0f)
		for (int k = 0; k < 24; k++)
		{
			float ang = 6.2831853f * k / 24.0f, cx = p.feet[0] + sinf(ang) * rad, cy = p.feet[1] + cosf(ang) * rad, fz = -1e9f;
			for (Cell* c : sc.cells)
			{
				float z;
				if (collisionFloor(c->collision, cx, cy, p.feet[2] + 600.0f, p.feet[2] - 600.0f, &z) && z > fz)
					fz = z;
			}
			if (fz < -1e8f || fz < 0.0f)
				continue;
			float nearest = 1e9f;
			for (int i : w.loadedActors)
			{
				const Ref& o = w.refs[i];
				if (i != victim && o.type == "NPC_" && !o.dead && !o.ally && w.active(i))
					nearest = fminf(nearest, hypotf(o.pos[0] - cx, o.pos[1] - cy));
			}
			if (nearest > best + 1.0f)
			{
				best = nearest;
				out[0] = cx;
				out[1] = cy;
				out[2] = fz;
			}
		}
	return best > radius + 300.0f;
}

bool makeTestItem(Session& s, const std::string& base, const std::string& id, int type, int charge,
	const std::vector<SpellEffect>& fx)
{
	World& w = s.w;
	const Object* bo = w.game.object(base);
	if (!bo)
		bo = w.game.object(spaced(base));
	if (!bo)
		return false;
	SpellDef en;
	en.type = type;                 // ENCH_USE: cast when used (effects become active effects: a Fortify goes past 100)
	en.cost = 1;                    // a use costs 1 of the charge: with 999999 as good as endless
	en.charge = charge;
	en.effects = fx;
	en.id = "mw3ds_ench_" + lower(id);
	en.name = spaced(id);
	Object item = *bo;
	item.id = lower(id);
	item.iconBase = bo->iconBase.empty() ? Atom(bo->id) : bo->iconBase;   // (the save rebuilds the item from this base)
	item.ench = en.id;
	item.magic = true;
	item.name = spaced(id);
	item.script.clear();
	w.game.spells[en.id] = en;
	w.game.objects[item.id] = item;
	if (std::find(w.madeItems.begin(), w.madeItems.end(), item.id) == w.madeItems.end())
		w.madeItems.push_back(item.id);
	w.addItem(item.id, 1);
	logf("test: item %s from %s (type %d), %zu effects, charge %d", item.id.c_str(), bo->id.c_str(), type, fx.size(), charge);
	return true;
}

bool mechanicsOp(Session& s, const std::vector<std::string>& a)
{
	for (auto part : { opsStats, opsPose, opsItems, opsWorld, opsSystem })
		if (std::optional<bool> r = part(s, a))
			return *r;
	return false;
}
