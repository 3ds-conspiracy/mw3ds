// Which dialogue response fits: the INFO conditions and filter functions, as OpenMW's Filter has them
#include "mwdialogue/dialoguemanagerimp.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <cstring>

#include "mwworld/worldimp.hpp"
#include "mwdialogue/dialoguehelpers.hpp"

bool compare(char op, float a, float b)
{
	switch (op)
	{
	case '0': return a == b;
	case '1': return a != b;
	case '2': return a > b;
	case '3': return a >= b;
	case '4': return a < b;
	case '5': return a <= b;
	}
	return false;
}

// Dialogue filter functions ('1' conditions), numbered as in the construction set
static float filterFunction(World& w, const Cond& c, const Ref& speaker, int ref, int choice)
{
	const ActorDef* a = speaker.actor >= 0 ? &w.game.actors[speaker.actor] : nullptr;
	int f = c.func;
	if (f == 10) return w.stats.attributes[ATTR_STRENGTH];
	if (f >= 11 && f <= 37) return w.stats.skills[f - 11];
	if (f >= 51 && f <= 57) return w.stats.attributes[f - 50];
	switch (f)
	{
	case 50: return (float)choice;
	case 63:                                                         // talked to PC: as it was when this talk began
		return (ref == s_talkRef ? s_talked : speaker.talkedToPC) ? 1.0f : 0.0f;
	case 3: return (float)speaker.reputation + (a ? a->reputation : 0);
	case 5: return (float)w.pcReputation;
	case 43: return (float)w.bounty;                // PC crime level
	case 39: return a && w.pcExpelled.count(lower(a->faction)) ? 1.0f : 0.0f;   // PC expelled
	case 40: return w.pcHasDisease(3) ? 1.0f : 0.0f;                 // PC common disease
	case 41: return w.pcHasDisease(2) ? 1.0f : 0.0f;                 // PC blight disease
	case 58: return w.pcHasCorprus() ? 1.0f : 0.0f;                  // PC corprus
	case 60: return w.effectTotal(133) > 0.0f ? 1.0f : 0.0f;        // PC vampire: the Vampirism effect, not a script variable
	case 59: return (float)w.weatherNow;                             // weather (0 clear .. 9 blizzard)
	case 66: return (float)std::min(speaker.friendlyHits, 4);        // friendly hit (OpenMW stops at 4)
	case 72: return a && !a->creature && speaker.werewolf ? 1.0f : 0.0f;   // werewolf: the speaker (Bloodmoon)
	case 73: return (float)w.stats.werewolfKills;                    // PC werewolf kills
	case 65:                                                         // creature targeted: fighting a creature
	{
		if (!speaker.ally && !(speaker.aiPackage == AIPKG_FOLLOW && speaker.aiTarget == "player"))
			return 0.0f;
		for (int i : w.loadedActors)
		{
			const Ref& o = w.refs[i];
			if (o.ai == AI_COMBAT && !o.dead && o.visible() && !o.ally && o.actor >= 0 && w.game.actors[o.actor].creature
				&& hypotf(o.pos[0] - speaker.pos[0], o.pos[1] - speaker.pos[1]) < 1500.0f)
				return 1.0f;
		}
		return 0.0f;
	}
	case 49:                                                         // alarmed: fighting, or a guard come to arrest
		return speaker.ai == AI_COMBAT || (speaker.alarmed && w.bounty > 0) ? 1.0f : 0.0f;   // (paying clears it)
	case 62: return speaker.attacked ? 1.0f : 0.0f;                  // attacked
	case 71:                                                         // should attack: in combat, or fight term >= 100
		return w.shouldAttackHook ? (w.shouldAttackHook(ref) ? 1.0f : 0.0f)
			: (speaker.fight >= 0 ? speaker.fight : a ? a->fight : 30) >= 80 ? 1.0f : 0.0f;
	case 0: case 1:                                                  // lowest / highest reaction of the speaker's faction to the player's
	{
		std::string fac = a ? lower(a->faction) : "";
		if (fac.empty())
			return 0.0f;
		int value = 0;
		for (auto& pf : w.pcRank)
		{
			int re = w.factionReaction(fac, pf.first);
			if (f == 0 ? re < value : re > value)
				value = re;
		}
		return (float)value;
	}
	case 2:
	{
		// Rank requirement: +1 the player meets the next rank's attributes and skills, +2 its reputation
		std::string f = a ? lower(a->faction) : "";
		auto fit = w.game.factions.find(f);
		if (fit == w.game.factions.end())
			return 0.0f;
		const FactionDef& fd = fit->second;
		int next = w.pcRankIn(f) + 1;
		if (next >= 10 || next >= (int)fd.ranks.size())
			return 0.0f;
		const int* rq = fd.reqs[next];
		const PlayerStats& s = w.stats;
		bool attrs = (fd.attrs[0] < 0 || s.attributes[fd.attrs[0]] >= rq[0]) && (fd.attrs[1] < 0 || s.attributes[fd.attrs[1]] >= rq[1]);
		std::vector<int> sk;
		for (int k : fd.skills)
			if (k >= 0 && k < 27)
				sk.push_back(s.skills[k]);
		std::sort(sk.rbegin(), sk.rend());
		bool skills = sk.empty() || (sk[0] >= rq[2] && (sk.size() < 2 || sk[1] >= rq[3]));
		auto rep = w.pcFacRep.find(f);
		bool reputation = (rep != w.pcFacRep.end() ? rep->second : 0) >= rq[4];
		return (attrs && skills ? 1.0f : 0.0f) + (reputation ? 2.0f : 0.0f);
	}
	case 46: return a && !a->faction.empty() && w.pcRankIn(lower(a->faction)) >= 0 ? 1.0f : 0.0f;   // same faction
	case 47: return a && !a->faction.empty() ? (float)(w.pcRankIn(lower(a->faction)) - a->rank) : 0.0f;   // the player's rank (-1: none) less theirs
	case 67: return (float)(speaker.fight >= 0 ? speaker.fight : a ? a->fight : 30);          // fight
	case 68: return (float)(speaker.hello >= 0 ? speaker.hello : a ? a->hello : 30);          // hello
	case 44: return a && !a->creature && (a->female != 0) == w.stats.female ? 1.0f : 0.0f;       // (a creature: never)
	case 45: return a && !a->creature && lower(a->race) == lower(w.stats.race) ? 1.0f : 0.0f;
	case 38: return w.stats.female ? 1.0f : 0.0f;
	case 6: return (float)w.stats.level;
	case 61: return a ? (float)a->level : 1.0f;
	case 4: return speaker.healthMax > 0.0f ? (float)(int)(speaker.health * 100.0f / speaker.healthMax) : 100.0f;   // health percent (whole)
	case 7: return w.stats.healthMax > 0.0f ? (float)(int)(w.stats.health * 100.0f / w.stats.healthMax) : 100.0f;   // PC health percent
	case 64: return w.stats.health;
	case 8: return w.stats.magicka;
	case 9: return w.stats.fatigue;
	case 48: return !w.awarenessHook || w.awarenessHook(ref) ? 1.0f : 0.0f;   // detected: the speaker notices the player
	case 69: return (float)(speaker.alarm >= 0 ? speaker.alarm : a ? a->alarm : 0);           // alarm
	case 70: return (float)(speaker.flee >= 0 ? speaker.flee : a ? a->flee : 30);             // flee
	case 42:
	{
		// clothing modifier: value of what is worn (armor except the shield, and clothing; not weapons, ammo or lights)
		if (g_dialogueClothValue >= 0)
			return (float)g_dialogueClothValue;
		float sum = 0.0f;
		for (auto& it : w.inventory)
			if (it.equipped)
				if (const Object* o = w.game.object(it.id))
					if (o->recType == REC_CLOT || (o->recType == REC_ARMO && o->subtype != 8))
						sum += o->value;
		return sum;
	}
	default: return 0.0f;   // crime, disease, vampirism, faction standing, weather: none in this level
	}
}

// invert: Service Refusal's disposition is a ceiling (they refuse below it; 0 = always), as OpenMW has it
// 0: no; 1: yes; 2: it fits but for the speaker's disposition (OpenMW's infoRefusal)
static int passes(World& w, const Info& info, int ref, int choice, bool invert = false)
{
	const Ref& speaker = w.refs[ref];
	const ActorDef* sa = speaker.actor >= 0 ? &w.game.actors[speaker.actor] : nullptr;
	if (!sa)
		return 0;
	// creatures (Dagoth Endus, the Puzzle Canal dremora, Creeper) say only what names them as speaker
	if (sa->creature && info.whoId.empty() && info.actors.empty())
		return 0;
	if (!info.actors.empty())
	{
		bool actorOk = false;
		for (int a : info.actors)
			if (a == speaker.actor)
				actorOk = true;
		if (!actorOk)
			return 0;
	}
	// Who may say it
	if (!info.whoId.empty() && lower(sa->id) != info.whoId)
		return 0;
	// A creature named as the speaker skips the race, class, faction, rank and sex filters (OpenMW's testActor)
	if (!sa->creature)
	{
		if (!info.whoRace.empty() && lower(sa->race) != info.whoRace)
			return 0;
		if (!info.whoClass.empty() && lower(sa->cls) != info.whoClass)
			return 0;
		if (!info.whoFaction.empty() && (info.whoFaction == "ffff" ? !sa->faction.empty() : lower(sa->faction) != info.whoFaction))
			return 0;
		// Rank: the speaker's own in their faction (-1 without one); not asked of "no faction" lines
		if (info.whoRank >= 0 && info.whoFaction != "ffff" && (sa->faction.empty() ? -1 : sa->rank) < info.whoRank)
			return 0;
		if (info.whoSex >= 0 && (sa->female ? 1 : 0) != info.whoSex)
			return 0;
	}
	// The cell is the player's (a prefix of its name)
	if (!info.whoCell.empty() && (w.current < 0 || lower(w.cells[w.current].name).compare(0, info.whoCell.size(), info.whoCell) != 0))
		return 0;
	if (info.uid >= 0 && !w.clearedInfos.empty() && w.clearedInfos.count(speaker.idLower + "|" + std::to_string(info.uid)))
		return 0;                  // ClearInfoActor: said once, not again
	const ScriptInstance* si = speaker.script >= 0 ? &w.scripts[speaker.script] : nullptr;
	for (const Cond& c : info.conds)
	{
		float value = 0.0f;
		switch (c.kind)
		{
		case '1':
			if (c.func == 50 && choice < 0)
				return 0;     // choice responses only answer a choice
			if (c.func == 59 && !w.cells[w.placeOf(ref)].underSky())
				return 0;     // weather is not asked indoors (it is under Mournhold's sky: OpenMW's, not the original's)
			value = filterFunction(w, c, speaker, ref, choice);
			break;
		case '2':
		{
			auto g = w.globals.find(c.name);
			if (g == w.globals.end())
				continue;     // a global that does not exist is ignored
			value = g->second;
			break;
		}
		case '3':
		{
			int idx = si ? si->script->localIndex(c.name) : -1;
			if (idx < 0)
				return 0;
			value = si->locals[idx];
			break;
		}
		case 'C':
		{
			// NotLocal: the opposite of the local test (no script or no such variable: passes)
			int idx = si ? si->script->localIndex(c.name) : -1;
			if (idx >= 0 && compare(c.op, si->locals[idx], c.value))
				return 0;
			continue;
		}
		case '4': value = (float)w.getJournal(c.name); break;
		case 'F':
		{
			// The player must be in this faction, at least at this rank
			int rank = w.pcRankIn(c.name);
			if (rank < 0)
				return 0;
			value = (float)rank;
			break;
		}
		case 'G':
		{
			// The player's rank in the speaker's faction
			value = !sa->faction.empty() ? (float)w.pcRankIn(lower(sa->faction)) : -1.0f;
			break;
		}
		case '5': value = (float)w.itemCount(c.name); break;
		// Not ID / faction / class / race / cell: 1 when the speaker's differs (the operator and value are not used)
		case '7': value = lower(sa->id) != c.name ? 1.0f : 0.0f; break;
		case '8': value = lower(sa->faction) != c.name ? 1.0f : 0.0f; break;
		case '9': value = lower(sa->cls) != c.name ? 1.0f : 0.0f; break;
		case 'A': value = lower(sa->race) != c.name ? 1.0f : 0.0f; break;
		case 'B': value = lower(w.cells[w.placeOf(ref)].name).compare(0, c.name.size(), c.name) != 0 ? 1.0f : 0.0f; break;
		case '6':
		{
			auto dead = w.deadCounts.find(c.name);
			value = dead != w.deadCounts.end() ? (float)dead->second : 0.0f;
			break;
		}
		default: return 0;
		}
		if (c.kind == '7' || c.kind == '8' || c.kind == '9' || c.kind == 'A' || c.kind == 'B')
		{
			if (value == 0.0f)
				return 0;
			continue;
		}
		if (!compare(c.op, value, c.value))
			return 0;
	}
	// Disposition last: a response only the speaker's mood refuses falls back to "Info Refusal". Creatures
	// don't ask. invert: Service Refusal's disposition is a ceiling (they refuse below it; 0 = always)
	if (!sa->creature)
	{
		int disp = w.disposition(ref);
		if (invert ? info.disposition != 0 && disp >= info.disposition : disp < info.disposition)
			return 2;
	}
	return 1;
}

// The first response that fits; with refusal, a topic whose responses fit but for the speaker's disposition
// answers with the "Info Refusal" topic's instead (OpenMW's search(topic, true): topic lists and learning too)
const Info* dialogueFindInfo(World& w, const Topic& topic, int ref, int choice, bool invert, bool refusal)
{
	bool refused = false;
	for (const Info& info : topic.infos)
	{
		int r = passes(w, info, ref, choice, invert);
		if (r == 1)
			return &info;
		refused |= r == 2;
	}
	if (refused && refusal)
		if (const Topic* t = w.game.topic("Info Refusal"))
			return dialogueFindInfo(w, *t, ref, choice, invert, false);
	return nullptr;
}
