// The player's body seen from outside: see playerbody.h.
#include "playerbody.h"

#include <cmath>
#include <cstring>

#include "linear.h"
#include "log.h"
#include "renderer.h"
#include "world.h"

namespace
{
// Body part slots (tools/npc.py PART_BONES order)
const int kSlotCount = 27;
const int SLOT_HEAD = 0, SLOT_HAIR = 1, SLOT_SHIELD = 10, SLOT_WEAPON = 25;
const int CLOT_ROBE = 4, CLOT_SKIRT = 7;
const int kRobeHides[] = { 3, 4, 5, 11, 12, 13, 14, 19, 20, 21, 22 };
const int kSkirtHides[] = { 4, 21, 22 };
}

const std::vector<std::pair<std::string, std::string>>* playerHeadChoices(const World& w, bool hair)
{
	const ThirdPersonDef& t = w.game.firstPerson.third;
	int female = w.stats.female ? 1 : 0;
	auto& map = (hair ? t.hairs : t.heads)[female];
	auto it = map.find(lower(w.stats.race));
	return it != map.end() && !it->second.empty() ? &it->second : nullptr;
}

void PlayerBody::rebuild(World& w, bool weaponOut)
{
	const ThirdPersonDef& t = w.game.firstPerson.third;
	std::string race = lower(w.stats.race);
	int female = w.stats.female ? 1 : 0;
	// The skeleton: the one NPCs of this race and sex use
	if (!ready || race != forRace || (female != 0) != forFemale)
	{
		free(w);
		skel = -1;
		for (auto& a : w.game.actors)
			if (!a.creature && lower(a.race) == race && (a.female != 0) == (female != 0) && a.skel >= 0)
			{
				skel = a.skel;
				break;
			}
		if (skel < 0)
			for (auto& a : w.game.actors)
				if (!a.creature && a.skel >= 0)
				{
					skel = a.skel;
					break;
				}
		if (skel < 0)
			return;
		skeletonEnsure(skel);
		set.actors.resize(1);
		Actor& a = set.actors[0];
		a = Actor();
		a.ref = -1;
		a.skeleton = skel;
		a.pose.assign(g_sharedSkeletons[skel].bones.size() * 12, 0.0f);
		a.place[0] = a.place[5] = a.place[10] = 1.0f;
		forRace = race;
		forFemale = female != 0;
		ready = true;
		builtFor.clear();
	}
	// What goes in each slot: the bare body, head and hair, then what's worn over it
	std::string part[kSlotCount];
	u32 glow[kSlotCount] = {};        // enchanted items glow
	for (int sex : { female, 0 })
	{
		auto it = t.races[sex].find(race);
		if (it == t.races[sex].end())
			continue;
		for (auto& s : it->second)
			if (s.first >= 0 && s.first < kSlotCount && part[s.first].empty())
				part[s.first] = s.second;
	}
	auto chosen = [&](bool hair) {
		const auto* list = playerHeadChoices(w, hair);
		if (!list)
			return std::string();
		const std::string& want = hair ? w.stats.hair : w.stats.head;
		for (auto& e : *list)
			if (e.first == want)
				return e.second;
		return list->front().second;
	};
	part[SLOT_HEAD] = chosen(false);
	part[SLOT_HAIR] = chosen(true);
	// A helmet takes the hair away (a coif or hood with a hair part of its own sets it back below)
	for (auto& it : w.inventory)
		if (it.equipped)
			if (const Object* o = w.game.object(it.id))
				if (o->type == "ARMO" && o->subtype == 0)
					part[SLOT_HAIR].clear();
	for (int layer = 0; layer < 4; layer++)
		for (auto& it : w.inventory)
		{
			if (!it.equipped)
				continue;
			const Object* o = w.game.object(it.id);
			if (!o || (o->type != "CLOT" && o->type != "ARMO"))
				continue;
			int l = o->type == "ARMO" ? 1 : o->subtype == CLOT_SKIRT ? 2 : o->subtype == CLOT_ROBE ? 3 : 0;
			if (l != layer || (o->type == "ARMO" && o->subtype == 8 && !weaponOut))
				continue;                 // a shield shows while fighting
			if (o->type == "CLOT" && o->subtype == CLOT_ROBE)
				for (int s : kRobeHides)
					part[s].clear();
			if (o->type == "CLOT" && o->subtype == CLOT_SKIRT)
				for (int s : kSkirtHides)
					part[s].clear();
			auto fit = t.items.find(lower(it.id));
			if (fit == t.items.end())
				continue;
			for (auto& s : fit->second)
				if (s.first >= 0 && s.first < kSlotCount && s.first != SLOT_WEAPON)
				{
					part[s.first] = female && !s.second.second.empty() ? s.second.second : s.second.first;
					glow[s.first] = w.game.enchantGlow(it.id);
				}
		}
	// The weapon while it is out (the first-person piece of it rides the weapon bone either way)
	if (weaponOut)
		for (auto& it : w.inventory)
			if (it.equipped)
				if (const Object* o = w.game.object(it.id))
					if (o->type == "WEAP")
					{
						auto fit = w.game.firstPerson.items.find(lower(it.id));
						if (fit != w.game.firstPerson.items.end() && fit->second.slots.count(SLOT_WEAPON))
						{
							part[SLOT_WEAPON] = fit->second.slots.at(SLOT_WEAPON).first;
							glow[SLOT_WEAPON] = w.game.enchantGlow(it.id);
						}
						break;
					}
	std::string key;
	for (int s = 0; s < kSlotCount; s++)
		key += part[s] + (glow[s] ? "*|" : "|");
	if (key == builtFor)
		return;
	builtFor = key;
	Actor& a = set.actors[0];
	a.meshes.clear();
	textures.clear();
	// Pieces no longer worn go first (each face or hair tried on the race screen stayed loaded: ~100 KB of
	// linear memory a time, until a new face couldn't load on the 3DS and vanished), so the new one fits
	for (auto it = pieces.begin(); it != pieces.end();)
	{
		bool used = false;
		for (int s = 0; s < kSlotCount && !used; s++)
			used = part[s] == it->first;
		if (used)
		{
			++it;
			continue;
		}
		fpFreePiece(w, it->second);
		it = pieces.erase(it);
	}
	const Skeleton& sk = g_sharedSkeletons[skel];
	for (int s = 0; s < kSlotCount; s++)
	{
		if (part[s].empty())
			continue;
		auto pit = pieces.find(part[s]);
		FpPiece* p = pit != pieces.end() ? pit->second : fpLoadPiece(w, sk, part[s]);
		if (!p)
			continue;
		if (pit == pieces.end())
			pieces[part[s]] = p;
		fpRetryTextures(w, p);
		for (auto& m : p->meshes)
		{
			ActorMesh copy = m;
			copy.tex = textures.size();
			copy.glow = glow[s];
			textures.push_back(p->textures[m.tex]);
			a.meshes.push_back(copy);
		}
	}
	a.deformed = false;
	logf("playerbody: %d meshes on skeleton %d, %d pieces kept", (int)a.meshes.size(), skel, (int)pieces.size());
}

void PlayerBody::update(World& w, float dt, float speed, bool swimming, bool sneaking, bool weaponOut)
{
	if (!ready || set.actors.empty())
		return;
	Actor& a = set.actors[0];
	// What they are doing
	const char* want = swimming ? (speed > 10.0f ? "SwimForward" : "Idle")
		: sneaking ? (speed > 10.0f ? "SneakForward" : "Idle")
		: speed > 200.0f ? "RunForward" : speed > 10.0f ? "WalkForward" : "Idle";
	if (strcmp(want, group) != 0)
	{
		if (actorPlay(set, a, want, strcmp(want, "Idle") == 0 ? ANIM_IDLE : ANIM_LOOP) || strcmp(want, "Idle") == 0)
			group = want;
		else if (actorPlay(set, a, "WalkForward", ANIM_LOOP))
			group = "WalkForward";
	}
	actorAnimate(set, a, dt);
	// Place: at the feet, turned to the view, the race's size
	const Player& p = w.player;
	float c = cosf(-p.yaw), s = sinf(-p.yaw);
	float place[12] = { c, -s, 0, p.feet[0], s, c, 0, p.feet[1], 0, 0, 1, p.feet[2] };
	memcpy(a.place, place, sizeof(place));
	(void)weaponOut;
}

void PlayerBody::draw(World& w, bool deform)
{
	(void)w;
	if (!ready || set.actors.empty() || set.actors[0].meshes.empty())
		return;
	Actor& a = set.actors[0];
	if (deform || !a.deformed)
	{
		actorDeform(set, a);
		a.deformed = true;
	}
	rendererDrawActor(a, textures, false);
}

void PlayerBody::free(World& w)
{
	for (auto& p : pieces)
		fpFreePiece(w, p.second);
	pieces.clear();
	if (!set.actors.empty())
		set.actors[0].meshes.clear();
	textures.clear();
	builtFor.clear();
	ready = false;
}
