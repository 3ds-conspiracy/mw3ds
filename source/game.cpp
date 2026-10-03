#include "game.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>

#include "cJSON.h"
#include "log.h"
#include "zfile.h"
#include "script.h"

std::string lower(const std::string& s)
{
	std::string out = s;
	for (auto& c : out)
		if (c >= 'A' && c <= 'Z')
			c = c - 'A' + 'a';
	return out;
}

int Script::localIndex(const std::string& lowerName) const
{
	for (size_t i = 0; i < localNames.size(); i++)
		if (localNames[i] == lowerName)
			return (int)i;
	return -1;
}

void GameData::levCandidates(const LeveledList& l, int level, std::vector<int>& out)
{
	out.clear();
	int top = 0;
	for (auto& e : l.entries)
		if (e.first > top && e.first <= level)
			top = e.first;
	for (size_t i = 0; i < l.entries.size(); i++)
		if (l.entries[i].first <= level && (l.all || l.entries[i].first == top))
			out.push_back((int)i);
}

// One of the entries at or below the player's level, at random (nested lists followed)
std::string GameData::pickLeveled(const std::string& list, int level, unsigned* seed) const
{
	auto it = leveled.find(list);
	if (it == leveled.end())
		return list;
	auto roll = [&](int n) {
		if (!seed)
			return rand() % n;
		*seed = *seed * 1103515245u + 12345u;
		return (int)((*seed >> 16) % (unsigned)n);
	};
	const LeveledList& l = it->second;
	if (l.none > 0 && roll(100) < l.none)
		return "";
	std::vector<int> ok;
	levCandidates(l, level, ok);
	if (ok.empty())
		return "";
	const std::string& pick = l.entries[ok[roll((int)ok.size())]].second;
	return leveled.count(pick) ? pickLeveled(pick, level, seed) : pick;
}

// School of each magic skill: alteration, conjuration, destruction, illusion, mysticism, restoration
static const int kSchoolSkillIdx[6] = { 11, 13, 10, 12, 14, 15 };

// What autocalc needs of each candidate spell, worked out once (the lookups and costs don't depend
// on who casts): per effect its school's skill and its cost term (calcWeakestSchool's x)
struct CalcEffect { int school, skill, attribute; float x; };
struct CalcSpell { const SpellDef* sp; std::vector<CalcEffect> effects; };

u32 g_gameGeneration = 1;

static const std::vector<CalcSpell>& calcSpells(const GameData& g, bool player)
{
	static std::vector<CalcSpell> lists[2];
	static bool built[2] = { false, false };
	// another session's data (a save loaded): the old lists point into freed spells. The generation, not the
	// address: a new GameData can be allocated right where the freed one was
	static u32 builtGen = 0;
	if (builtGen != g_gameGeneration)
	{
		builtGen = g_gameGeneration;
		lists[0].clear();
		lists[1].clear();
		built[0] = built[1] = false;
	}
	std::vector<CalcSpell>& out = lists[player ? 1 : 0];
	if (built[player ? 1 : 0])
		return out;
	built[player ? 1 : 0] = true;
	float costMult = g.gmstf("feffectcostmult", 0.5f);
	for (auto& kv : g.spells)
	{
		const SpellDef& sp = kv.second;
		if (sp.type != 0 || !(sp.flags & (player ? 2 : 1)))
			continue;
		CalcSpell cs;
		cs.sp = &sp;
		for (auto& e : sp.effects)
		{
			auto me = g.magicEffects.find(e.effect);
			if (me == g.magicEffects.end())
				continue;
			const MagicEffectDef& d = me->second;
			int mn = 1, mx = 1;
			if (!(d.flags & 0x8)) { mn = e.min; mx = e.max; }
			int dur = (d.flags & 0x4) ? 0 : e.duration;
			dur = dur < 1 ? 1 : dur;
			float x = 0.5f * ((mn > 1 ? mn : 1) + (mx > 1 ? mx : 1)) * 0.1f * d.cost * (1 + dur) + 0.05f * d.cost;
			x *= costMult;
			if (e.range == 2)
				x *= 1.5f;
			int school = d.school >= 0 && d.school < 6 ? d.school : 0;
			cs.effects.push_back({ school, e.skill, e.attribute, x });
		}
		out.push_back(cs);
	}
	std::sort(out.begin(), out.end(), [](const CalcSpell& a, const CalcSpell& b) { return a.sp->id < b.sp->id; });
	return out;
}

// The school a spell is hardest in for these skills, and 2 x that skill (calcWeakestSchool)
static int weakestSchool(const CalcSpell& cs, const int* skills, float* skillTerm)
{
	float minChance = 1e9f;
	int school = -1;
	*skillTerm = 0.0f;
	for (auto& e : cs.effects)
	{
		float s = 2.0f * skills[kSchoolSkillIdx[e.school]];
		if (s - e.x < minChance)
		{
			minChance = s - e.x;
			school = e.school;
			*skillTerm = s;
		}
	}
	return school;
}

std::vector<std::string> GameData::autoCalcSpells(const int* attributes, const int* skills, bool player) const
{
	static float magickaMult[2], chanceNeeded[2];
	static int timesCanCast, attSkillMin, capLimit[6], pcLimit;
	static bool init = false;
	if (!init)
	{
		init = true;
		magickaMult[0] = gmstf("fnpcbasemagickamult", 2.0f);
		magickaMult[1] = gmstf("fpcbasemagickamult", 1.0f);
		chanceNeeded[0] = gmstf("fautospellchance", 80.0f);
		chanceNeeded[1] = gmstf("fautopcspellchance", 50.0f);
		timesCanCast = (int)gmstf("iautospelltimescancast", 3.0f);
		attSkillMin = (int)gmstf("iautospellattskillmin", 70.0f);
		static const char* capNames[6] = { "iautospellalterationmax", "iautospellconjurationmax", "iautospelldestructionmax",
			"iautospellillusionmax", "iautospellmysticismmax", "iautospellrestorationmax" };
		for (int k = 0; k < 6; k++)
			capLimit[k] = (int)gmstf(capNames[k], 5.0f);
		pcLimit = (int)gmstf("iautopcspellmax", 100.0f);
	}
	int pi = player ? 1 : 0;
	float magicka = magickaMult[pi] * attributes[1];
	struct Cap { int count = 0, limit = 0, minCost = 1 << 30; const CalcSpell* weakest = nullptr; bool full = false; } caps[6], pc;
	for (int k = 0; k < 6; k++)
		caps[k].limit = capLimit[k];
	pc.limit = pcLimit;
	std::vector<const CalcSpell*> picked;
	for (const CalcSpell& cs : calcSpells(*this, player))
	{
		const SpellDef* sp = cs.sp;
		float skillTerm;
		int school = weakestSchool(cs, skills, &skillTerm);
		Cap& cap = player ? pc : caps[school >= 0 ? school : 0];
		if (cap.full && sp->cost <= cap.minCost)
			continue;
		if (magicka < (player ? 1 : timesCanCast) * sp->cost)
			continue;
		bool ok = true;
		for (auto& e : cs.effects)
			if ((e.skill >= 0 && e.skill < 27 && skills[e.skill] < attSkillMin)
				|| (e.attribute >= 0 && e.attribute < 8 && attributes[e.attribute] < attSkillMin))
				ok = false;
		if (!ok)
			continue;
		float chance = (sp->flags & 4) ? 100.0f : skillTerm - sp->cost + 0.2f * attributes[2] + 0.1f * attributes[7];
		if (chance < chanceNeeded[pi])
			continue;
		picked.push_back(&cs);
		if (cap.full)
		{
			// the cheapest of that school goes, and the next cheapest is the one to beat
			auto gone = std::find(picked.begin(), picked.end(), cap.weakest);
			if (gone != picked.end())
				picked.erase(gone);
			cap.minCost = 1 << 30;
			cap.weakest = nullptr;
			for (const CalcSpell* p : picked)
			{
				float st;
				if ((player || weakestSchool(*p, skills, &st) == school) && p->sp->cost < cap.minCost)
				{
					cap.minCost = p->sp->cost;
					cap.weakest = p;
				}
			}
		}
		else
		{
			if (sp->cost < cap.minCost)
			{
				cap.minCost = sp->cost;
				cap.weakest = &cs;
			}
			if (++cap.count >= cap.limit)
				cap.full = true;
		}
	}
	std::vector<std::string> out;
	for (const CalcSpell* p : picked)
		out.push_back(p->sp->id);
	return out;
}

const Object* GameData::object(const std::string& id) const
{
	auto it = objects.find(lower(id));
	return it == objects.end() ? nullptr : &it->second;
}

const Script* GameData::script(const std::string& name) const
{
	auto it = scripts.find(lower(name));
	return it == scripts.end() ? nullptr : &it->second;
}

std::string GameData::gmst(const char* name, const char* fallback) const
{
	auto it = gmstText.find(lower(name));
	return it == gmstText.end() ? fallback : it->second;
}

float GameData::gmstf(const char* name, float fallback) const
{
	auto it = gmstNum.find(lower(name));
	return it == gmstNum.end() ? fallback : it->second;
}

const Topic* GameData::topic(const std::string& name) const
{
	std::string l = lower(name);
	for (auto& t : topics)
		if (t.lower == l)
			return &t;
	return nullptr;
}

std::string GameData::factionName(const std::string& id) const
{
	auto it = factions.find(lower(id));
	return it == factions.end() ? id : it->second.name;
}

std::string GameData::rankName(const std::string& faction, int rank) const
{
	auto it = factions.find(lower(faction));
	if (it == factions.end() || rank < 0 || rank >= (int)it->second.ranks.size())
		return "";
	return it->second.ranks[rank];
}

// ---- JSON helpers

static std::string str(const cJSON* obj, const char* key)
{
	const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
	return cJSON_IsString(v) ? v->valuestring : "";
}

static double num(const cJSON* obj, const char* key, double fallback = 0.0)
{
	const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
	return cJSON_IsNumber(v) ? v->valuedouble : fallback;
}

static const cJSON* arr(const cJSON* obj, const char* key)
{
	return cJSON_GetObjectItemCaseSensitive(obj, key);
}

// ---- Syntax tree conversion

static void convertExpr(const cJSON* j, Node& n, const Script* scope);

static void convertBlock(const cJSON* list, Node& n, const Script* scope);

static OpKind opKind(const char* s)
{
	static const char* names[] = { "+", "-", "*", "/", "==", "!=", "<", "<=", ">", ">=", "//" };
	for (int i = 0; i < 11; i++)
		if (strcmp(s, names[i]) == 0)
			return (OpKind)i;
	return OP_EQ;
}

static void convertCall(const cJSON* j, Node& n, const Script* scope)
{
	n.kind = N_CALL;
	const cJSON* ref = cJSON_GetArrayItem(j, 1);
	n.a = cJSON_IsString(ref) ? ref->valuestring : "";
	n.b = cJSON_GetArrayItem(j, 2)->valuestring;
	n.func = scriptFunctionId(n.b);
	const cJSON* args = cJSON_GetArrayItem(j, 3);
	const cJSON* a;
	cJSON_ArrayForEach(a, args)
	{
		n.kids.emplace_back();
		convertExpr(a, n.kids.back(), scope);
	}
}

static void convertExpr(const cJSON* j, Node& n, const Script* scope)
{
	if (cJSON_IsNumber(j))
	{
		n.kind = N_NUM;
		n.num = (float)j->valuedouble;
		return;
	}
	if (!cJSON_IsArray(j) || cJSON_GetArraySize(j) == 0)
	{
		n.kind = N_NUM;
		return;
	}
	const char* tag = cJSON_GetArrayItem(j, 0)->valuestring;
	if (strcmp(tag, "s") == 0)
	{
		n.kind = N_STR;
		n.a = cJSON_GetArrayItem(j, 1)->valuestring;
	}
	else if (strcmp(tag, "l") == 0)
	{
		n.kind = N_LOCAL;
		n.a = cJSON_GetArrayItem(j, 1)->valuestring;
		n.func = scope ? scope->localIndex(n.a) : -1;
	}
	else if (strcmp(tag, "g") == 0)
	{
		n.kind = N_GLOBAL;
		n.a = cJSON_GetArrayItem(j, 1)->valuestring;
	}
	else if (strcmp(tag, "r") == 0)
	{
		n.kind = N_REMOTE;
		n.a = cJSON_GetArrayItem(j, 1)->valuestring;
		n.b = cJSON_GetArrayItem(j, 2)->valuestring;
	}
	else if (strcmp(tag, "c") == 0)
		convertCall(j, n, scope);
	else if (strcmp(tag, "neg") == 0)
	{
		n.kind = N_NEG;
		n.kids.emplace_back();
		convertExpr(cJSON_GetArrayItem(j, 1), n.kids.back(), scope);
	}
	else
	{
		n.kind = N_OP;
		n.op = opKind(tag);
		n.kids.resize(2);
		convertExpr(cJSON_GetArrayItem(j, 1), n.kids[0], scope);
		convertExpr(cJSON_GetArrayItem(j, 2), n.kids[1], scope);
	}
}

static void convertStatement(const cJSON* j, Node& n, const Script* scope)
{
	const char* tag = cJSON_GetArrayItem(j, 0)->valuestring;
	if (strcmp(tag, "set") == 0)
	{
		n.kind = N_SET;
		n.kids.resize(2);
		convertExpr(cJSON_GetArrayItem(j, 1), n.kids[0], scope);
		convertExpr(cJSON_GetArrayItem(j, 2), n.kids[1], scope);
	}
	else if (strcmp(tag, "if") == 0)
	{
		n.kind = N_IF;
		const cJSON* branch;
		cJSON_ArrayForEach(branch, cJSON_GetArrayItem(j, 1))
		{
			n.kids.emplace_back();
			convertExpr(cJSON_GetArrayItem(branch, 0), n.kids.back(), scope);
			n.kids.emplace_back();
			convertBlock(cJSON_GetArrayItem(branch, 1), n.kids.back(), scope);
		}
		const cJSON* elseBody = cJSON_GetArrayItem(j, 2);
		if (cJSON_IsArray(elseBody))
		{
			n.kids.emplace_back();
			convertBlock(elseBody, n.kids.back(), scope);
		}
	}
	else if (strcmp(tag, "while") == 0)
	{
		n.kind = N_WHILE;
		n.kids.resize(2);
		convertExpr(cJSON_GetArrayItem(j, 1), n.kids[0], scope);
		convertBlock(cJSON_GetArrayItem(j, 2), n.kids[1], scope);
	}
	else if (strcmp(tag, "ret") == 0)
		n.kind = N_RET;
	else if (strcmp(tag, "c") == 0)
		convertCall(j, n, scope);
}

static void convertBlock(const cJSON* list, Node& n, const Script* scope)
{
	n.kind = N_BLOCK;
	const cJSON* s;
	cJSON_ArrayForEach(s, list)
	{
		n.kids.emplace_back();
		convertStatement(s, n.kids.back(), scope);
	}
}

static bool treeCalls(const Node& n, const char* func)
{
	if (n.kind == N_CALL && n.b == func)
		return true;
	for (auto& k : n.kids)
		if (treeCalls(k, func))
			return true;
	return false;
}

// ---- Loading

static char* readFile(const char* path, size_t* size)
{
	return zreadAll(path, size);
}

static std::vector<SpellEffect> readEffects(const cJSON* list)
{
	std::vector<SpellEffect> out;
	const cJSON* e;
	cJSON_ArrayForEach(e, list)
		out.push_back({ (int)num(e, "effect"), (int)num(e, "skill"), (int)num(e, "attribute"),
			(int)num(e, "min"), (int)num(e, "max"), (int)num(e, "duration"), (int)num(e, "range"), (int)num(e, "area") });
	return out;
}

// Big top-level sections are in files of their own (game_<name>.json, {"<name>": ...}) so only one
// is parsed at a time: a whole parsed game.json is several times its size and doesn't fit the
// heap next to the converted data. Older data has them inside game.json. The returned node stays
// valid until the next call (or sectionDone).
static cJSON* s_section = nullptr;
static std::string s_dataDir;

static void sectionDone()
{
	cJSON_Delete(s_section);
	s_section = nullptr;
}

// Reads game_<file>.json and returns its <name> member (nullptr: no such file)
static const cJSON* sectionFile(const std::string& file, const char* name)
{
	sectionDone();
	std::string path = s_dataDir + "/game_" + file + ".json";
	size_t size = 0;
	char* text = zreadAll(path.c_str(), &size);
	if (!text)
		return nullptr;
	s_section = cJSON_Parse(text);
	free(text);
	if (!s_section)
	{
		logf("game: %s: JSON parse error", path.c_str());
		return nullptr;
	}
	logf("game: %s (%u KB), heap used %d KB", file.c_str(), (unsigned)(size / 1024), mallinfo().uordblks / 1024);
	return cJSON_GetObjectItemCaseSensitive(s_section, name);
}

// Calls fn(item) for every member of a section: in game.json, in game_<name>.json, or in the parts
// game_<name>_0.json, _1 ... the converter splits big sections into (read one at a time)
template <typename Fn>
static void forSection(const cJSON* root, const char* name, Fn fn)
{
	const cJSON* it;
	if (const cJSON* v = cJSON_GetObjectItemCaseSensitive(root, name))
	{
		cJSON_ArrayForEach(it, v)
			fn(it);
		return;
	}
	if (const cJSON* v = sectionFile(name, name))
	{
		cJSON_ArrayForEach(it, v)
			fn(it);
		sectionDone();
		return;
	}
	for (int k = 0;; k++)
	{
		const cJSON* v = sectionFile(std::string(name) + "_" + std::to_string(k), name);
		if (!v)
			break;
		cJSON_ArrayForEach(it, v)
			fn(it);
	}
	sectionDone();
}

static const cJSON* section(const cJSON* root, const char* name)
{
	sectionDone();
	if (const cJSON* v = cJSON_GetObjectItemCaseSensitive(root, name))
		return v;
	std::string path = s_dataDir + "/game_" + name + ".json";
	size_t size = 0;
	char* text = zreadAll(path.c_str(), &size);
	if (!text)
		return nullptr;
	s_section = cJSON_Parse(text);
	free(text);
	if (!s_section)
	{
		logf("game: %s: JSON parse error", path.c_str());
		return nullptr;
	}
	logf("game: %s (%u KB), heap used %d KB", name, (unsigned)(size / 1024), mallinfo().uordblks / 1024);
	return cJSON_GetObjectItemCaseSensitive(s_section, name);
}

bool gameLoad(GameData& g, const char* path)
{
	g_gameGeneration++;
	s_dataDir = path;
	size_t slash = s_dataDir.rfind('/');
	s_dataDir = slash == std::string::npos ? "." : s_dataDir.substr(0, slash);
	size_t size = 0;
	char* text = readFile(path, &size);
	if (!text)
	{
		logf("game: cannot read %s", path);
		return false;
	}
	logf("game: parsing %u KB, heap used %d KB", (unsigned)(size / 1024), mallinfo().uordblks / 1024);
	cJSON* root = cJSON_Parse(text);
	free(text);
	if (!root)
	{
		logf("game: JSON parse error near %.40s", cJSON_GetErrorPtr());
		return false;
	}

	const cJSON* it0;
	g.cellName = str(root, "cell");
	g.chargen = !cJSON_IsFalse(cJSON_GetObjectItem(root, "chargen"));
	cJSON_ArrayForEach(it0, arr(root, "cells"))
	{
		CellDef c{ str(it0, "name"), str(it0, "file"), cJSON_IsTrue(cJSON_GetObjectItem(it0, "interior")) != 0 };
		if (const cJSON* g = cJSON_GetObjectItem(it0, "grid"))
		{
			c.gx = cJSON_GetArrayItem(g, 0)->valueint;
			c.gy = cJSON_GetArrayItem(g, 1)->valueint;
		}
		c.sea = num(it0, "sea");
		c.noSleep = cJSON_IsTrue(cJSON_GetObjectItem(it0, "nosleep"));
		c.region = str(it0, "region");
		g.cells.push_back(c);
	}
	const cJSON* it;
	forSection(root, "actors", [&](const cJSON* it) {
		ActorDef a;
		a.id = str(it, "id"); a.name = str(it, "name"); a.race = str(it, "race"); a.cls = str(it, "class");
		a.faction = str(it, "faction"); a.script = str(it, "script");
		a.rank = num(it, "rank"); a.female = num(it, "female"); a.level = num(it, "level");
		a.disposition = num(it, "disposition"); a.reputation = num(it, "reputation");
		for (int k = 0; k < 8; k++)
			if (const cJSON* v = cJSON_GetArrayItem(arr(it, "attributes"), k))
				a.attributes[k] = v->valueint;
		for (int k = 0; k < 27; k++)
			a.skills[k] = cJSON_GetArrayItem(arr(it, "skills"), k) ? cJSON_GetArrayItem(arr(it, "skills"), k)->valueint : 20;
		a.health = num(it, "health", 50); a.fatigue = num(it, "fatigue", 200); a.gold = num(it, "gold");
		a.fight = num(it, "fight", 30); a.services = (unsigned)num(it, "services");
		a.weapon = lower(str(it, "weapon")); a.armor = num(it, "armor"); a.wander = num(it, "wander");
		a.lib = str(it, "lib");
		a.skel = num(it, "skel", -1);
		if (const cJSON* ai = cJSON_GetObjectItem(it, "ai"))
		{
			a.aiPackage = cJSON_GetArrayItem(ai, 0)->valueint;
			a.aiTarget = cJSON_GetArrayItem(ai, 1)->valuestring;
			for (int k = 0; k < 3; k++)
				a.aiDest[k] = (float)cJSON_GetArrayItem(ai, 2 + k)->valuedouble;
			a.aiDuration = (float)cJSON_GetArrayItem(ai, 5)->valuedouble;
		}
		a.creature = cJSON_IsTrue(cJSON_GetObjectItem(it, "creature"));
		a.magicka = num(it, "magicka"); a.flee = num(it, "flee"); a.soul = num(it, "soul"); a.resistNormal = num(it, "resist_normal"); a.hello = (int)num(it, "hello", 30); a.alarm = (int)num(it, "alarm", 100);
		a.respawn = num(it, "respawn") != 0; a.essential = num(it, "essential") != 0; a.blood = (int)num(it, "blood"); a.afloat = (u8)num(it, "afloat");
		a.autocalc = num(it, "autocalc") != 0;
		a.ammo = lower(str(it, "ammo")); a.shield = lower(str(it, "shield"));
		const cJSON* cs;
		cJSON_ArrayForEach(cs, arr(it, "combat_spells"))
			a.combatSpells.push_back(lower(cs->valuestring));
		cJSON_ArrayForEach(cs, arr(it, "diseases"))
			a.diseases.push_back(lower(cs->valuestring));
		cJSON_ArrayForEach(cs, arr(it, "const_effects"))
			if (cJSON_GetArraySize(cs) >= 2)
				a.constEffects.push_back({ cJSON_GetArrayItem(cs, 0)->valueint, (float)cJSON_GetArrayItem(cs, 1)->valuedouble });
		const cJSON* sp;
		cJSON_ArrayForEach(sp, arr(it, "spells"))
			a.spells.push_back(lower(sp->valuestring));
		const cJSON* at;
		cJSON_ArrayForEach(at, arr(it, "attack"))
			a.attack.emplace_back(cJSON_GetArrayItem(at, 0)->valueint, cJSON_GetArrayItem(at, 1)->valueint);
		const cJSON* td;
		cJSON_ArrayForEach(td, arr(it, "travel"))
		{
			TravelDest d;
			d.cell = str(td, "cell");
			for (int k = 0; k < 3; k++)
			{
				d.pos[k] = cJSON_GetArrayItem(arr(td, "pos"), k)->valuedouble;
				d.rot[k] = cJSON_GetArrayItem(arr(td, "rot"), k)->valuedouble;
			}
			if (const cJSON* g = arr(td, "grid"))
			{
				d.hasGrid = true;
				d.grid[0] = cJSON_GetArrayItem(g, 0)->valueint;
				d.grid[1] = cJSON_GetArrayItem(g, 1)->valueint;
			}
			a.travel.push_back(d);
		}
		g.actors.push_back(a);
	});

	forSection(root, "objects", [&](const cJSON* it) {
		Object o;
		o.id = str(it, "id"); o.type = str(it, "type"); o.name = str(it, "name"); o.script = str(it, "script");
		o.icon = str(it, "icon"); o.iconIx = num(it, "ic", -1); o.text = str(it, "text"); o.sound = str(it, "sound");
		o.openSound = str(it, "open_sound"); o.closeSound = str(it, "close_sound");
		o.weight = num(it, "weight"); o.value = num(it, "value"); o.armor = num(it, "armor");
		o.flags = num(it, "flags"); o.scroll = num(it, "scroll") != 0; o.respawn = num(it, "respawn") != 0; o.organic = num(it, "organic") != 0;
		if (cJSON_GetObjectItem(it, "capacity"))
			o.capacity = num(it, "capacity");
		o.lightRadius = (int)num(it, "radius");
		if (const cJSON* lc = cJSON_GetObjectItem(it, "color"))
			for (int k = 0; k < 3; k++)
				if (const cJSON* c = cJSON_GetArrayItem(lc, k))
					o.lightColor[k] = (u8)c->valueint;
		// (this used to share the line above, inside the light colour loop: only lights ever read "magic", so every
		// enchanted weapon counted as ordinary against Resist Normal Weapons)
		o.magic = num(it, "magic") != 0;
		o.bookSkill = num(it, "skill", -1);
		o.subtype = num(it, "wtype", num(it, "atype", num(it, "ctype", num(it, "subtype", -1))));
		for (int k = 0; k < 2; k++)
		{
			const cJSON* v;
			if ((v = cJSON_GetArrayItem(arr(it, "chop"), k))) o.chop[k] = v->valueint;
			if ((v = cJSON_GetArrayItem(arr(it, "slash"), k))) o.slash[k] = v->valueint;
			if ((v = cJSON_GetArrayItem(arr(it, "thrust"), k))) o.thrust[k] = v->valueint;
		}
		o.speed = num(it, "speed", 1.0); o.reach = num(it, "reach", 1.0);
		o.health = num(it, "health"); o.uses = num(it, "uses"); o.quality = num(it, "quality");
		o.effects = readEffects(arr(it, "effects"));
		o.ench = str(it, "ench");
		o.enchant = num(it, "enchant");
		const cJSON* ie;
		cJSON_ArrayForEach(ie, arr(it, "ingr"))
			o.ingredient.push_back({ cJSON_GetArrayItem(ie, 0)->valueint, cJSON_GetArrayItem(ie, 1)->valueint,
				cJSON_GetArrayItem(ie, 2)->valueint });
		const cJSON* item;
		cJSON_ArrayForEach(item, arr(it, "items"))
			o.items.emplace_back((int)cJSON_GetArrayItem(item, 0)->valuedouble, cJSON_GetArrayItem(item, 1)->valuestring);
		g.objects[it->string] = o;
	});

	forSection(root, "scripts", [&](const cJSON* it) {
		Script& s = g.scripts[it->string];
		s.name = str(it, "name");
		const cJSON* l;
		cJSON_ArrayForEach(l, arr(it, "locals"))
		{
			s.localTypes.push_back(cJSON_GetArrayItem(l, 0)->valuestring[0]);
			s.localNames.push_back(cJSON_GetArrayItem(l, 1)->valuestring);
		}
		convertBlock(arr(it, "body"), s.body, &s);
		s.usesOnActivate = treeCalls(s.body, "onactivate");
		s.scriptedHits = treeCalls(s.body, "hitonme") || treeCalls(s.body, "sethealth");
	});

	forSection(root, "dialogue", [&](const cJSON* it) {
		Topic t;
		t.name = str(it, "name");
		t.lower = lower(t.name);
		t.type = num(it, "type");
		const cJSON* ij;
		cJSON_ArrayForEach(ij, arr(it, "infos"))
		{
			Info info;
			static int nextUid = 0;
			info.uid = nextUid++;
			const cJSON* a;
			cJSON_ArrayForEach(a, arr(ij, "actors"))
				info.actors.push_back(a->valueint);
			if (const cJSON* who = cJSON_GetObjectItem(ij, "who"))
			{
				info.whoId = str(who, "id");
				info.whoRace = str(who, "race");
				info.whoClass = str(who, "class");
				info.whoFaction = str(who, "faction");
				info.whoCell = str(who, "cell");
				info.whoSex = num(who, "sex", -1);
				info.whoRank = num(who, "rank", -1);
			}
			info.text = str(ij, "text");
			info.sound = str(ij, "sound");
			info.disposition = num(ij, "disp");
			const cJSON* c;
			cJSON_ArrayForEach(c, arr(ij, "conds"))
			{
				Cond cond;
				cond.kind = cJSON_GetArrayItem(c, 0)->valuestring[0];
				const cJSON* name = cJSON_GetArrayItem(c, 1);
				cond.name = cJSON_IsString(name) ? name->valuestring : "";
				cond.func = cond.kind == '1' ? atoi(cond.name.c_str()) : 0;
				cond.op = cJSON_GetArrayItem(c, 2)->valuestring[0];
				cond.value = (float)cJSON_GetArrayItem(c, 3)->valuedouble;
				info.conds.push_back(cond);
			}
			convertBlock(arr(ij, "script"), info.script, nullptr);
			t.infos.push_back(std::move(info));
		}
		g.topics.push_back(std::move(t));
	});

	forSection(root, "journal", [&](const cJSON* it) {
		Journal& jn = g.journals[it->string];
		jn.name = str(it, "name");
		jn.title = str(it, "title");
		const cJSON* e;
		cJSON_ArrayForEach(e, arr(it, "entries"))
			jn.entries.push_back({ (int)num(e, "index"), str(e, "text"), num(e, "finished") != 0, num(e, "restart") != 0 });
	});

	cJSON_ArrayForEach(it, arr(root, "globals"))
	{
		std::string type = str(it, "type");
		g.globals[it->string] = { type.empty() ? 'f' : type[0], (float)num(it, "value") };
	}

	forSection(root, "gmst", [&](const cJSON* it) {
		if (cJSON_IsString(it))
			g.gmstText[it->string] = it->valuestring;
		else if (cJSON_IsNumber(it))
			g.gmstNum[it->string] = (float)it->valuedouble;
	});

	forSection(root, "classes", [&](const cJSON* it) {
		ClassDef c;
		c.id = str(it, "id"); c.name = str(it, "name"); c.desc = str(it, "desc");
		for (int i = 0; i < 2; i++) c.attributes[i] = cJSON_GetArrayItem(arr(it, "attributes"), i)->valueint;
		for (int i = 0; i < 5; i++)
		{
			c.major[i] = cJSON_GetArrayItem(arr(it, "major"), i)->valueint;
			c.minor[i] = cJSON_GetArrayItem(arr(it, "minor"), i)->valueint;
		}
		c.specialization = num(it, "specialization");
		c.playable = num(it, "playable") != 0;
		g.classes.push_back(c);
	});

	forSection(root, "races", [&](const cJSON* it) {
		RaceDef r;
		r.id = str(it, "id"); r.name = str(it, "name"); r.desc = str(it, "desc");
		const cJSON* b;
		cJSON_ArrayForEach(b, arr(it, "skill_bonus"))
			r.skillBonus.emplace_back(cJSON_GetArrayItem(b, 0)->valueint, cJSON_GetArrayItem(b, 1)->valueint);
		for (int i = 0; i < 8; i++)
			for (int s = 0; s < 2; s++)
				r.attributes[i][s] = cJSON_GetArrayItem(cJSON_GetArrayItem(arr(it, "attributes"), i), s)->valueint;
		r.playable = num(it, "playable") != 0;
		r.beast = num(it, "beast") != 0;
		cJSON_ArrayForEach(b, arr(it, "spells"))
			r.spells.push_back(b->valuestring);
		g.races.push_back(r);
	});

	forSection(root, "birthsigns", [&](const cJSON* it) {
		BirthDef b;
		b.id = str(it, "id"); b.name = str(it, "name"); b.desc = str(it, "desc"); b.texture = str(it, "texture");
		const cJSON* s;
		cJSON_ArrayForEach(s, arr(it, "spells"))
			b.spells.push_back(s->valuestring);
		g.birthsigns.push_back(b);
	});

	forSection(root, "spells", [&](const cJSON* it) {
		SpellDef s;
		s.id = str(it, "id"); s.name = str(it, "name"); s.type = num(it, "type");
		s.cost = num(it, "cost"); s.flags = num(it, "flags");
		s.effects = readEffects(arr(it, "effects"));
		g.spells[it->string] = s;
	});
	forSection(root, "enchantments", [&](const cJSON* it) {
		SpellDef s;
		s.id = it->string;
		s.type = ENCH_ONCE + (int)num(it, "type");
		s.cost = num(it, "cost");
		s.charge = num(it, "charge");
		s.effects = readEffects(arr(it, "effects"));
		g.spells[it->string] = s;
	});
	cJSON_ArrayForEach(it, arr(root, "magic_effects"))
	{
		MagicEffectDef& me = g.magicEffects[atoi(it->string)];
		me = { (int)num(it, "school"), (float)num(it, "cost", 1.0), (int)num(it, "flags"), str(it, "name") };
		me.cvfx = str(it, "cvfx"); me.bvfx = str(it, "bvfx"); me.hvfx = str(it, "hvfx"); me.avfx = str(it, "avfx");
		me.snd[0] = str(it, "csnd"); me.snd[1] = str(it, "bsnd"); me.snd[2] = str(it, "hsnd"); me.snd[3] = str(it, "asnd");
		if (const cJSON* c = cJSON_GetObjectItem(it, "rgb"))
			if (cJSON_GetArraySize(c) == 3)
				me.rgb = (std::min(255, cJSON_GetArrayItem(c, 0)->valueint) << 16) | (std::min(255, cJSON_GetArrayItem(c, 1)->valueint) << 8)
					| std::min(255, cJSON_GetArrayItem(c, 2)->valueint);
	}

	cJSON_ArrayForEach(it, arr(root, "skills"))
	{
		SkillDef sd = { (int)num(it, "attribute"), (int)num(it, "specialization"), str(it, "desc"), { 1, 1, 1, 1 } };
		for (int k = 0; k < 4; k++)
			if (const cJSON* u = cJSON_GetArrayItem(arr(it, "use"), k))
				sd.use[k] = u->valuedouble;
		g.skills.push_back(sd);
	}

	cJSON_ArrayForEach(it, arr(root, "sounds"))
		g.sounds[it->string] = { str(it, "file"), (float)num(it, "volume", 1.0), (int)num(it, "min"), (int)num(it, "max") };
	forSection(root, "voices", [&](const cJSON* it) { g.voices[it->string] = it->valuestring; });
	cJSON_ArrayForEach(it, arr(root, "music_battle"))
		if (cJSON_IsString(it))
			g.battleMusic.push_back(it->valuestring);
	cJSON_ArrayForEach(it, arr(root, "music"))
		g.music.push_back(it->valuestring);

	for (int k = 0; k < 2; k++)
		cJSON_ArrayForEach(it, arr(root, k ? "skip_chargen_items" : "start_items"))
		{
			const cJSON* id = cJSON_GetArrayItem(it, 1);
			if (cJSON_IsString(id))
				(k ? g.skipChargenItems : g.startItems).emplace_back(cJSON_GetArrayItem(it, 0)->valueint, lower(id->valuestring));
		}
	g.waterSound = str(cJSON_GetObjectItem(root, "ambient"), "water");
	forSection(root, "ref_cells", [&](const cJSON* it0) {
		const cJSON* c;
		cJSON_ArrayForEach(c, it0)
			g.refCells[it0->string].push_back(c->valueint);
	});
	if (const cJSON* m = cJSON_GetObjectItem(root, "map"))
	{
		g.map.file = str(m, "file");
		g.map.originX = cJSON_GetArrayItem(arr(m, "origin"), 0)->valuedouble;
		g.map.originY = cJSON_GetArrayItem(arr(m, "origin"), 1)->valuedouble;
		g.map.unitsPerPixel = num(m, "units_per_pixel", 128);
		g.map.width = num(m, "width"); g.map.height = num(m, "height");
		g.map.texWidth = num(m, "tex_width"); g.map.texHeight = num(m, "tex_height");
		const cJSON* l;
		cJSON_ArrayForEach(l, arr(m, "labels"))
			g.map.labels.push_back({ str(l, "text"), (float)num(l, "x"), (float)num(l, "y") });
		if (const cJSON* d = cJSON_GetObjectItem(m, "detail"))
		{
			g.map.detailUnitsPerPixel = num(d, "units_per_pixel");
			g.map.detailWidth = num(d, "width"); g.map.detailHeight = num(d, "height");
			cJSON_ArrayForEach(l, arr(d, "tiles"))
			{
				MapTile t;
				t.file = str(l, "file");
				t.x = num(l, "x"); t.y = num(l, "y"); t.w = num(l, "w"); t.h = num(l, "h");
				t.ox = num(l, "ox"); t.oy = num(l, "oy");
				t.texWidth = num(l, "tex_width"); t.texHeight = num(l, "tex_height");
				g.map.tiles.push_back(t);
			}
		}
	}

	if (const cJSON* ui = section(root, "ui"))
	{
		UiThemeDef& t = g.ui;
		if (const cJSON* ic = cJSON_GetObjectItem(ui, "icons"))
		{
			t.iconSize = num(ic, "size", 32);
			t.iconPage = num(ic, "page", 256);
			const cJSON* p;
			cJSON_ArrayForEach(p, arr(ic, "pages"))
				t.iconPages.emplace_back(str(p, "file"), (int)num(p, "height"));
		}
		if (const cJSON* a = cJSON_GetObjectItem(ui, "atlas"))
		{
			t.atlasFile = str(a, "file");
			t.atlasW = num(a, "width"); t.atlasH = num(a, "height");
			const cJSON* p;
			cJSON_ArrayForEach(p, cJSON_GetObjectItem(a, "pieces"))
			{
				auto v = [&](int k) { const cJSON* e = cJSON_GetArrayItem(p, k); return e ? (u16)e->valueint : (u16)0; };
				t.pieces[p->string] = { v(0), v(1), v(2), v(3) };
			}
		}
		if (const cJSON* f = cJSON_GetObjectItem(ui, "font"))
		{
			t.fontFile = str(f, "file");
			t.fontW = num(f, "width"); t.fontH = num(f, "height");
			t.fontLine = num(f, "line");
			const cJSON* gl;
			cJSON_ArrayForEach(gl, cJSON_GetObjectItem(f, "glyphs"))
			{
				auto v = [&](int k) { const cJSON* e = cJSON_GetArrayItem(gl, k); return e ? (float)e->valuedouble : 0.0f; };
				t.glyphs[(u32)strtoul(gl->string, nullptr, 10)] = { (u16)v(0), (u16)v(1), (u16)v(2), (u16)v(3), v(4), v(5), v(6) };
			}
		}
		const cJSON* c;
		cJSON_ArrayForEach(c, cJSON_GetObjectItem(ui, "colors"))
		{
			auto v = [&](int k) { const cJSON* e = cJSON_GetArrayItem(c, k); return e ? (u32)(e->valueint & 255) : 0u; };
			t.colors[c->string] = 0xFF000000u | (v(2) << 16) | (v(1) << 8) | v(0);
		}
	}

	const cJSON* ts;
	if (const cJSON* wt = cJSON_GetObjectItem(root, "weather"))
	{
		static const char* kinds[4] = { "sky", "fog", "ambient", "sun" };
		for (int k = 0; k < 4; k++)
			for (int t = 0; t < 4; t++)
				for (int c = 0; c < 3; c++)
					if (const cJSON* v = cJSON_GetArrayItem(cJSON_GetArrayItem(cJSON_GetObjectItem(wt, kinds[k]), t), c))
						g.weather[k][t][c] = (float)v->valuedouble;
		g.hasWeather = true;
	}
	const cJSON* wty;
	cJSON_ArrayForEach(wty, cJSON_GetObjectItem(root, "weather_types"))
	{
		WeatherType t;
		t.name = str(wty, "name");
		static const char* kinds[4] = { "sky", "fog", "ambient", "sun" };
		for (int k = 0; k < 4; k++)
			for (int ti = 0; ti < 4; ti++)
				for (int c = 0; c < 3; c++)
					if (const cJSON* v = cJSON_GetArrayItem(cJSON_GetArrayItem(cJSON_GetObjectItem(wty, kinds[k]), ti), c))
						t.colors[k][ti][c] = (float)v->valuedouble;
		for (int k = 0; k < 2; k++)
			if (const cJSON* v = cJSON_GetArrayItem(cJSON_GetObjectItem(wty, "fog_depth"), k))
				t.fog[k] = (float)v->valuedouble;
		t.sound = str(wty, "sound");
		t.delta = (float)num(wty, "delta", t.delta);
		t.wind = (float)num(wty, "wind", t.wind);
		t.glare = (float)num(wty, "glare", t.glare);
		t.rainThreshold = (float)num(wty, "rain_threshold", t.rainThreshold);
		t.thunderFreq = (float)num(wty, "thunder_freq", t.thunderFreq);
		t.thunderThreshold = (float)num(wty, "thunder_threshold", t.thunderThreshold);
		t.flashDecrement = (float)num(wty, "flash_decrement", t.flashDecrement);
		t.cloudsMax = (float)num(wty, "clouds_max", t.cloudsMax);
		t.precip = num(wty, "precip", 0.0) != 0.0;
		g.weatherTypes.push_back(t);
	}
	g.weatherHours = (float)num(root, "weather_hours", 20.0);
	if (const cJSON* wc = cJSON_GetObjectItem(root, "weather_clock"))
	{
		WeatherClock& c = g.weatherClock;
		c.sunrise = (float)num(wc, "sunrise", c.sunrise);
		c.sunset = (float)num(wc, "sunset", c.sunset);
		c.sunriseDur = (float)num(wc, "sunrise_duration", c.sunriseDur);
		c.sunsetDur = (float)num(wc, "sunset_duration", c.sunsetDur);
		for (int k = 0; k < 4; k++)
			for (int j = 0; j < 4; j++)
				if (const cJSON* v = cJSON_GetArrayItem(cJSON_GetArrayItem(cJSON_GetObjectItem(wc, "windows"), k), j))
					c.win[k][j] = (float)v->valuedouble;
	}
	const cJSON* rg;
	cJSON_ArrayForEach(rg, cJSON_GetObjectItem(root, "regions"))
	{
		RegionDef r;
		r.name = str(rg, "name");
		r.sleep = str(rg, "sleep");
		{
			const cJSON* a;
			cJSON_ArrayForEach(a, cJSON_GetObjectItem(rg, "ambient"))
				r.ambient.emplace_back(cJSON_GetArrayItem(a, 0)->valuestring, cJSON_GetArrayItem(a, 1)->valueint);
		}
		for (int k = 0; k < 10; k++)
			if (const cJSON* v = cJSON_GetArrayItem(cJSON_GetObjectItem(rg, "chances"), k))
				r.chances[k] = v->valueint;
		g.regions[rg->string] = r;
	}
	const cJSON* lv;
	cJSON_ArrayForEach(lv, cJSON_GetObjectItem(root, "leveled"))
	{
		GameData::LeveledList& l = g.leveled[lv->string];
		l.all = num(lv, "all") != 0;
		l.none = (int)num(lv, "none");
		const cJSON* e;
		cJSON_ArrayForEach(e, cJSON_GetObjectItem(lv, "e"))
			l.entries.emplace_back(cJSON_GetArrayItem(e, 0)->valueint, cJSON_GetArrayItem(e, 1)->valuestring);
	}
	// leveled item lists (containers, inventories) share the creature lists' map: ids don't clash
	forSection(root, "leveled_items", [&](const cJSON* lv) {
		GameData::LeveledList& l = g.leveled[lv->string];
		l.all = num(lv, "all") != 0;
		l.each = num(lv, "each") != 0;
		l.none = (int)num(lv, "none");
		const cJSON* e;
		cJSON_ArrayForEach(e, cJSON_GetObjectItem(lv, "e"))
			l.entries.emplace_back(cJSON_GetArrayItem(e, 0)->valueint, cJSON_GetArrayItem(e, 1)->valuestring);
	});
	// Autocalc NPCs: the spells the engine picks for them (to cast in a fight, and to sell)
	int autoNpcs = 0;
	for (auto& a : g.actors)
	{
		if (!a.autocalc || a.creature)
			continue;
		for (auto& id : g.autoCalcSpells(a.attributes, a.skills, false))
		{
			std::string l = lower(id);
			if (std::find(a.spells.begin(), a.spells.end(), l) == a.spells.end())
				a.spells.push_back(l);
			auto sp = g.spells.find(l);
			if (sp == g.spells.end())
				continue;
			bool harm = false, heal = false;
			for (auto& e : sp->second.effects)
			{
				auto me = g.magicEffects.find(e.effect);
				harm |= me != g.magicEffects.end() && (me->second.flags & 0x10) && e.range > 0;
				heal |= e.effect == 75 && e.range == 0;
			}
			if ((harm || heal) && std::find(a.combatSpells.begin(), a.combatSpells.end(), l) == a.combatSpells.end())
				a.combatSpells.push_back(l);
		}
		autoNpcs++;
	}
	logf("game: autocalc spells for %d NPCs", autoNpcs);
	for (auto& t : g.topics)
		for (auto& info : t.infos)
			if (!info.whoId.empty())
				g.speakers.insert(lower(info.whoId));
	const cJSON* mk = cJSON_GetObjectItem(root, "markers");
	const cJSON* pm;
	cJSON_ArrayForEach(pm, cJSON_GetObjectItem(mk, "prison"))
	{
		GameData::PrisonMarker p = {};
		for (int k = 0; k < 3; k++)
			p.pos[k] = (float)cJSON_GetArrayItem(cJSON_GetObjectItem(pm, "pos"), k)->valuedouble;
		for (int k = 0; k < 4; k++)
			p.dest[k] = (float)cJSON_GetArrayItem(cJSON_GetObjectItem(pm, "dest"), k)->valuedouble;
		p.cell = str(pm, "cell");
		g.prisonMarkers.push_back(p);
	}
	for (int k = 0; k < 2; k++)
	{
		const cJSON* m;
		cJSON_ArrayForEach(m, cJSON_GetObjectItem(mk, k ? "temple" : "divine"))
		{
			std::vector<float> v;
			const cJSON* x;
			cJSON_ArrayForEach(x, m)
				v.push_back((float)x->valuedouble);
			if (v.size() == 4)
				(k ? g.templeMarkers : g.divineMarkers).push_back(v);
		}
	}
	const cJSON* sp;
	cJSON_ArrayForEach(sp, cJSON_GetObjectItem(root, "spawn"))
		g.spawns[sp->string] = { (int)num(sp, "actor"), str(sp, "lib"), str(sp, "type"), (int)num(sp, "skeleton") };
	cJSON_ArrayForEach(ts, cJSON_GetObjectItem(root, "town_spawns"))
	{
		std::vector<float> v;
		const cJSON* n;
		cJSON_ArrayForEach(n, ts)
			v.push_back((float)n->valuedouble);
		if (v.size() == 4)
			g.townSpawns[lower(ts->string)] = v;
	}
	if (const cJSON* art = cJSON_GetObjectItem(root, "art"))
	{
		auto one = [&](const cJSON* a) { ArtRef r; r.file = str(a, "file"); r.w = (int)num(a, "w"); r.h = (int)num(a, "h"); return r; };
		const cJSON* a;
		cJSON_ArrayForEach(a, cJSON_GetObjectItem(art, "splash"))
			g.art.splash.push_back(one(a));
		cJSON_ArrayForEach(a, cJSON_GetObjectItem(art, "levelup"))
			g.art.levelup[lower(a->string)] = one(a);
		cJSON_ArrayForEach(a, cJSON_GetObjectItem(art, "bookart"))
			g.art.bookart[lower(a->string)] = one(a);
		cJSON_ArrayForEach(a, cJSON_GetObjectItem(art, "hud"))
			g.art.hud[lower(a->string)] = one(a);
	}
	if (const cJSON* sb = cJSON_GetObjectItem(root, "sky_bodies"))
	{
		SkyBodiesDef& d = g.skyBodies;
		d.sun = str(sb, "sun"); d.glare = str(sb, "glare"); d.stars = str(sb, "stars");
		for (int k = 0; k < 8; k++)
		{
			const cJSON* a = cJSON_GetArrayItem(cJSON_GetObjectItem(sb, "masser"), k);
			const cJSON* b = cJSON_GetArrayItem(cJSON_GetObjectItem(sb, "secunda"), k);
			d.masser[k] = cJSON_IsString(a) ? a->valuestring : "";
			d.secunda[k] = cJSON_IsString(b) ? b->valuestring : "";
		}
		const char* names[2] = { "masser", "secunda" };
		for (int m = 0; m < 2; m++)
			if (const cJSON* md = cJSON_GetObjectItem(cJSON_GetObjectItem(sb, "moons"), names[m]))
			{
				MoonDef& o = d.moons[m];
				o.size = num(md, "Size", o.size); o.fadeInStart = num(md, "Fade In Start", o.fadeInStart);
				o.fadeInFinish = num(md, "Fade In Finish", o.fadeInFinish); o.fadeOutStart = num(md, "Fade Out Start", o.fadeOutStart);
				o.fadeOutFinish = num(md, "Fade Out Finish", o.fadeOutFinish); o.axisOffset = num(md, "Axis Offset", o.axisOffset);
				o.speed = num(md, "Speed", o.speed); o.dailyIncrement = num(md, "Daily Increment", o.dailyIncrement);
			}
		d.sunrise = num(sb, "sunrise", 6); d.sunset = num(sb, "sunset", 18);
	}
	if (const cJSON* fp = section(root, "firstperson"))
	{
		const cJSON* race;
		cJSON_ArrayForEach(race, cJSON_GetObjectItem(fp, "races"))
			for (int female = 0; female < 2; female++)
			{
				const cJSON* slot;
				cJSON_ArrayForEach(slot, cJSON_GetObjectItem(race, female ? "f" : "m"))
					g.firstPerson.races[female][race->string][atoi(slot->string)] = slot->valuestring;
			}
		const cJSON* vf;
		cJSON_ArrayForEach(vf, cJSON_GetObjectItem(fp, "vfx"))
			if (cJSON_IsString(vf))
				g.firstPerson.vfx[vf->string] = vf->valuestring;
		if (const cJSON* tp = cJSON_GetObjectItem(fp, "third"))
		{
			ThirdPersonDef& t = g.firstPerson.third;
			cJSON_ArrayForEach(race, cJSON_GetObjectItem(tp, "races"))
				for (int female = 0; female < 2; female++)
				{
					const cJSON* slot;
					cJSON_ArrayForEach(slot, cJSON_GetObjectItem(race, female ? "f" : "m"))
						t.races[female][race->string][atoi(slot->string)] = slot->valuestring;
				}
			for (int hair = 0; hair < 2; hair++)
				cJSON_ArrayForEach(race, cJSON_GetObjectItem(tp, hair ? "hairs" : "heads"))
					for (int female = 0; female < 2; female++)
					{
						const cJSON* e;
						auto& list = (hair ? t.hairs : t.heads)[female][race->string];
						cJSON_ArrayForEach(e, cJSON_GetObjectItem(race, female ? "f" : "m"))
							list.emplace_back(cJSON_GetArrayItem(e, 0)->valuestring, cJSON_GetArrayItem(e, 1)->valuestring);
					}
			const cJSON* ti;
			cJSON_ArrayForEach(ti, cJSON_GetObjectItem(tp, "items"))
			{
				auto& slots = t.items[lower(ti->string)];
				const cJSON* slot;
				cJSON_ArrayForEach(slot, ti)
					slots[atoi(slot->string)] = { cJSON_GetArrayItem(slot, 0)->valuestring, cJSON_GetArrayItem(slot, 1)->valuestring };
			}
		}
		const cJSON* item;
		cJSON_ArrayForEach(item, cJSON_GetObjectItem(fp, "items"))
		{
			FpItem& fi = g.firstPerson.items[lower(item->string)];
			fi.model = str(item, "model");
			const cJSON* slot;
			cJSON_ArrayForEach(slot, cJSON_GetObjectItem(item, "slots"))
				fi.slots[atoi(slot->string)] = { cJSON_GetArrayItem(slot, 0)->valuestring, cJSON_GetArrayItem(slot, 1)->valuestring };
		}
	}

	const cJSON* lu;
	cJSON_ArrayForEach(lu, cJSON_GetObjectItem(root, "levelup"))
		if (cJSON_IsString(lu))
			g.levelUpText[lower(lu->string)] = lu->valuestring;
	cJSON_ArrayForEach(it, arr(root, "quiz"))
	{
		QuizQuestion q;
		q.question = str(it, "question");
		q.sound = str(it, "sound");
		for (int i = 0; i < 3; i++)
		{
			const cJSON* a = cJSON_GetArrayItem(arr(it, "answers"), i);
			q.answers[i] = cJSON_IsString(a) ? a->valuestring : "";
		}
		g.quiz.push_back(q);
	}

	forSection(root, "factions", [&](const cJSON* it) {
		FactionDef& f = g.factions[it->string];
		f.name = str(it, "name");
		const cJSON* rk;
		cJSON_ArrayForEach(rk, arr(it, "ranks"))
			f.ranks.push_back(rk->valuestring);
		for (int k = 0; k < 2; k++)
			if (const cJSON* v = cJSON_GetArrayItem(arr(it, "attrs"), k))
				f.attrs[k] = v->valueint;
		for (int r = 0; r < 10; r++)
			for (int k = 0; k < 5; k++)
				if (const cJSON* v = cJSON_GetArrayItem(cJSON_GetArrayItem(arr(it, "reqs"), r), k))
					f.reqs[r][k] = v->valueint;
		cJSON_ArrayForEach(rk, arr(it, "skills"))
			f.skills.push_back(rk->valueint);
		cJSON_ArrayForEach(rk, cJSON_GetObjectItem(it, "reactions"))
			f.reactions[lower(rk->string)] = rk->valueint;
	});

	sectionDone();
	cJSON_Delete(root);
	logf("game: %d objects, %d scripts, %d topics, %d journals, %d sounds, %d voices",
		(int)g.objects.size(), (int)g.scripts.size(), (int)g.topics.size(), (int)g.journals.size(),
		(int)g.sounds.size(), (int)g.voices.size());
	return true;
}

u32 GameData::enchantGlow(const std::string& itemId) const
{
	const Object* o = object(itemId);
	if (!o || o->ench.empty())
		return 0;
	auto sp = spells.find(o->ench);
	if (sp == spells.end() || sp->second.effects.empty())
		return 0;
	auto me = magicEffects.find(sp->second.effects[0].effect);
	return me != magicEffects.end() ? me->second.rgb : 0;
}
