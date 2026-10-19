#include "components/interpreter/interpreter.hpp"

#include <optional>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_set>

#include "components/debug/debuglog.hpp"
#include "mwworld/worldimp.hpp"
#include "components/interpreter/interpreterhelpers.hpp"

int scriptFunctionId(const std::string& name)
{
	for (int i = 1; i < F_COUNT; i++)
		if (name == kFunctionNames[i])
			return i;
	return F_UNKNOWN;
}

static std::unordered_set<std::string> s_reportedUnknown;

// A variable of a script instance and its declared type ('s' short, 'l' long, 'f' float)
static float* instVar(ScriptInstance& si, const std::string& name, char* type)
{
	int k = si.script ? si.script->localIndex(name) : -1;
	if (k < 0 || k >= (int)si.locals.size())
		return nullptr;
	if (type && k < (int)si.script->localTypes.size())
		*type = si.script->localTypes[k];
	return &si.locals[k];
}

static float* localVar(Ctx& c, const Node& n, char* type = nullptr)
{
	if (c.inst < 0)
		return nullptr;
	ScriptInstance& si = c.w.scripts[c.inst];
	if (n.func >= 0 && n.func < (int)si.locals.size())
	{
		if (type && n.func < (int)si.script->localTypes.size())
			*type = si.script->localTypes[n.func];
		return &si.locals[n.func];
	}
	return instVar(si, n.a, type);
}

// What a short / long variable holds: whole numbers, a short in 16 bits (OpenMW's Type_Short)
static float storeAs(char type, float v)
{
	if (type == 'f')
		return v;
	v = truncf(fmaxf(-2147483648.0f, fminf(2147483520.0f, v)));
	return type == 's' ? (float)(short)(int)v : v;
}

// ScriptName.var is a global script's variable, id.var one of that object's script
static float* remoteVar(Ctx& c, const std::string& refId, const std::string& var, char* type = nullptr)
{
	for (auto& s : c.w.scripts)
		if (s.ref < 0 && s.item.empty() && s.script && lower(s.script->name) == refId)
			if (float* v = instVar(s, var, type))
				return v;
	int r = c.w.findRefAnywhere(refId);
	if (r < 0 || c.w.refs[r].script < 0)
		return nullptr;
	return instVar(c.w.scripts[c.w.refs[r].script], var, type);
}

std::string argStr(Ctx& c, const Node& call, size_t i)
{
	if (i >= call.kids.size())
		return "";
	const Node& a = call.kids[i];
	if (a.kind == N_STR)
		return a.a;
	char buf[32];
	float v = eval(c, a);
	if (v == floorf(v))
		snprintf(buf, sizeof(buf), "%d", (int)v);
	else
		snprintf(buf, sizeof(buf), "%g", v);
	return buf;
}

// The player's cell as GetPCCell and ^Cell name it: an unnamed exterior cell goes by its region's name
std::string displayCellName(World& w)
{
	if (w.current < 0)
		return "";
	const LevelCell& cd = w.cells[w.current];
	if (cd.interior || !cd.name.empty() || cd.region.empty())
		return cd.name;
	auto rg = w.game.regions.find(cd.region);
	if (rg != w.game.regions.end() && !rg->second.name.empty())
		return rg->second.name;
	return cd.region;
}

// A MessageBox's ^ and % names (OpenMW's fixDefinesMsgBox): the player's name, race, class, cell, bounty, a
// global's value. The ones that only mean something in dialogue (faction, rank) come out empty
static std::string messageDefines(World& w, const std::string& in)
{
	const RaceDef* rc = w.race();
	const ClassDef* cl = w.playerClass();
	struct { const char* key; std::string value; } vars[] = {
		{ "actionactivate", "A" },                    // the 3DS's button
		{ "pccrimelevel", std::to_string(w.bounty) },
		{ "nextpcrank", "" }, { "pcnextrank", "" }, { "faction", "" },
		{ "pcclass", cl ? cl->name : "" }, { "pcname", w.stats.name }, { "pcrace", rc ? rc->name : "" },
		{ "pcrank", "" }, { "class", cl ? cl->name : "" }, { "cell", displayCellName(w) },
		{ "race", rc ? rc->name : "" }, { "rank", "" }, { "name", w.stats.name },
	};
	std::string out;
	for (size_t i = 0; i < in.size(); i++)
	{
		if (in[i] != '%' && in[i] != '^')
		{
			out += in[i];
			continue;
		}
		std::string rest = lower(in.substr(i + 1));
		bool found = false;
		for (auto& v : vars)
		{
			size_t len = strlen(v.key);
			if (rest.compare(0, len, v.key) == 0)
			{
				out += v.value;
				i += len;
				found = true;
				break;
			}
		}
		if (!found)
		{
			// a global variable, the longest name that fits
			const std::string* best = nullptr;
			for (auto& g : w.globals)
				if (!g.first.empty() && rest.compare(0, g.first.size(), lower(g.first)) == 0 && (!best || g.first.size() > best->size()))
					best = &g.first;
			if (best)
			{
				auto gd = w.game.globals.find(*best);
				float v = w.globals[*best];
				char buf[32];
				if (gd != w.game.globals.end() && gd->second.type != 'f')
					snprintf(buf, sizeof(buf), "%d", (int)v);
				else
					snprintf(buf, sizeof(buf), "%g", v);
				out += buf;
				i += best->size();
				found = true;
			}
		}
		if (!found)
			out += in[i];
	}
	return out;
}

// Morrowind's MessageBox: format string, one argument per % specifier, then button labels
void messageBox(Ctx& c, const Node& n)
{
	std::string fmt = argStr(c, n, 0), text;
	size_t arg = 1;
	for (size_t i = 0; i < fmt.size(); i++)
	{
		if (fmt[i] != '%')
		{
			text += fmt[i];
			continue;
		}
		if (i + 1 < fmt.size() && fmt[i + 1] == '%')
		{
			text += '%';
			i++;
			continue;
		}
		size_t j = i + 1;
		while (j < fmt.size() && (fmt[j] == '.' || (fmt[j] >= '0' && fmt[j] <= '9')))
			j++;
		if (j < fmt.size() && strchr("gfdis", fmt[j]))
		{
			std::string spec = fmt.substr(i, j - i + 1);
			char buf[64];
			if (fmt[j] == 's')
				text += argStr(c, n, arg++);
			else if (fmt[j] == 'd' || fmt[j] == 'i')
			{
				// an integer slot: a float argument is cut to a whole number
				spec[spec.size() - 1] = 'd';
				snprintf(buf, sizeof(buf), spec.c_str(), (int)argNum(c, n, arg++));
				text += buf;
			}
			else
			{
				snprintf(buf, sizeof(buf), spec.c_str(), (double)argNum(c, n, arg++));
				text += buf;
			}
			i = j;
		}
		else
			text += '%';
	}
	text = messageDefines(c.w, text);
	std::vector<std::string> buttons;
	for (size_t i = arg; i < n.kids.size(); i++)
		buttons.push_back(argStr(c, n, i));
	c.host.messageBox(text, buttons);
}

static int targetRef(Ctx& c, const Node& n, bool* isPlayer)
{
	*isPlayer = false;
	if (n.a.empty())
		return c.self;
	if (n.a == "player")
	{
		*isPlayer = true;
		return -1;
	}
	return c.w.findRefAnywhere(n.a);
}

// Stat functions, by name: Get / Set / Mod + an attribute or skill (Strength, LongBlade...), and Get /
// Set / Mod / ModCurrent + Health, Magicka or Fatigue. The player's go through the growth bonuses
// (recomputeStats keeps current values); an NPC's attributes and skills read from its record.

// GetEffect's argument: an effect's GMST name (sEffectWaterWalking) -> its index
int magicEffectIndex(const std::string& arg)
{
	static const char* names[] = { "waterbreathing", "swiftswim", "waterwalking", "shield", "fireshield",
		"lightningshield", "frostshield", "burden", "feather", "jump", "levitate", "slowfall", "lock", "open",
		"firedamage", "shockdamage", "frostdamage", "drainattribute", "drainhealth", "drainspellpoints",
		"drainfatigue", "drainskill", "damageattribute", "damagehealth", "damagemagicka", "damagefatigue",
		"damageskill", "poison", "weaknesstofire", "weaknesstofrost", "weaknesstoshock", "weaknesstomagicka",
		"weaknesstocommondisease", "weaknesstoblightdisease", "weaknesstocorprusdisease", "weaknesstopoison",
		"weaknesstonormalweapons", "disintegrateweapon", "disintegratearmor", "invisibility", "chameleon",
		"light", "sanctuary", "nighteye", "charm", "paralyze", "silence", "blind", "sound", "calmhumanoid",
		"calmcreature", "frenzyhumanoid", "frenzycreature", "demoralizehumanoid", "demoralizecreature",
		"rallyhumanoid", "rallycreature", "dispel", "soultrap", "telekinesis", "mark", "recall",
		"divineintervention", "almsiviintervention", "detectanimal", "detectenchantment", "detectkey",
		"spellabsorption", "reflect", "curecommondisease", "cureblightdisease", "curecorprusdisease",
		"curepoison", "cureparalyzation", "restoreattribute", "restorehealth", "restorespellpoints",
		"restorefatigue", "restoreskill", "fortifyattribute", "fortifyhealth", "fortifyspellpoints",
		"fortifyfatigue", "fortifyskill", "fortifymagickamultiplier", "absorbattribute", "absorbhealth",
		"absorbspellpoints", "absorbfatigue", "absorbskill", "resistfire", "resistfrost", "resistshock",
		"resistmagicka", "resistcommondisease", "resistblightdisease", "resistcorprusdisease", "resistpoison",
		"resistnormalweapons", "resistparalysis" };
	static const char* more[] = { "removecurse", "turnundead", "summonscamp", "summonclannfear", "summondaedroth",
		"summondremora", "summonancestralghost", "summonskeletalminion", "summonbonewalker",
		"summongreaterbonewalker", "summonbonelord", "summonwingedtwilight", "summonhunger", "summongoldensaint",
		"summonflameatronach", "summonfrostatronach", "summonstormatronach", "fortifyattackbonus", "commandcreature",
		"commandhumanoid", "bounddagger", "boundlongsword", "boundmace", "boundbattleaxe", "boundspear", "boundlongbow",
		"extraspell", "boundcuirass", "boundhelm", "boundboots", "boundshield", "boundgloves", "corprus", "vampirism",
		"summoncenturionsphere", "sundamage", "stuntedmagicka" };    // effects 100 on
	std::string a = lower(arg);
	if (a.compare(0, 7, "seffect") == 0)
		a = a.substr(7);
	if (!a.empty() && isdigit((unsigned char)a[0]))
		return atoi(a.c_str()) <= 136 ? atoi(a.c_str()) : -1;
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if (a == names[i])
			return i;
	for (size_t i = 0; i < sizeof(more) / sizeof(more[0]); i++)
		if (a == more[i])
			return 100 + i;
	return -1;
}

// A notify GMST ("%s has been added ...", "%d %s has been added ..."): the count, then the name
std::string gmstFormat(const std::string& fmt, int count, const std::string& name)
{
	std::string out;
	bool usedCount = false;
	for (size_t i = 0; i < fmt.size(); i++)
	{
		if (fmt[i] == '%' && i + 1 < fmt.size() && (fmt[i + 1] == 'd' || fmt[i + 1] == 's'))
		{
			if (fmt[i + 1] == 'd' && !usedCount)
			{
				out += std::to_string(count);
				usedCount = true;
			}
			else
				out += name;
			i++;
		}
		else
			out += fmt[i];
	}
	return out;
}

// Move's offset along an object's own axes (OpenMW: the object's attitude; an actor's is its turn about Z only)
void localToWorld(const float rot[3], bool turnOnly, float v[3])
{
	// each turn is clockwise seen along its axis, X first, then Y, then Z
	if (!turnOnly)
	{
		float cx = cosf(rot[0]), sx = sinf(rot[0]), y = v[1] * cx + v[2] * sx, z = -v[1] * sx + v[2] * cx;
		v[1] = y;
		v[2] = z;
		float cy = cosf(rot[1]), sy = sinf(rot[1]), x = v[0] * cy - v[2] * sy;
		z = v[0] * sy + v[2] * cy;
		v[0] = x;
		v[2] = z;
	}
	float cz = cosf(rot[2]), sz = sinf(rot[2]), x = v[0] * cz + v[1] * sz, y = -v[0] * sz + v[1] * cz;
	v[0] = x;
	v[1] = y;
}

float call(Ctx& c, const Node& n)
{
	World& w = c.w;
	// Enabling / disabling something in a cell not read yet needn't read it
	if ((n.func == F_ENABLE || n.func == F_DISABLE) && !n.a.empty() && n.a != "player"
		&& w.deferEnable(n.a, n.func == F_ENABLE))
		return 0.0f;
	bool onPlayer = false;
	int ref = targetRef(c, n, &onPlayer);
	// A call on a named object that isn't in this cell does nothing
	if (!n.a.empty() && !onPlayer && ref < 0)
		return 0.0f;
	Ref* r = ref >= 0 ? &w.refs[ref] : nullptr;

	for (auto part : { aiExtensions, animationExtensions, cellExtensions, containerExtensions, controlExtensions, dialogueExtensions, guiExtensions, skyExtensions, soundExtensions, statsExtensions, transformationExtensions, miscExtensions })
		if (std::optional<float> v = part(c, n, w, onPlayer, ref, r))
			return *v;
	float v;
	if (statFunction(c, n, onPlayer, r, ref, &v))
		return v;
	if (s_reportedUnknown.insert(n.b).second)
		logf("script: unsupported function %s", n.b.c_str());
	monitorOnce(("fn:" + n.b).c_str(), "script uses unsupported function %s", n.b.c_str());
	return 0.0f;
}

float eval(Ctx& c, const Node& n)
{
	switch (n.kind)
	{
	case N_NUM: return n.num;
	case N_STR: return (float)atof(n.a.c_str());
	case N_LOCAL:
	{
		float* v = localVar(c, n);
		if (v)
			return *v;
		auto g = c.w.globals.find(n.a);    // dialogue snippets may name globals the compiler took for locals
		return g == c.w.globals.end() ? 0.0f : g->second;
	}
	case N_GLOBAL:
	{
		auto g = c.w.globals.find(n.a);
		return g == c.w.globals.end() ? 0.0f : g->second;
	}
	case N_REMOTE:
	{
		float* v = remoteVar(c, n.a, n.b);
		return v ? *v : 0.0f;
	}
	case N_CALL: return call(c, n);
	case N_NEG: return -eval(c, n.kids[0]);
	case N_OP:
	{
		float a = eval(c, n.kids[0]), b = eval(c, n.kids[1]);
		switch (n.op)
		{
		case OP_ADD: return a + b;
		case OP_SUB: return a - b;
		case OP_MUL: return a * b;
		case OP_DIV: return b != 0.0f ? a / b : 0.0f;
		case OP_IDIV:
		{
			int d = (int)b;       // two whole numbers divide as whole numbers (7 / 2 is 3)
			return d != 0 ? (float)((int)a / d) : 0.0f;
		}
		case OP_EQ: return a == b;
		case OP_NE: return a != b;
		case OP_LT: return a < b;
		case OP_LE: return a <= b;
		case OP_GT: return a > b;
		case OP_GE: return a >= b;
		}
		return 0.0f;
	}
	default: return 0.0f;
	}
}

void assign(Ctx& c, const Node& target, float value)
{
	if (target.kind == N_LOCAL)
	{
		char type = 'f';
		float* v = localVar(c, target, &type);
		if (v)
			*v = storeAs(type, value);
		else
			c.w.globals[target.a] = value;
		return;
	}
	if (target.kind == N_GLOBAL)
	{
		auto it = c.w.game.globals.find(target.a);
		// short / long globals hold whole numbers
		if (it != c.w.game.globals.end() && it->second.type != 'f')
			value = truncf(value);
		c.w.setGlobal(target.a, value);
		return;
	}
	if (target.kind == N_REMOTE)
	{
		char type = 'f';
		if (float* v = remoteVar(c, target.a, target.b, &type))
			*v = storeAs(type, value);
	}
}

static void exec(Ctx& c, const Node& block)
{
	for (const Node& s : block.kids)
	{
		if (c.returned)
			return;
		switch (s.kind)
		{
		case N_SET: assign(c, s.kids[0], eval(c, s.kids[1])); break;
		case N_CALL: call(c, s); break;
		case N_RET: c.returned = true; return;
		case N_IF:
		{
			size_t k = 0;
			bool taken = false;
			for (; k + 1 < s.kids.size(); k += 2)
				if (eval(c, s.kids[k]) != 0.0f)
				{
					exec(c, s.kids[k + 1]);
					taken = true;
					break;
				}
			if (!taken && s.kids.size() % 2 == 1)
				exec(c, s.kids.back());
			break;
		}
		case N_WHILE:
		{
			int guard = 0;
			while (!c.returned && eval(c, s.kids[0]) != 0.0f && guard++ < 1000)
				exec(c, s.kids[1]);
			break;
		}
		default: break;
		}
	}
}

void scriptsRun(World& w, ScriptHost& host, float dt, bool globalOnly)
{
	// Index loop: StartScript / AddItem can append instances while we run. The local scripts run first,
	// then the global ones (OpenMW's frame)
	for (int pass = globalOnly ? 1 : 0; pass < 2; pass++)
	{
		for (size_t i = 0; i < w.scripts.size(); i++)
		{
			bool global = w.scripts[i].ref < 0 && w.scripts[i].item.empty();
			if (global != (pass == 1) || !w.scripts[i].running)
				continue;
			// Scripts on references run while their cell is loaded and they haven't been taken (carried
			// items' own instances always run)
			int ref = w.scripts[i].ref;
			if (ref >= 0 && w.scripts[i].item.empty() && (!w.active(ref) || w.refs[ref].pickedUp))
				continue;
			// (a global script started on an object runs with it as its reference; one whose cell went out of
			// memory is read back, as a saved game's are)
			if (w.scripts[i].target < 0 && !w.scripts[i].targetCell.empty())
			{
				int tc = w.cellIndex(w.scripts[i].targetCell);
				if (tc >= 0 && w.ensureRefs(tc) && w.scripts[i].targetIndex < w.cells[tc].refCount)
					w.scripts[i].target = w.cells[tc].refBase + w.scripts[i].targetIndex;
				w.scripts[i].targetCell.clear();
			}
			Ctx c{ w, host, (int)i, ref >= 0 || !w.scripts[i].item.empty() ? ref : w.scripts[i].target, dt };
			exec(c, w.scripts[i].script->body);
			// OnActivate lasts one frame; OnPCAdd / OnPCEquip stay set until the script clears them
			w.scripts[i].activated = false;
		}
	}
	w.scriptCell = w.current;
}

void scriptRunSnippet(World& w, ScriptHost& host, const Node& block, int speakerRef)
{
	int inst = speakerRef >= 0 ? w.refs[speakerRef].script : -1;
	Ctx c{ w, host, inst, speakerRef, 0.0f };
	exec(c, block);
}

float scriptEval(World& w, ScriptHost& host, const Node& expr, int selfRef)
{
	int inst = selfRef >= 0 ? w.refs[selfRef].script : -1;
	Ctx c{ w, host, inst, selfRef, 0.0f };
	return eval(c, expr);
}

// What call() runs: the table, and statFunction's families (Get / Set / Mod + attribute or skill; Get / Set / Mod /
// ModCurrent + health, magicka, fatigue; Get...GetRatio; GetLevel / SetLevel; ModPCCrimeLevel)
bool scriptFunctionSupported(const std::string& name)
{
	if (scriptFunctionId(name) != F_UNKNOWN)
		return true;
	if (name == "getlevel" || name == "setlevel" || name == "modpccrimelevel")
		return true;
	int op = name.compare(0, 10, "modcurrent") == 0 ? 3 : name.compare(0, 3, "get") == 0 ? 0
		: name.compare(0, 3, "set") == 0 ? 1 : name.compare(0, 3, "mod") == 0 ? 2 : -1;
	if (op < 0)
		return false;
	std::string stat = name.substr(op == 3 ? 10 : 3);
	if (op == 0 && stat.size() > 8 && stat.compare(stat.size() - 8, 8, "getratio") == 0)
		stat.resize(stat.size() - 8);
	else if (op == 3 && stat != "health" && stat != "magicka" && stat != "fatigue")
		return false;
	if (stat == "health" || stat == "magicka" || stat == "fatigue")
		return true;
	for (const char* a : kAttrFuncs)
		if (stat == a)
			return true;
	for (const char* k : kSkillFuncs)
		if (stat == k)
			return true;
	return false;
}
