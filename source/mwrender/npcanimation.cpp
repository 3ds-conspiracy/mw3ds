// The player's body seen from outside: see mwrender/npcanimation.hpp.
#include "mwrender/npcanimation.hpp"

#include <cmath>
#include <cstring>

#include "platform/linear.hpp"
#include "components/debug/debuglog.hpp"
#include <algorithm>
#include <array>
#include <functional>
#include <sys/stat.h>
#include <tex3ds.h>
#include <citro2d.h>
#include "mwbase/session.hpp"
#include <cstdlib>
#include <malloc.h>

namespace
{
// Body part slots (tools/convert/npc.py PART_BONES order)
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
			if (l != layer)
				continue;                 // (a shield shows on the arm, weapon out or not: OpenMW's updateCarriedLeftVisible)
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
	// The weapon while it is out (the first-person piece of it rides the weapon bone either way). Equipped arrows and
	// bolts are ammo, not the weapon (Session::playerWeaponItem): arrows ahead of a sword in the inventory put the arrow
	// in the fist, a thin stick with its head out at the tip
	if (weaponOut)
		for (auto& it : w.inventory)
			if (it.equipped)
				if (const Object* o = w.game.object(it.id))
					if (o->type == "WEAP" && o->subtype < WEAP_ARROW)
					{
						auto fit = w.game.firstPerson.items.find(lower(it.id));
						if (fit != w.game.firstPerson.items.end() && fit->second.slots.count(SLOT_WEAPON))
						{
							part[SLOT_WEAPON] = fit->second.slots.at(SLOT_WEAPON).first;
							glow[SLOT_WEAPON] = w.game.enchantGlow(it.id);
						}
						break;
					}
	weapon = part[SLOT_WEAPON];
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
			if (s == SLOT_SHIELD)
				copy.hand = 2;            // put away while both hands are busy (Actor::bothHands)
			textures.push_back(p->textures[m.tex]);
			a.meshes.push_back(copy);
		}
	}
	a.deformed = false;
	logf("playerbody: %d meshes on skeleton %d, %d pieces kept", (int)a.meshes.size(), skel, (int)pieces.size());
}

// Where a blow's wind-up holds, as a share of the way from the group's start to its hit: the min and max attack marks
// of base_anim.nif's chop / shoot text keys (the exported group keeps only start, hit and stop). The wind-up plays to
// the min mark, then on toward the max mark as the blow is drawn back (OpenMW's character controller)
static void windupMarks(const char* group, float& lo, float& hi)
{
	static const struct { const char* name; float lo, hi; } kMarks[] = {
		{ "Attack1h", 0.17f, 0.5f }, { "AttackHH", 0.2f, 0.5f }, { "Attack2c", 0.25f, 0.66f }, { "Attack2w", 0.22f, 0.45f },
		{ "AttackBow", 0.64f, 0.92f }, { "AttackXbow", 0.6f, 0.6f }, { "AttackThrow", 0.39f, 0.61f } };
	lo = 0.2f;
	hi = 0.5f;
	for (auto& m : kMarks)
		if (strncmp(group, m.name, strlen(m.name)) == 0)      // (and its slash / thrust: Attack1hSl, Attack1hTh)
		{
			lo = m.lo;
			hi = m.hi;
		}
}

void PlayerBody::update(World& w, float dt, float speed, bool swimming, bool sneaking, bool weaponOut, const BodyAct& act)
{
	if (!ready || set.actors.empty())
		return;
	Actor& a = set.actors[0];
	const Skeleton& sk = actorSkeleton(set, a.skeleton);
	// Place: at the feet, turned to the view, the race's size
	const Player& p = w.player;
	float c = cosf(-p.yaw), s = sinf(-p.yaw);
	float place[12] = { c, -s, 0, p.feet[0], s, c, 0, p.feet[1], 0, 0, 1, p.feet[2] };
	memcpy(a.place, place, sizeof(place));
	// held in a pose for a side-by-side screenshot (the test driver's BODYPOSE): nothing moves on
	if (a.frozen)
	{
		actorAnimate(set, a, 0.0f);
		return;
	}
	// A new blow or cast takes the whole body (one group at a time here: no upper-body layer); a blow let go plays on
	// from where its wind-up held
	if (act.action != BODY_NONE && (act.action != acting || actGroup != act.group))
	{
		bool letGo = acting == BODY_WINDUP && act.action == BODY_STRIKE && actGroup == act.group && a.mode == ANIM_HOLD;
		if (letGo)
			a.mode = ANIM_ONCE;
		else if (!actorPlay(set, a, act.group, act.action == BODY_WINDUP || act.action == BODY_DEATH ? ANIM_HOLD : ANIM_ONCE))
			logf("playerbody: no %s group", act.group);
		else
		{
			// from the start (a blow seen first at its release, the view changed mid-blow: from the max attack mark)
			float lo, hi;
			windupMarks(act.group, lo, hi);
			const AnimGroup& g = sk.groups[a.group];
			a.time = act.action == BODY_STRIKE ? g.start + (g.loopStart - g.start) * hi : g.start;
		}
		acting = act.action;
		actGroup = act.group;
		followCut = false;
		group = "";
	}
	else if (act.action == BODY_NONE && acting == BODY_WINDUP)
	{
		acting = BODY_NONE;                     // the wind-up was cut short (a menu, a knockdown)
		group = "";
	}
	else if (act.action == BODY_NONE)
		acting = BODY_NONE;                     // a blow or cast already going plays out (ANIM_ONCE)
	bool busy = a.mode == ANIM_ONCE || (a.mode == ANIM_HOLD && (acting == BODY_WINDUP || acting == BODY_DEATH));
	float step = dt;
	if (busy && a.mode == ANIM_HOLD && acting == BODY_WINDUP)
	{
		// Drawn back: at the weapon's speed toward the hold point the charge sets, held there
		float lo, hi;
		windupMarks(actGroup.c_str(), lo, hi);
		const AnimGroup& g = sk.groups[a.group];
		float hold = g.start + (g.loopStart - g.start) * (lo + (hi - lo) * act.charge);
		a.time = fmaxf(a.time, fminf(a.time + dt * act.speed, hold));
		step = 0.0f;
	}
	else if (busy && strncmp(actGroup.c_str(), "Attack", 6) == 0)
	{
		step = dt * act.speed;
		// At the hit, a weaker blow cuts over to its small / medium follow-through (OpenMW: under 0.33 small,
		// under 0.66 medium, else the large one the group runs on into)
		const AnimGroup& g = sk.groups[a.group];
		if (!followCut && strcmp(g.name, actGroup.c_str()) == 0 && a.time + step >= g.loopStart)
		{
			followCut = true;
			std::string next = actGroup + (act.strength < 0.33f ? "S" : "M");
			if (act.strength < 0.66f && actorFindGroup(sk, next.c_str()) >= 0)
			{
				actorPlay(set, a, next.c_str(), ANIM_ONCE);
				step = 0.0f;
			}
		}
	}
	const AnimGroup* cur = a.group >= 0 && a.group < (int)sk.groups.size() ? &sk.groups[a.group] : nullptr;
	if (!busy && !act.airborne && group == "Jump" && cur && strncmp(cur->name, "Jump", 4) == 0 && cur->loopStop < cur->stop)
	{
		// Touching down: the jump's landing, Loop Stop to Stop, plays once before the walk or idle (OpenMW's
		// JumpState_Landing; in the air the jump held at its loop)
		a.mode = ANIM_ONCE;
		a.time = cur->loopStop;
		group = "";
	}
	else if (!busy)
	{
		// Walking, running and sneaking the way the stick goes against the facing (OpenMW's movement states: forward,
		// back, left, right), in the air the jump, standing; with a weapon out, its ready stance when the skeleton has
		// one ("Idle1h", "WalkForward2c")
		const char* dir = fabsf(act.moveX) > fabsf(act.moveY) ? (act.moveX < 0.0f ? "Left" : "Right")
			: act.moveY < 0.0f ? "Back" : "Forward";
		// (in the water the same names: actorPlay takes the swimming groups, SwimRunLeft, IdleSwim ... It asked for a
		// "SwimForward" no skeleton has, and swam with the land's walk and idle)
		a.swimming = swimming;
		std::string want = swimming ? (speed <= 10.0f ? "Idle" : std::string(speed > 200.0f ? "Run" : "Walk") + dir)
			: act.airborne ? "Jump"
			: speed <= 10.0f ? (sneaking ? "IdleSneak" : "Idle")     // (crouched still: OpenMW's CharState_IdleSneak)
			: std::string(act.flying ? "Walk" : sneaking ? "Sneak" : speed > 200.0f ? "Run" : "Walk") + dir;   // (OpenMW: no run or sneak in the air)
		if (want != group || weaponOut != stanceOut || swimming != swimOut)
		{
			stanceOut = weaponOut;
			swimOut = swimming;
			std::string stance = weaponOut && !swimming && act.stance[0] ? want + act.stance : "";
			// (data without the other ways: forward, then walking)
			std::string forward = want.substr(0, want.size() - strlen(dir)) + "Forward";
			if (!stance.empty() && actorFindGroup(sk, stance.c_str()) >= 0 && actorPlay(set, a, stance.c_str(), ANIM_LOOP))
				group = want;
			else if (want == "IdleSneak" && actorFindGroup(sk, "IdleSneak") < 0 && actorPlay(set, a, "Idle", ANIM_IDLE))
				group = want;     // (data without it: standing, as OpenMW falls back to the idle)
			else if (actorPlay(set, a, want.c_str(), want == "Idle" ? ANIM_IDLE : ANIM_LOOP) || want == "Idle")
				group = want;
			else if (speed > 10.0f && !act.airborne && actorPlay(set, a, forward.c_str(), ANIM_LOOP))
				group = want;
			else if (actorPlay(set, a, "WalkForward", ANIM_LOOP))
				group = "WalkForward";
		}
	}
	actorAnimate(set, a, step);
}

bool PlayerBody::loopTimes(float& time, float& loopStart, float& loopStop) const
{
	if (!ready || set.actors.empty())
		return false;
	const Actor& a = set.actors[0];
	const Skeleton& sk = actorSkeleton(set, a.skeleton);
	if (a.group < 0 || a.group >= (int)sk.groups.size())
		return false;
	time = a.time;
	loopStart = sk.groups[a.group].loopStart;
	loopStop = sk.groups[a.group].loopStop;
	return true;
}

const char* PlayerBody::playing() const
{
	if (!ready || set.actors.empty())
		return "";
	const Actor& a = set.actors[0];
	const Skeleton& sk = actorSkeleton(set, a.skeleton);
	return a.group >= 0 && a.group < (int)sk.groups.size() ? sk.groups[a.group].name : "";
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

bool npcDress(World& w, LoadedCell& l, Actor& a, const ActorDef& d, const std::vector<std::string>& items)
{
	const ThirdPersonDef& t = w.game.firstPerson.third;
	std::string race = lower(d.race.str());
	int female = d.female ? 1 : 0;
	// The bare body of its race and sex (male parts where the female's are missing), its own head and hair
	std::string part[kSlotCount];
	u32 glow[kSlotCount] = {};
	for (int sex : { female, 0 })
	{
		auto it = t.races[sex].find(race);
		if (it == t.races[sex].end())
			continue;
		for (auto& s : it->second)
			if (s.first >= 0 && s.first < kSlotCount && part[s.first].empty())
				part[s.first] = s.second;
	}
	auto own = [&](bool hair) {
		auto& map = (hair ? t.hairs : t.heads)[female];
		auto it = map.find(race);
		const std::string want = (hair ? d.hair : d.head).str();
		if (it != map.end())
			for (auto& e : it->second)
				if (e.first == want)
					return e.second;
		return std::string();
	};
	part[SLOT_HEAD] = own(false);
	part[SLOT_HAIR] = own(true);
	if (part[SLOT_HEAD].empty())
		return false;                     // (a head the pieces lack, a vampire's: the library stays)
	// What it still wears, as npc.py's parts(): a helmet takes the hair; clothing, armor, skirts, then robes over all
	for (auto& id : items)
		if (const Object* o = w.game.object(id))
			if (o->type == "ARMO" && o->subtype == 0)
				part[SLOT_HAIR].clear();
	for (int layer = 0; layer < 4; layer++)
		for (auto& id : items)
		{
			const Object* o = w.game.object(id);
			if (!o || (o->type != "CLOT" && o->type != "ARMO"))
				continue;
			int lr = o->type == "ARMO" ? 1 : o->subtype == CLOT_SKIRT ? 2 : o->subtype == CLOT_ROBE ? 3 : 0;
			if (lr != layer)
				continue;
			if (o->type == "CLOT" && o->subtype == CLOT_ROBE)
				for (int s : kRobeHides)
					part[s].clear();
			if (o->type == "CLOT" && o->subtype == CLOT_SKIRT)
				for (int s : kSkirtHides)
					part[s].clear();
			auto fit = t.items.find(id);
			if (fit == t.items.end())
				continue;
			for (auto& s : fit->second)
				if (s.first >= 0 && s.first < kSlotCount && s.first != SLOT_WEAPON)
				{
					part[s.first] = female && !s.second.second.empty() ? s.second.second : s.second.first;
					glow[s.first] = w.game.enchantGlow(id);
				}
		}
	// The pieces, read for this actor alone (a skin re-posed on the CPU writes its own vertices)
	const Skeleton& sk = actorSkeleton(l.actors, a.skeleton);
	std::vector<ActorMesh> made;
	bool ok = true;
	for (int s = 0; s < kSlotCount && ok; s++)
	{
		if (part[s].empty())
			continue;
		FpPiece* p = fpLoadPiece(w, sk, part[s]);
		if (!p)
		{
			ok = false;
			break;
		}
		for (auto& m : p->meshes)
		{
			ActorMesh copy = m;
			copy.tex = -1;
			const std::string& name = m.tex >= 0 && m.tex < (int)p->texNames.size() ? p->texNames[m.tex] : std::string();
			for (size_t k = 0; k < l.cell.textureNames.size() && !name.empty(); k++)
				if (l.cell.textureNames[k] == name)
					copy.tex = k;
			if (copy.tex < 0 && !name.empty())
			{
				copy.tex = l.cell.textureNames.size();
				l.cell.textureNames.push_back(name);
				l.cell.textures.push_back(w.textures.acquire(w.dataDir, name));
			}
			copy.glow = glow[s];
			if (s == SLOT_SHIELD)
				copy.hand = 2;            // put away while both hands are busy or the torch is out
			made.push_back(copy);
		}
		p->meshes.clear();                // (their buffers are the actor's now; the piece gives back its own textures)
		fpFreePiece(w, p);
	}
	if (!ok)
	{
		for (auto& m : made)
		{
			lockedLinearFree(m.verts);
			lockedLinearFree(m.indices);
		}
		return false;
	}
	// The library's torch and weapon stay; the rest goes
	std::vector<ActorMesh> keep;
	for (auto& m : a.meshes)
		if (m.hand == 1 || (m.flags & 128))
			keep.push_back(m);
		else
		{
			lockedLinearFree(m.verts);
			lockedLinearFree(m.indices);
			for (void* v : m.pathFrames)
				lockedLinearFree(v);
		}
	keep.insert(keep.end(), made.begin(), made.end());
	a.meshes.swap(keep);
	a.skinned = false;
	for (auto& m : a.meshes)
		a.skinned |= m.kind == ACTOR_SKINNED;
	a.deformed = false;
	a.skinSerial = ~0u;
	a.dressed = true;
	logf("npcdress: %s dressed again, %d items on, %d meshes", d.id.c_str(), (int)items.size(), (int)a.meshes.size());
	return true;
}

// NPCs wear what their library has on (the converter dressed them). Twice a second each loaded one is looked at: when
// an item of that leaves its pack (a stripped corpse, RemoveItem) it is dressed again from the third-person pieces with
// what is left (npcDress), as OpenMW's NpcAnimation rebuilds its parts when the inventory changes; a cell read again
// with the item still gone comes back dressed the same way
void Session::updateDress(float dt)
{
	if ((dressTimer -= dt) > 0.0f)
		return;
	dressTimer = 0.5f;
	for (LoadedCell* l : w.loaded)
		for (Actor& a : l->actors.actors)
		{
			if (a.ref < 0 || a.ref >= (int)w.refs.size() || w.refs[a.ref].actor < 0)
				continue;
			const Ref& r = w.refs[a.ref];
			const ActorDef& d = w.game.actors[r.actor];
			if (d.creature || d.worn.empty() || d.worn.size() > 32)
				continue;
			u32 mask = 0;
			std::vector<std::string> left;
			for (size_t k = 0; k < d.worn.size(); k++)
			{
				// (rings and amulets show nothing: one gone changes no part)
				if (!w.game.firstPerson.third.items.count(d.worn[k].str()))
				{
					mask |= 1u << k;
					continue;
				}
				for (const ContentItem& c : r.contents)
					if (c.first != 0 && c.second == d.worn[k].view())
					{
						mask |= 1u << k;
						left.push_back(c.second);
						break;
					}
			}
			u32 all = d.worn.size() == 32 ? ~0u : (1u << d.worn.size()) - 1;
			if (a.wornMask == ~0u && mask == all && !a.dressed)
				a.wornMask = mask;               // (as its library has it)
			if (mask == a.wornMask)
				continue;
			a.wornMask = mask;
			if (!npcDress(w, *l, a, d, left))
				logf("npcdress: %s kept as it was (a piece wouldn't load)", d.id.c_str());
		}
}
