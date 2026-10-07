#include "script.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_set>

#include "game.h"
#include "actors.h"
#include "log.h"
#include "world.h"

#define SCRIPT_FUNCTIONS(X) \
	X(UNKNOWN, "") X(MENUMODE, "menumode") X(ONACTIVATE, "onactivate") X(ACTIVATE, "activate") \
	X(GETHEALTH, "gethealth") X(SETHELLO, "sethello") X(ENABLE, "enable") X(DISABLE, "disable") \
	X(GETDISABLED, "getdisabled") X(GETDISTANCE, "getdistance") X(SAY, "say") X(SAYDONE, "saydone") \
	X(DISABLEPLAYERCONTROLS, "disableplayercontrols") X(ENABLEPLAYERCONTROLS, "enableplayercontrols") \
	X(DISABLEPLAYERJUMPING, "disableplayerjumping") X(ENABLEPLAYERJUMPING, "enableplayerjumping") \
	X(ENABLEPLAYERFIGHTING, "enableplayerfighting") X(DISABLEPLAYERFIGHTING, "disableplayerfighting") \
	X(ENABLEPLAYERMAGIC, "enableplayermagic") X(DISABLEPLAYERMAGIC, "disableplayermagic") \
	X(ENABLEPLAYERVIEWSWITCH, "enableplayerviewswitch") X(DISABLEPLAYERVIEWSWITCH, "disableplayerviewswitch") \
	X(ENABLEVANITYMODE, "enablevanitymode") X(DISABLEVANITYMODE, "disablevanitymode") \
	X(ENABLECLASSMENU, "enableclassmenu") X(ENABLEBIRTHMENU, "enablebirthmenu") \
	X(ENABLESTATREVIEWMENU, "enablestatreviewmenu") X(ENABLERACEMENU, "enableracemenu") \
	X(ENABLENAMEMENU, "enablenamemenu") X(ENABLESTATSMENU, "enablestatsmenu") \
	X(ENABLEINVENTORYMENU, "enableinventorymenu") X(ENABLEMAGICMENU, "enablemagicmenu") \
	X(ENABLEMAPMENU, "enablemapmenu") X(ENABLEREST, "enablerest") X(SHOWRESTMENU, "showrestmenu") \
	X(STARTSCRIPT, "startscript") X(STOPSCRIPT, "stopscript") X(SCRIPTRUNNING, "scriptrunning") \
	X(MESSAGEBOX, "messagebox") X(GETBUTTONPRESSED, "getbuttonpressed") X(XBOX, "xbox") \
	X(GETSECONDSPASSED, "getsecondspassed") X(GETITEMCOUNT, "getitemcount") X(ADDITEM, "additem") \
	X(REMOVEITEM, "removeitem") X(UNLOCK, "unlock") X(LOCK, "lock") X(GETLOCKED, "getlocked") \
	X(PLAYSOUND, "playsound") X(PLAYSOUND3D, "playsound3d") X(PLAYSOUNDVP, "playsoundvp") \
	X(PLAYSOUND3DVP, "playsound3dvp") X(GETSTANDINGPC, "getstandingpc") X(GETRACE, "getrace") \
	X(JOURNAL, "journal") X(GETJOURNALINDEX, "getjournalindex") X(SETJOURNALINDEX, "setjournalindex") \
	X(ADDTOPIC, "addtopic") X(GOODBYE, "goodbye") X(CHOICE, "choice") X(FORCEGREETING, "forcegreeting") \
	X(MODDISPOSITION, "moddisposition") X(SETDISPOSITION, "setdisposition") X(GETDISPOSITION, "getdisposition") \
	X(RANDOM, "random") X(GETPCCELL, "getpccell") X(POSITIONCELL, "positioncell") X(CHANGEWEATHER, "changeweather") \
	X(SHOWMAP, "showmap") X(CLEARINFOACTOR, "clearinfoactor") X(EQUIP, "equip") X(HASITEMEQUIPPED, "hasitemequipped") \
	X(GETPOS, "getpos") X(GETANGLE, "getangle") X(ADDSPELL, "addspell") X(REMOVESPELL, "removespell") \
	X(GETSPELL, "getspell") X(MODPCFACREP, "modpcfacrep") X(STREAMMUSIC, "streammusic") X(FADEIN, "fadein") \
	X(FADEOUT, "fadeout") X(STOPSOUND, "stopsound") X(GETSOUNDPLAYING, "getsoundplaying") \
	X(GETCURRENTTIME, "getcurrenttime") X(PCJOINFACTION, "pcjoinfaction") X(GETPCRANK, "getpcrank") \
	X(GETPCCRIMELEVEL, "getpccrimelevel") X(SETPCCRIMELEVEL, "setpccrimelevel") X(PAYFINE, "payfine") \
	X(PAYFINETHIEF, "payfinethief") X(ONDEATH, "ondeath") X(GETDEADCOUNT, "getdeadcount") X(STARTCOMBAT, "startcombat") X(STOPCOMBAT, "stopcombat") \
	X(PCRAISERANK, "pcraiserank") X(PCLOWERRANK, "pclowerrank") X(PCEXPELLED, "pcexpelled") \
	X(GETPCFACREP, "getpcfacrep") X(SETPCFACREP, "setpcfacrep") X(MODREPUTATION, "modreputation") \
	X(SETREPUTATION, "setreputation") X(GETREPUTATION, "getreputation") X(MODFIGHT, "modfight") \
	X(SETFIGHT, "setfight") X(GETFIGHT, "getfight") X(GETPCSLEEP, "getpcsleep") X(WAKEUPPC, "wakeuppc") \
	X(PCEXPELL, "pcexpell") X(PCCLEAREXPELLED, "pcclearexpelled") X(CELLCHANGED, "cellchanged") \
	X(MODFACTIONREACTION, "modfactionreaction") X(SETFACTIONREACTION, "setfactionreaction") \
	X(GETFACTIONREACTION, "getfactionreaction") X(DONTSAVEOBJECT, "dontsaveobject") X(PLAYBINK, "playbink") \
	X(ENABLETELEPORTING, "enableteleporting") X(DISABLETELEPORTING, "disableteleporting") \
	X(AIWANDER, "aiwander") X(AITRAVEL, "aitravel") X(AIFOLLOW, "aifollow") X(AIFOLLOWCELL, "aifollowcell") \
	X(AIESCORT, "aiescort") X(AIESCORTCELL, "aiescortcell") X(AIACTIVATE, "aiactivate") \
	X(GETAIPACKAGEDONE, "getaipackagedone") X(GETCURRENTAIPACKAGE, "getcurrentaipackage") \
	X(ONMURDER, "onmurder") X(GETCOMMONDISEASE, "getcommondisease") X(GETBLIGHTDISEASE, "getblightdisease") \
	X(CAST, "cast") X(PLAYGROUP, "playgroup") X(LOOPGROUP, "loopgroup") X(ROTATE, "rotate") \
	X(PLAYLOOPSOUND3D, "playloopsound3d") X(PLAYLOOPSOUND3DVP, "playloopsound3dvp") X(MODREGION, "modregion") \
	X(PLACEATPC, "placeatpc") X(PLACEATME, "placeatme") X(POSITION, "position") \
	X(HITONME, "hitonme") X(GETEFFECT, "geteffect") X(GETSPELLEFFECTS, "getspelleffects") X(GETTARGET, "gettarget") \
	X(GETDETECTED, "getdetected") X(GETLOS, "getlos") X(GETLINEOFSIGHT, "getlineofsight") X(GETATTACKED, "getattacked") \
	X(SETANGLE, "setangle") X(SETPOS, "setpos") X(MOVE, "move") X(MOVEWORLD, "moveworld") X(ROTATEWORLD, "rotateworld") \
	X(SETATSTART, "setatstart") X(GETSTARTINGANGLE, "getstartingangle") X(GETSTARTINGPOS, "getstartingpos") \
	X(ONKNOCKOUT, "onknockout") X(RESURRECT, "resurrect") X(MODFLEE, "modflee") X(SETFLEE, "setflee") \
	X(GETFLEE, "getflee") X(SETALARM, "setalarm") X(GETALARM, "getalarm") X(MODALARM, "modalarm") X(DROP, "drop") \
	X(FORCESNEAK, "forcesneak") X(CLEARFORCESNEAK, "clearforcesneak") X(HURTSTANDINGACTOR, "hurtstandingactor") \
	X(GETFORCESNEAK, "getforcesneak") X(FORCERUN, "forcerun") X(CLEARFORCERUN, "clearforcerun") \
	X(GETFORCERUN, "getforcerun") X(FORCEJUMP, "forcejump") X(CLEARFORCEJUMP, "clearforcejump") \
	X(GETFORCEJUMP, "getforcejump") X(FORCEMOVEJUMP, "forcemovejump") X(CLEARFORCEMOVEJUMP, "clearforcemovejump") \
	X(GETFORCEMOVEJUMP, "getforcemovejump") X(ENABLEPLAYERLOOKING, "enableplayerlooking") \
	X(DISABLEPLAYERLOOKING, "disableplayerlooking") X(GETPLAYERCONTROLSDISABLED, "getplayercontrolsdisabled") \
	X(GETPLAYERFIGHTINGDISABLED, "getplayerfightingdisabled") X(GETPLAYERJUMPINGDISABLED, "getplayerjumpingdisabled") \
	X(GETPLAYERLOOKINGDISABLED, "getplayerlookingdisabled") X(GETPLAYERMAGICDISABLED, "getplayermagicdisabled") \
	X(GETPLAYERVIEWSWITCHDISABLED, "getplayerviewswitchdisabled") X(GETVANITYMODEDISABLED, "getvanitymodedisabled") \
	X(GOTOJAIL, "gotojail") X(RAISERANK, "raiserank") X(LOWERRANK, "lowerrank") X(HASSOULGEM, "hassoulgem") \
	X(REMOVESOULGEM, "removesoulgem") X(GETCURRENTWEATHER, "getcurrentweather") X(GETPOS_, "getposition") X(FALL, "fall")

enum ScriptFunction
{
#define X(id, name) F_##id,
	SCRIPT_FUNCTIONS(X)
#undef X
	F_COUNT
};

static const char* kFunctionNames[] = {
#define X(id, name) name,
	SCRIPT_FUNCTIONS(X)
#undef X
};

int scriptFunctionId(const std::string& name)
{
	for (int i = 1; i < F_COUNT; i++)
		if (name == kFunctionNames[i])
			return i;
	return F_UNKNOWN;
}

// ---- Execution context

struct Ctx
{
	World& w;
	ScriptHost& host;
	int inst;            // index into w.scripts, -1 when running without locals
	int self;            // owning reference, -1 for none
	float dt;
	bool returned = false;
};

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

static float eval(Ctx& c, const Node& n);

static std::string argStr(Ctx& c, const Node& call, size_t i)
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

static float argNum(Ctx& c, const Node& call, size_t i, float fallback = 0.0f)
{
	if (i >= call.kids.size())
		return fallback;
	const Node& a = call.kids[i];
	if (a.kind == N_STR)
		return (float)atof(a.a.c_str());
	return eval(c, a);
}

// The player's cell as GetPCCell and ^Cell name it: an unnamed exterior cell goes by its region's name
static std::string displayCellName(World& w)
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
static void messageBox(Ctx& c, const Node& n)
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
static const char* kAttrFuncs[8] = { "strength", "intelligence", "willpower", "agility", "speed", "endurance",
	"personality", "luck" };
static const char* kSkillFuncs[27] = { "block", "armorer", "mediumarmor", "heavyarmor", "bluntweapon", "longblade",
	"axe", "spear", "athletics", "enchant", "destruction", "alteration", "illusion", "conjuration", "mysticism",
	"restoration", "alchemy", "unarmored", "security", "sneak", "acrobatics", "lightarmor", "shortblade", "marksman",
	"mercantile", "speechcraft", "handtohand" };

// GetEffect's argument: an effect's GMST name (sEffectWaterWalking) -> its index
static int magicEffectIndex(const std::string& arg)
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

static bool statFunction(Ctx& c, const Node& n, bool onPlayer, Ref* r, int ref, float* out)
{
	World& w = c.w;
	const std::string& name = n.b;
	int op = name.compare(0, 10, "modcurrent") == 0 ? 3 : name.compare(0, 3, "get") == 0 ? 0
		: name.compare(0, 3, "set") == 0 ? 1 : name.compare(0, 3, "mod") == 0 ? 2 : -1;
	if (op < 0)
		return false;
	std::string stat = name.substr(op == 3 ? 10 : 3);
	bool player = onPlayer || !r;
	PlayerStats& s = w.stats;
	float arg = op == 0 ? 0.0f : argNum(c, n, 0);
	// ModPCCrimeLevel: the bounty never goes below nothing
	if (stat == "pccrimelevel" && op == 2)
	{
		w.bounty = std::max(0, w.bounty + (int)arg);
		w.globals["pccrimelevel"] = (float)w.bounty;
		*out = 0.0f;
		return true;
	}
	// GetLevel / SetLevel: the player's, or an actor's (the record's until a script sets it; -1 for objects)
	if (stat == "level" && op <= 1)
	{
		if (player)
		{
			if (op == 1)
				s.level = (int)arg;
			*out = (float)s.level;
			return true;
		}
		if (r->actor < 0)
		{
			*out = -1.0f;
			return true;
		}
		ActorDef& a = w.game.actors[r->actor];
		if (op == 1)
			r->level = a.level = (int)arg;      // the record too: dialogue conditions read it there
		*out = (float)a.level;
		return true;
	}
	// GetHealthGetRatio and its kin: current over maximum
	bool ratio = stat.size() > 8 && stat.compare(stat.size() - 8, 8, "getratio") == 0;
	if (ratio)
	{
		if (op != 0)
			return false;
		stat.resize(stat.size() - 8);
	}
	// Health / magicka / fatigue: Set / Mod change the maximum, ModCurrent the current value
	int dyn = stat == "health" ? 0 : stat == "magicka" ? 1 : stat == "fatigue" ? 2 : -1;
	if (dyn >= 0)
	{
		if (!player && r->actor < 0)
		{
			*out = 0.0f;
			return true;
		}
		float* cur = player ? (dyn == 0 ? &s.health : dyn == 1 ? &s.magicka : &s.fatigue)
			: (dyn == 0 ? &r->health : dyn == 1 ? &r->magicka : &r->fatigue);
		float* max = player ? (dyn == 0 ? &s.healthMax : dyn == 1 ? &s.magickaMax : &s.fatigueMax)
			: (dyn == 0 ? &r->healthMax : dyn == 1 ? &r->magickaMax : &r->fatigueMax);
		if (ratio)
		{
			*out = *max > 0.0f ? *cur / *max : 1.0f;
			return true;
		}
		if (op == 0)
		{
			*out = dyn == 1 && *cur < 0.0f ? 0.0f : *cur;       // magicka never reads below nothing
			return true;
		}
		if (op == 3)
		{
			// Up to the maximum and down to nothing; fatigue may go below, and at nothing they fall down
			float v = *cur + arg;
			if (v > *cur)
			{
				if (v <= *max)
					*cur = v;
				else if (*cur <= *max)
					*cur = *max;
			}
			else if (v > 0.0f || dyn == 2)
				*cur = v;
			else if (*cur > 0.0f)
				*cur = 0.0f;
			if (dyn == 2 && v <= 0.0f && !player && !r->dead)
				c.host.knockDownActor(ref);
		}
		else if (op == 1)
		{
			if (player && dyn == 0)
				s.healthBonus += arg - *max;
			*max = *cur = arg;
		}
		else
		{
			// The maximum moves by the amount (fatigue alone may go below nothing), and so does the current value
			float target = *max + arg;
			if (dyn != 2)
				target = fmaxf(target, 0.0f);
			if (player && dyn == 0)
				s.healthBonus += target - *max;
			*max = target;
			*cur += arg;
		}
		*out = *cur;
		if (dyn == 0 && *cur <= 0.0f && !player && r->actor >= 0 && !r->dead)
			c.host.killActor(ref);
		return true;
	}
	if (ratio || op == 3)
		return false;
	int attr = -1, skill = -1;
	for (int i = 0; i < 8; i++)
		if (stat == kAttrFuncs[i])
			attr = i;
	for (int i = 0; i < 27; i++)
		if (stat == kSkillFuncs[i])
			skill = i;
	if (attr < 0 && skill < 0)
		return false;
	if (!player)
	{
		// NPCs and creatures: the record's value plus what scripts changed (skills only for NPCs)
		if (r->actor < 0)
		{
			*out = 0.0f;
			return true;
		}
		const ActorDef& a = w.game.actors[r->actor];
		float base = (float)(attr >= 0 ? a.attributes[attr] : a.skills[skill]);
		int slot = attr >= 0 ? attr : 8 + skill;
		float value = fmaxf(0.0f, base + (r->statDelta.empty() ? 0.0f : r->statDelta[slot]));
		if (op != 0 && (attr >= 0 || !a.creature))
		{
			if (r->statDelta.empty())
				r->statDelta.assign(35, 0.0f);
			float& delta = r->statDelta[slot];
			// Set takes any value; Mod stays between 0 and 100
			if (op == 1)
				value = fmaxf(0.0f, arg);
			else
				value = fmaxf(0.0f, value + (arg > 0.0f ? fminf(arg, fmaxf(0.0f, 100.0f - value)) : arg));
			delta = value - base;
		}
		*out = value;
		return true;
	}
	int value = attr >= 0 ? s.attributes[attr] : s.skills[skill];
	if (op != 0)
	{
		// Mod stays between 0 and 100
		int delta = (int)arg;
		if (op == 1)
			delta = (int)arg - value;
		else if (delta > 0)
			delta = std::min(delta, std::max(0, 100 - value));
		else if (value + delta < 0)
			delta = -value;
		(attr >= 0 ? s.attrBonus[attr] : s.skillBonus[skill]) += delta;
		float health = s.health, magicka = s.magicka, fatigue = s.fatigue;
		w.recomputeStats();
		s.health = fminf(s.healthMax, health);
		s.magicka = fminf(s.magickaMax, magicka);
		s.fatigue = fminf(s.fatigueMax, fatigue);
		value = attr >= 0 ? s.attributes[attr] : s.skills[skill];
	}
	*out = (float)value;
	return true;
}

// The other gold coin records are gold_001 to item scripts (OpenMW)
static std::string goldId(const std::string& id)
{
	std::string l = lower(id);
	return l == "gold_005" || l == "gold_010" || l == "gold_025" || l == "gold_100" ? "gold_001" : l;
}

// A notify GMST ("%s has been added ...", "%d %s has been added ..."): the count, then the name
static std::string gmstFormat(const std::string& fmt, int count, const std::string& name)
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
static void localToWorld(const float rot[3], bool turnOnly, float v[3])
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

static float call(Ctx& c, const Node& n)
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

	switch (n.func)
	{
	case F_MENUMODE: return c.host.menuMode() ? 1.0f : 0.0f;
	case F_ONACTIVATE:
	{
		// asked once: reading it clears it (OpenMW); asking also makes the object hold its activations back
		if (r && r->script >= 0)
			w.scripts[r->script].suppress = true;
		if (r && r->script >= 0 && w.scripts[r->script].activated)
		{
			w.scripts[r->script].activated = false;
			w.scripts[r->script].buffered = true;
			return 1.0f;
		}
		return 0.0f;
	}
	// Activate: OpenMW does it only for something the player tried to activate while its script held the activation
	// back (OnActivate), and for containers and actors
	case F_ACTIVATE:
		if (r && r->script >= 0)
			w.scripts[r->script].suppress = false;
		if (r && ((r->script >= 0 && w.scripts[r->script].buffered) || r->type == "CONT" || r->actor >= 0))
			c.host.activate(ref);
		return 0.0f;
	// An object's health is the item's condition (0 for things that don't wear)
	case F_GETHEALTH:
		return onPlayer || !r ? w.stats.health : r->actor >= 0 ? r->health : r->obj ? (float)r->obj->health : 0.0f;
	case F_ONDEATH:
		if (r && r->died)
		{
			r->died = false;
			return 1.0f;
		}
		return 0.0f;
	case F_ONMURDER:
		if (r && r->murdered)
		{
			r->murdered = false;
			return 1.0f;
		}
		return 0.0f;
	// PlaceAtPC / PlaceAtMe id count distance direction (0 front, 1 back, 2 left, 3 right)
	case F_PLACEATPC: case F_PLACEATME:
	{
		bool atPC = n.func == F_PLACEATPC || !r;
		w.placeAt(argStr(c, n, 0), (int)argNum(c, n, 1, 1), argNum(c, n, 2, 0), (int)argNum(c, n, 3, 0),
			atPC ? w.player.feet : r->pos, atPC ? w.player.yaw : r->rot[2]);
		return 0.0f;
	}
	// Which weapon hit this object last (asked once: the flag clears)
	case F_HITONME:
		if (r && !r->hitBy.empty() && r->hitBy == lower(argStr(c, n, 0)))
		{
			r->hitBy.clear();
			return 1.0f;
		}
		return 0.0f;
	// GetEffect sEffectX (or its number): whether that magic effect is running on the player or the reference
	// (whatever its magnitude; creatures and people too: their abilities and the spells on them)
	case F_GETEFFECT:
	{
		int e = magicEffectIndex(argStr(c, n, 0));
		if (e < 0)
			return 0.0f;
		if (onPlayer || !r)
		{
			for (auto& a : w.effects)
				if (a.effect == e)
					return 1.0f;
			for (auto& a : w.abilities)
				if (a.effect == e)
					return 1.0f;
			return 0.0f;
		}
		if (r->actor < 0)
			return 0.0f;
		for (auto& t : r->effects)
			if (t.effect == e)
				return 1.0f;
		for (auto& k : w.game.actors[r->actor].constEffects)
			if (k.first == e)
				return 1.0f;
		return 0.0f;
	}
	// GetSpellEffects id: a spell, potion or enchanted item's effects are running on the player (kept by name here)
	case F_GETSPELLEFFECTS:
	{
		std::string id = lower(argStr(c, n, 0));
		auto sp = w.game.spells.find(id);
		const Object* o = sp == w.game.spells.end() ? w.game.object(id) : nullptr;
		if ((sp == w.game.spells.end() && !o) || (!onPlayer && r))
			return 0.0f;
		const std::string& name = sp != w.game.spells.end() ? sp->second.name : o->name;
		for (auto& a : w.effects)
			if (a.source == name)
				return 1.0f;
		for (auto& a : w.abilities)
			if (a.source == name)
				return 1.0f;
		return 0.0f;
	}
	// The player is the one thing fought here, so "player" is the only target: fighting them, or (OpenMW) with no
	// fight on, being in the middle of talking to them
	case F_GETTARGET:
		return r && r->actor >= 0 && lower(argStr(c, n, 0)) == "player" && (r->ai == AI_COMBAT || c.host.talkingTo(ref))
			? 1.0f : 0.0f;
	case F_GETLOS: case F_GETLINEOFSIGHT: case F_GETDETECTED:
	{
		std::string t = lower(argStr(c, n, 0));
		// The player has noticed them lately (they notice by line of sight and awareness)
		if (n.func == F_GETDETECTED && t == "player")
			return r && w.time - r->lastSeen < 3.0f ? 1.0f : 0.0f;
		// Both must be actors, enabled, and loaded (OpenMW); GetDetected of another actor is line of sight
		// (their awareness check always passes unless sneaking or invisible)
		if (r && (r->actor < 0 || !r->visible() || !w.active(ref)))
			return 0.0f;
		const float* from = r ? r->pos : w.player.feet;
		const float* to = t == "player" ? w.player.feet : nullptr;
		int ti = to ? -1 : w.findRef(t);
		if (ti >= 0)
		{
			if (w.refs[ti].actor < 0 || !w.refs[ti].visible() || !w.active(ti))
				return 0.0f;
			to = w.refs[ti].pos;
		}
		if (!to)
			return 0.0f;
		float a[3] = { from[0], from[1], from[2] + 100.0f }, b[3] = { to[0], to[1], to[2] + 100.0f };
		return w.lineOfSight(a, b) ? 1.0f : 0.0f;
	}
	case F_GETATTACKED: return r && r->attacked ? 1.0f : 0.0f;
	// Placement: angles in degrees, axes "x" / "y" / "z" (moving what the level baked moves only actors and doors)
	case F_SETANGLE: case F_SETPOS: case F_MOVE: case F_MOVEWORLD: case F_ROTATEWORLD: case F_GETSTARTINGANGLE:
	case F_GETSTARTINGPOS:
	{
		std::string ax = lower(argStr(c, n, 0));
		// Any other axis does nothing (and reads 0); SetAngle also takes u / v / w (the other rotation order, the
		// same angles here)
		int k = ax == "x" ? 0 : ax == "y" ? 1 : ax == "z" ? 2 : -1;
		if (n.func == F_SETANGLE && k < 0)
			k = ax == "u" ? 0 : ax == "v" ? 1 : ax == "w" ? 2 : -1;
		if (k < 0)
			return 0.0f;
		float v = argNum(c, n, 1);
		float* pos = onPlayer || !r ? w.player.feet : r->pos;
		switch (n.func)
		{
		// the start angles kept are the turn about Z only
		case F_GETSTARTINGANGLE: return r && k == 2 ? r->start[3] * 57.29578f : 0.0f;
		case F_GETSTARTINGPOS: return r ? r->start[k] : 0.0f;
		case F_SETPOS: pos[k] = v; break;
		case F_MOVE: case F_MOVEWORLD:
		{
			float d[3] = { 0.0f, 0.0f, 0.0f };
			d[k] = v * c.dt;
			if (n.func == F_MOVE)
			{
				// Move goes along the object's own axes; a disabled object isn't moved
				if (r && !onPlayer && !r->visible())
					return 0.0f;
				bool mover = r && !onPlayer;
				float rot[3] = { 0.0f, 0.0f, w.player.yaw };
				if (mover)
					memcpy(rot, r->rot, sizeof(rot));
				localToWorld(rot, !mover || r->actor >= 0, d);
			}
			for (int i = 0; i < 3; i++)
				pos[i] += d[i];
			break;
		}
		case F_SETANGLE:
			if (onPlayer || !r)
			{
				if (k == 2) w.player.yaw = v / 57.29578f;
				else if (k == 0) w.player.pitch = fmaxf(-1.5f, fminf(1.5f, -v / 57.29578f));   // ours turns up, OpenMW's down
			}
			else
				r->rot[k] = v / 57.29578f;
			break;
		case F_ROTATEWORLD:
			// actors turn about Z only
			if (r && !onPlayer && (k == 2 || r->actor < 0))
				r->rot[k] += v / 57.29578f * c.dt;
			break;
		}
		if (r && !onPlayer)
		{
			r->moved = true;
			r->fitBox();
			if (r->actor >= 0)
				w.syncActor(ref);
		}
		return 0.0f;
	}
	case F_SETATSTART:
		if (r)
		{
			memcpy(r->pos, r->start, sizeof(r->pos));
			r->rot[2] = r->start[3];
			r->fitBox();
			r->moved = true;
			if (r->actor >= 0)
				w.syncActor(ref);
		}
		return 0.0f;
	case F_ONKNOCKOUT:
		if (r && r->knockedOut)
		{
			r->knockedOut = false;
			return 1.0f;
		}
		return 0.0f;
	case F_RESURRECT:
		if (onPlayer && w.stats.health <= 0.0f)
			w.stats.health = w.stats.healthMax;
		// A dead actor comes back as new: what scripts changed on it is gone
		if (r && r->actor >= 0 && r->dead)
		{
			r->dead = r->died = false;
			r->statDelta.clear();
			r->spells.clear();
			r->noSpells.clear();
			r->health = r->healthMax;
			r->ai = AI_IDLE;
			if (r->anim >= 0)
				w.syncActor(ref);
		}
		return 0.0f;
	case F_MODFLEE: case F_SETFLEE: case F_GETFLEE:
	{
		if (!r || r->actor < 0)
			return 0.0f;
		if (r->flee < 0)
			r->flee = w.game.actors[r->actor].flee;
		// (no clamping, as in OpenMW: SetFlee 150 reads back 150)
		if (n.func == F_MODFLEE) r->flee += (int)argNum(c, n, 0);
		if (n.func == F_SETFLEE) r->flee = (int)argNum(c, n, 0);
		return (float)r->flee;
	}
	case F_SETALARM: case F_GETALARM: case F_MODALARM:
		if (!r || r->actor < 0)
			return 0.0f;
		if (r->alarm < 0) r->alarm = w.game.actors[r->actor].alarm;
		if (n.func == F_SETALARM) r->alarm = (int)argNum(c, n, 0);
		if (n.func == F_MODALARM) r->alarm += (int)argNum(c, n, 0);
		return (float)r->alarm;
	// Drop: the item leaves the inventory and lies at their feet (freed slaves' bracers). OpenMW: the whole amount
	// is dropped, what the actor doesn't carry is made; 0 or less does nothing, and only actors drop things
	case F_DROP:
	{
		std::string id = lower(argStr(c, n, 0));
		int want = (int)argNum(c, n, 1, 1);
		if (want <= 0 || !w.game.object(id) || (!onPlayer && r && r->actor < 0))
			return 0.0f;
		if (onPlayer || !r)
		{
			int have = std::min(want, w.itemCount(id));
			if (have > 0)
				w.removeItem(id, have);
			w.dropItemAt({ id, want, false }, w.player.feet, w.player.yaw, 50.0f);
			return 0.0f;
		}
		for (size_t k = 0; k < r->contents.size(); k++)
			if (r->contents[k].second == id)
			{
				int have = std::min(want, stockCount(r->contents[k].first));
				r->contents[k].first += r->contents[k].first < 0 ? have : -have;
				if (r->contents[k].first == 0)
					r->contents.erase(r->contents.begin() + k);
				break;
			}
		w.dropItemAt({ id, want, false }, r->pos, r->rot[2], 30.0f);
		return 0.0f;
	}
	// Force flags are the referenced actor's own (the player's, with no reference): only the player's sneak acts
	case F_FORCESNEAK: case F_CLEARFORCESNEAK: case F_GETFORCESNEAK:
	case F_FORCERUN: case F_CLEARFORCERUN: case F_GETFORCERUN:
	case F_FORCEJUMP: case F_CLEARFORCEJUMP: case F_GETFORCEJUMP:
	case F_FORCEMOVEJUMP: case F_CLEARFORCEMOVEJUMP: case F_GETFORCEMOVEJUMP:
	{
		int f = n.func;
		unsigned bit = f >= F_FORCEMOVEJUMP ? 4 : f >= F_FORCEJUMP ? 2 : f >= F_FORCERUN ? 1 : 8;
		if (f == F_FORCESNEAK || f == F_CLEARFORCESNEAK || f == F_GETFORCESNEAK)
			bit = 8;
		bool get = f == F_GETFORCESNEAK || f == F_GETFORCERUN || f == F_GETFORCEJUMP || f == F_GETFORCEMOVEJUMP;
		bool clear = f == F_CLEARFORCESNEAK || f == F_CLEARFORCERUN || f == F_CLEARFORCEJUMP || f == F_CLEARFORCEMOVEJUMP;
		int key = onPlayer || !r ? -1 : ref;
		unsigned& bits = w.moveFlags[key];
		if (get)
			return (bits & bit) ? 1.0f : 0.0f;
		bits = clear ? bits & ~bit : bits | bit;
		if (key < 0 && bit == 8)
			w.forceSneak = !clear;
		return 0.0f;
	}
	// Lava and the like: hurts whoever stands on it, the player and the living actors (health per second; a
	// negative one heals). Nothing while a menu is up
	case F_HURTSTANDINGACTOR:
	{
		if (!r || !r->hasBox || c.host.menuMode())
			return 0.0f;
		// The lava's walk-on collision rises up to 64 above its flat picture (Arkngthand's in_lava_1024): standing
		// from just under the picture to that high counts as standing on it
		float rate = argNum(c, n, 0), top = r->boxMax[2];
		auto over = [&](const float* f) {
			return f[0] >= r->boxMin[0] && f[0] <= r->boxMax[0] && f[1] >= r->boxMin[1] && f[1] <= r->boxMax[1]
				&& f[2] > top - 20.0f && f[2] < top + 70.0f;
		};
		if (over(w.player.feet) && w.player.onGround)
		{
			if (rate > 0.0f)
				c.host.hurtPlayer(rate * c.dt);
			else
				w.stats.health = fminf(w.stats.healthMax, w.stats.health - rate * c.dt);
		}
		w.forLoadedActors([&](int i) {
			Ref& a = w.refs[i];
			if (i == ref || a.dead || !a.visible() || a.actor < 0 || !over(a.pos))
				return;
			a.health = fminf(a.healthMax > 0.0f ? a.healthMax : a.health, a.health - rate * c.dt);
			if (a.health <= 0.0f)
				c.host.killActor(i);
		});
		return 0.0f;
	}
	case F_GOTOJAIL: c.host.goToJail(); return 0.0f;
	// An NPC's own rank in its faction (not the player's: that does nothing). Raising stops at the top rank,
	// lowering at the bottom one. The record changes too: dialogue reads the rank there
	case F_RAISERANK: case F_LOWERRANK:
	{
		if (onPlayer || !r || r->actor < 0)
			return 0.0f;
		ActorDef& a = w.game.actors[r->actor];
		auto fit = w.game.factions.find(lower(a.faction));
		if (a.creature || a.faction.empty() || fit == w.game.factions.end())
			return 0.0f;
		int top = std::max(0, std::min(9, (int)fit->second.ranks.size() - 1));
		int next = n.func == F_RAISERANK ? std::min(a.rank + 1, top) : std::max(a.rank - 1, 0);
		r->rank = a.rank = next;
		return 0.0f;
	}
	// HasSoulGem creature: how many gems hold that creature's soul; RemoveSoulGem takes one
	case F_HASSOULGEM: case F_REMOVESOULGEM:
	{
		if (!onPlayer && r)
			return 0.0f;           // other actors' and containers' gems aren't kept with their souls
		std::string soul = lower(argStr(c, n, 0));
		int count = 0;
		for (size_t i = 0; i < w.inventory.size(); i++)
			if (w.inventory[i].soul == soul)
			{
				count += w.inventory[i].count;
				if (n.func == F_REMOVESOULGEM)
				{
					if (--w.inventory[i].count <= 0)
						w.inventory.erase(w.inventory.begin() + i);
					return 1.0f;
				}
			}
		return (float)count;
	}

	case F_GETCOMMONDISEASE: case F_GETBLIGHTDISEASE:
	{
		int type = n.func == F_GETCOMMONDISEASE ? 3 : 2;
		if (onPlayer || !r)
			return w.pcHasDisease(type) ? 1.0f : 0.0f;
		return r->actor >= 0 && w.actorHasSpellType(*r, type) ? 1.0f : 0.0f;
	}
	// Cast spell target: the spell's effects land on the target (the player or an NPC)
	case F_CAST:
	{
		auto sp = w.game.spells.find(lower(argStr(c, n, 0)));
		if (sp == w.game.spells.end())
			return 0.0f;
		// OpenMW: the player only readies the spell
		if (onPlayer)
		{
			w.stats.selectedSpell = sp->first;
			return 0.0f;
		}
		std::string target = lower(argStr(c, n, 1));
		int t = target == "player" ? -1 : w.findRef(target);
		if (t < 0 && target != "player")
			return 0.0f;
		// an actor casts it (Reflect can send it back); an object's spell has no caster to answer
		int caster = r && r->actor >= 0 ? ref : -1;
		for (auto& e : sp->second.effects)
			c.host.castEffect(t, e, sp->second.name, caster, (float)sp->second.cost);
		return 0.0f;
	}
	// Animation groups on actors: once (then back to idling) or looping (LoopGroup group count: that many
	// more times round)
	case F_PLAYGROUP: case F_LOOPGROUP:
		if (Actor* a = r && r->enabled ? w.actorOf(ref) : nullptr)
			if (actorPlay(*w.actorsOf(ref), *a, argStr(c, n, 0).c_str(), n.func == F_LOOPGROUP ? ANIM_LOOP : ANIM_ONCE)
				&& n.func == F_LOOPGROUP)
				a->loopsLeft = (s8)std::max(0, std::min(120, (int)argNum(c, n, 1)));
		return 0.0f;
	// Rotating objects are baked into the cell's batches
	// Rotate axis degrees-per-second (the object's own axis; objects scripts turn have their own mesh)
	case F_ROTATE:
	{
		std::string ax = lower(argStr(c, n, 0));
		float turn = argNum(c, n, 1) / 57.29578f * c.dt;
		if (onPlayer || !r)
			w.player.yaw += turn;       // the player turns about Z whatever the axis
		else if (ax == "x" || ax == "y" || ax == "z")
		{
			r->rot[ax == "x" ? 0 : ax == "y" ? 1 : 2] += turn;
			r->moved = true;
		}
		return 0.0f;
	}
	// ModRegion region clear cloudy foggy overcast rain thunder ash blight [snow blizzard]: its chances
	case F_MODREGION:
	{
		// a weather left out has no chance; each is 0..100
		std::vector<int> ch(10, 0);
		for (size_t k = 1; k < n.kids.size() && k <= 10; k++)
			ch[k - 1] = std::max(0, std::min(100, (int)argNum(c, n, k)));
		w.modRegion(lower(argStr(c, n, 0)), ch);
		return 0.0f;
	}
	// ChangeWeather region weather: that region's weather now, until the next weather change
	case F_CHANGEWEATHER:
		w.changeWeather(lower(argStr(c, n, 0)), (int)argNum(c, n, 1));
		return 0.0f;
	// the weather in effect: the new one only once the change to it is done
	case F_GETCURRENTWEATHER:
		return (float)w.weatherNow;
	case F_PLAYLOOPSOUND3D: case F_PLAYLOOPSOUND3DVP:
		c.host.loopSound(ref, argStr(c, n, 0), true, argNum(c, n, 1, 1.0f), argNum(c, n, 2, 1.0f));
		return 0.0f;
	case F_GETDEADCOUNT:
	{
		auto dead = w.deadCounts.find(lower(argStr(c, n, 0)));
		return dead != w.deadCounts.end() ? (float)dead->second : 0.0f;
	}
	// StartCombat target: only the player can be fought here, so another actor as the target leaves them be
	// (rather than turning them on the player)
	case F_STARTCOMBAT:
		if (r && r->actor >= 0 && !r->dead && lower(argStr(c, n, 0)) == "player")
		{
			r->aggressor = true;
			c.host.startCombat(ref);
		}
		return 0.0f;
	case F_STOPCOMBAT:
		if (r && r->actor >= 0)
			c.host.stopCombat(ref);
		return 0.0f;
	case F_SETHELLO:
		if (r && r->actor >= 0)
			r->hello = (int)argNum(c, n, 0);
		return 0.0f;
	case F_ENABLE: w.setEnabled(ref, true); return 0.0f;
	case F_DISABLE: w.setEnabled(ref, false); return 0.0f;
	case F_GETDISABLED: return r && !r->enabled ? 1.0f : 0.0f;
	case F_GETDISTANCE:
	{
		if (!r)
			return 100000.0f;
		std::string to = lower(argStr(c, n, 0));
		if (to == "player")
			return w.distanceToPlayer(ref);
		int o = w.findRef(to);
		if (o < 0)
			return 100000.0f;
		if (!w.sameSpace(o, ref))
			return 100000.0f;
		float d[3] = { r->pos[0] - w.refs[o].pos[0], r->pos[1] - w.refs[o].pos[1], r->pos[2] - w.refs[o].pos[2] };
		return sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	}
	case F_SAY: c.host.say(ref, argStr(c, n, 0), argStr(c, n, 1)); return 0.0f;
	case F_SAYDONE: return c.host.sayDone(ref) ? 1.0f : 0.0f;
	case F_DISABLEPLAYERCONTROLS: w.controlsEnabled = false; return 0.0f;
	case F_ENABLEPLAYERCONTROLS: w.controlsEnabled = true; return 0.0f;
	case F_DISABLEPLAYERJUMPING: w.jumpingEnabled = false; return 0.0f;
	case F_ENABLEPLAYERJUMPING: w.jumpingEnabled = true; return 0.0f;
	case F_ENABLEPLAYERFIGHTING: w.fightingEnabled = true; return 0.0f;
	case F_DISABLEPLAYERFIGHTING: w.fightingEnabled = false; return 0.0f;
	case F_ENABLEPLAYERMAGIC: w.magicEnabled = true; return 0.0f;
	case F_DISABLEPLAYERMAGIC: w.magicEnabled = false; return 0.0f;
	// Looking, view switching and vanity mode: kept so the Get...Disabled questions answer (no third person here)
	case F_ENABLEPLAYERLOOKING: w.controlsOff &= ~1u; return 0.0f;
	case F_DISABLEPLAYERLOOKING: w.controlsOff |= 1u; return 0.0f;
	case F_ENABLEPLAYERVIEWSWITCH: w.controlsOff &= ~2u; return 0.0f;
	case F_DISABLEPLAYERVIEWSWITCH: w.controlsOff |= 2u; return 0.0f;
	case F_ENABLEVANITYMODE: w.controlsOff &= ~4u; return 0.0f;
	case F_DISABLEVANITYMODE: w.controlsOff |= 4u; return 0.0f;
	case F_GETPLAYERCONTROLSDISABLED: return w.controlsEnabled ? 0.0f : 1.0f;
	case F_GETPLAYERFIGHTINGDISABLED: return w.fightingEnabled ? 0.0f : 1.0f;
	case F_GETPLAYERJUMPINGDISABLED: return w.jumpingEnabled ? 0.0f : 1.0f;
	case F_GETPLAYERLOOKINGDISABLED: return w.controlsOff & 1u ? 1.0f : 0.0f;
	case F_GETPLAYERMAGICDISABLED: return w.magicEnabled ? 0.0f : 1.0f;
	case F_GETPLAYERVIEWSWITCHDISABLED: return w.controlsOff & 2u ? 1.0f : 0.0f;
	case F_GETVANITYMODEDISABLED: return w.controlsOff & 4u ? 1.0f : 0.0f;
	case F_ENABLECLASSMENU: c.host.openMenu(SMENU_CLASS); return 0.0f;
	case F_ENABLEBIRTHMENU: c.host.openMenu(SMENU_BIRTH); return 0.0f;
	case F_ENABLESTATREVIEWMENU: c.host.openMenu(SMENU_REVIEW); return 0.0f;
	case F_ENABLERACEMENU: c.host.openMenu(SMENU_RACE); return 0.0f;
	case F_ENABLENAMEMENU: c.host.openMenu(SMENU_NAME); return 0.0f;
	case F_ENABLESTATSMENU: w.menusEnabled |= MENU_STATS; return 0.0f;
	case F_ENABLEINVENTORYMENU: w.menusEnabled |= MENU_INVENTORY; return 0.0f;
	case F_ENABLEMAGICMENU: w.menusEnabled |= MENU_MAGIC; return 0.0f;
	case F_ENABLEMAPMENU: w.menusEnabled |= MENU_MAP; return 0.0f;
	case F_ENABLEREST: w.menusEnabled |= MENU_REST; return 0.0f;
	// From a bed's own script: sleeping is allowed there whatever the cell says
	case F_SHOWRESTMENU:
		// (OpenMW's sleepInBed: a werewolf, enemies about, an owned bed whose trespass someone reported)
		if (c.self >= 0 && c.host.bedRefused(c.self))
			return 0.0f;
		c.host.openMenu(c.self >= 0 ? SMENU_REST_BED : SMENU_REST);
		return 0.0f;
	case F_GETPCSLEEP: return w.pcSleeping ? 1.0f : 0.0f;
	case F_WAKEUPPC:
		// awake at once (GetPCSleep 0): the Sleepers script gives one dream a night, returning after it
		w.wakeUp = true;
		w.pcSleeping = false;
		return 0.0f;
	// StartScript name: global, with the reference it is called on (or the one the calling script runs on) as its
	// own. StopScript only stops global scripts (a local one can't stop itself)
	case F_STARTSCRIPT: w.startGlobalScript(argStr(c, n, 0), onPlayer ? -1 : ref); return 0.0f;
	case F_STOPSCRIPT: w.stopGlobalScript(argStr(c, n, 0)); return 0.0f;
	case F_SCRIPTRUNNING:
	{
		std::string name = lower(argStr(c, n, 0));
		for (auto& s : w.scripts)
			if (s.running && s.ref < 0 && s.item.empty() && lower(s.script->name) == name)
				return 1.0f;
		return 0.0f;
	}
	case F_MESSAGEBOX: messageBox(c, n); return 0.0f;
	case F_GETBUTTONPRESSED: return (float)c.host.takeButtonPressed();
	case F_XBOX: return 1.0f;           // console wording: "Press A to ..."
	case F_GETSECONDSPASSED: return c.dt;
	// Items, as OpenMW's containerextensions.cpp: on the player, or on the container / actor named or running
	// the script; the gold coins of other sizes are gold_001; the messages (the game's own) only in conversation
	case F_GETITEMCOUNT:
	{
		std::string id = goldId(argStr(c, n, 0));
		if (onPlayer)
			return (float)w.itemCount(id);
		return r && (r->type == "CONT" || r->actor >= 0) ? (float)w.refItemCount(*r, id) : 0.0f;
	}
	case F_ADDITEM:
	case F_REMOVEITEM:
	{
		std::string id = goldId(argStr(c, n, 0));
		int count = (int)argNum(c, n, 1, 1.0f);
		if (count < 0)
			count = (u16)count;
		const Object* o = w.game.object(id);
		bool leveled = !o && w.game.leveled.count(id);
		if ((!o && !leveled) || count == 0 || (!onPlayer && (!r || (r->type != "CONT" && r->actor < 0))))
			return 0.0f;
		if (n.func == F_ADDITEM)
		{
			for (int k = 0; k < (leveled ? count : 1); k++)
			{
				std::string what = leveled ? w.game.pickLeveled(id, onPlayer ? w.stats.level : 1) : id;
				if (what.empty() || what == id && leveled)
					continue;
				if (onPlayer)
					w.addItem(what, leveled ? 1 : count);
				else
					w.refAddItem(*r, what, leveled ? 1 : count);
			}
			if (onPlayer && o && c.host.talking())
				c.host.notify(gmstFormat(w.game.gmst(count == 1 ? "sNotifyMessage60" : "sNotifyMessage61"), count, o->name));
			return 0.0f;
		}
		int removed = onPlayer ? w.removeItem(id, count) : w.refRemoveItem(*r, id, count);
		if (onPlayer && o && removed > 0 && c.host.talking())
			c.host.notify(gmstFormat(w.game.gmst(removed > 1 ? "sNotifyMessage63" : "sNotifyMessage62"), removed, o->name));
		return 0.0f;
	}
	// Locked is a positive level; Unlock keeps the level as a negative one (OpenMW keeps it too), so Lock with no
	// level locks again at it, or at 100 for what never had one. A level of 0 or less given still locks (as 1).
	// Locking a door shuts it at once (not a teleport door)
	case F_UNLOCK: if (r && r->lockLevel > 0) r->lockLevel = -r->lockLevel; return 0.0f;
	case F_LOCK:
	{
		if (!r)
			return 0.0f;
		int level = r->lockLevel < 0 ? -r->lockLevel : r->lockLevel;
		if (n.kids.size() > 0)
			level = std::max(1, (int)argNum(c, n, 0));
		r->lockLevel = level ? level : 100;
		if (r->type == "DOOR" && !r->hasDest)
			r->doorAngle = r->doorTarget = 0.0f;
		return 0.0f;
	}
	case F_GETLOCKED: return r && r->lockLevel > 0 ? 1.0f : 0.0f;
	case F_PLAYSOUND: case F_PLAYSOUNDVP:
		c.host.playSound(-1, argStr(c, n, 0), argNum(c, n, 1, 1.0f), argNum(c, n, 2, 1.0f));
		return 0.0f;
	case F_PLAYSOUND3D: case F_PLAYSOUND3DVP:
		c.host.playSound(ref, argStr(c, n, 0), argNum(c, n, 1, 1.0f), argNum(c, n, 2, 1.0f), true);
		return 0.0f;
	case F_GETSTANDINGPC:
	{
		if (!r || !r->hasBox)
			return 0.0f;
		const float* f = w.player.feet;
		bool over = f[0] >= r->boxMin[0] && f[0] <= r->boxMax[0] && f[1] >= r->boxMin[1] && f[1] <= r->boxMax[1];
		return over && fabsf(f[2] - r->boxMax[2]) < 30.0f && w.player.onGround ? 1.0f : 0.0f;
	}
	// Whose race: the player's, or an NPC's (creatures have none)
	case F_GETRACE:
	{
		std::string race = lower(argStr(c, n, 0));
		if (onPlayer || !r)
			return race == lower(w.stats.race) ? 1.0f : 0.0f;
		const ActorDef* a = r->actor >= 0 ? &w.game.actors[r->actor] : nullptr;
		return a && !a->creature && race == lower(a->race) ? 1.0f : 0.0f;
	}
	case F_JOURNAL:
	{
		std::string quest = argStr(c, n, 0);
		int idx = (int)argNum(c, n, 1);
		// Journal writes the entry but never lowers the quest's index (SetJournalIndex does): Gilvas Barelo's
		// "Journal A2_4_MiloGone 50 / ... 49" leaves it at 50 with both entries. The message comes only with
		// an entry that has text (or a stage already written that raises the index)
		if (w.journalAdd(quest, idx))
			c.host.notify(w.game.gmst("sjournalentry", "Your journal has been updated."));
		return 0.0f;
	}
	case F_GETJOURNALINDEX: return (float)w.getJournal(argStr(c, n, 0));
	case F_SETJOURNALINDEX: w.journalIndex[lower(argStr(c, n, 0))] = (int)argNum(c, n, 1); return 0.0f;
	case F_ADDTOPIC: w.knownTopics.insert(lower(argStr(c, n, 0))); return 0.0f;
	case F_GOODBYE: c.host.dialogueGoodbye(); return 0.0f;
	case F_CHOICE:
	{
		std::vector<std::pair<std::string, int>> choices;
		// a question with no number after it answers 1
		for (size_t i = 0; i < n.kids.size(); i += 2)
			choices.emplace_back(argStr(c, n, i), i + 1 < n.kids.size() ? (int)argNum(c, n, i + 1) : 1);
		c.host.dialogueChoice(choices);
		return 0.0f;
	}
	case F_FORCEGREETING: if (ref >= 0) c.host.forceGreeting(ref); return 0.0f;
	// Disposition belongs to NPCs: creatures have none (reads 0, changes are ignored)
	case F_MODDISPOSITION: case F_SETDISPOSITION: case F_GETDISPOSITION:
	{
		if (!r || r->actor < 0 || w.game.actors[r->actor].creature)
			return 0.0f;
		if (n.func == F_MODDISPOSITION)
			r->disposition += (int)argNum(c, n, 0);
		else if (n.func == F_SETDISPOSITION)
			r->disposition = (int)argNum(c, n, 0);
		else
			return (float)w.disposition(ref);
		return 0.0f;
	}
	case F_RANDOM: return (float)(rand() % (int)fmaxf(1.0f, argNum(c, n, 0, 100.0f)));
	case F_GETPCCELL: return w.current >= 0 && lower(displayCellName(w)).rfind(lower(argStr(c, n, 0)), 0) == 0 ? 1.0f : 0.0f;
	case F_GETPOS:
	{
		std::string axis = lower(argStr(c, n, 0));
		const float* p = onPlayer ? w.player.feet : (r ? r->pos : nullptr);
		if (!p) return 0.0f;
		return axis == "x" ? p[0] : axis == "y" ? p[1] : axis == "z" ? p[2] : 0.0f;
	}
	case F_GETANGLE:
	{
		std::string axis = lower(argStr(c, n, 0));
		if (onPlayer)
			return axis == "z" ? w.player.yaw * 57.29578f : axis == "x" ? -w.player.pitch * 57.29578f : 0.0f;
		if (!r) return 0.0f;
		return axis == "x" ? r->rot[0] * 57.29578f : axis == "y" ? r->rot[1] * 57.29578f
			: axis == "z" ? r->rot[2] * 57.29578f : 0.0f;
	}
	case F_HASITEMEQUIPPED:
	{
		std::string id = lower(argStr(c, n, 0));
		if (!onPlayer && r)
		{
			// an NPC wears what it carries of armor, clothes and weapons
			const Object* o = r->actor >= 0 ? w.game.object(id) : nullptr;
			return o && (o->type == "ARMO" || o->type == "CLOT" || o->type == "WEAP") && w.refItemCount(*r, id) > 0
				? 1.0f : 0.0f;
		}
		for (auto& it : w.inventory)
			if (it.id == id && it.equipped)
				return 1.0f;
		return 0.0f;
	}
	case F_GETCURRENTTIME: return fmodf(w.gameHour, 24.0f);
	case F_ADDSPELL:
		if (!onPlayer && r && r->actor >= 0)
		{
			std::string id = lower(argStr(c, n, 0));
			r->noSpells.erase(std::remove(r->noSpells.begin(), r->noSpells.end(), id), r->noSpells.end());
			if (!w.actorHasSpell(*r, id))
				r->spells.push_back(id);
		}
		if (onPlayer)
		{
			std::string id = lower(argStr(c, n, 0));
			bool have = false;
			for (auto& s : w.stats.spells)
				have |= s == id;
			if (!have)
			{
				w.stats.spells.push_back(id);
				float hp = w.stats.health, mp = w.stats.magicka, fp = w.stats.fatigue;
				w.recomputeStats();
				w.stats.health = fminf(w.stats.healthMax, hp);
				w.stats.magicka = fminf(w.stats.magickaMax, mp);
				w.stats.fatigue = fminf(w.stats.fatigueMax, fp);
			}
		}
		return 0.0f;
	case F_REMOVESPELL:
		if (!onPlayer && r && r->actor >= 0)
		{
			std::string id = lower(argStr(c, n, 0));
			bool had = w.actorHasSpell(*r, id);
			r->spells.erase(std::remove(r->spells.begin(), r->spells.end(), id), r->spells.end());
			if (had && w.actorHasSpell(*r, id))
				r->noSpells.push_back(id);          // theirs from the record: marked as taken away
		}
		if (onPlayer)
		{
			std::string id = lower(argStr(c, n, 0));
			for (size_t i = 0; i < w.stats.spells.size(); i++)
				if (w.stats.spells[i] == id)
					w.stats.spells.erase(w.stats.spells.begin() + i--);
			float hp = w.stats.health, mp = w.stats.magicka, fp = w.stats.fatigue;
			w.recomputeStats();
			w.stats.health = fminf(w.stats.healthMax, hp);
			w.stats.magicka = fminf(w.stats.magickaMax, mp);
			w.stats.fatigue = fminf(w.stats.fatigueMax, fp);
		}
		return 0.0f;
	case F_GETSPELL:
	{
		std::string id = lower(argStr(c, n, 0));
		if (!onPlayer && r)
			return r->actor >= 0 && w.actorHasSpell(*r, id) ? 1.0f : 0.0f;
		for (auto& s : w.stats.spells)
			if (s == id)
				return 1.0f;
		return 0.0f;
	}
	case F_GETPCCRIMELEVEL: return (float)w.bounty;
	case F_SETPCCRIMELEVEL:
		w.bounty = (int)argNum(c, n, 0);
		w.globals["pccrimelevel"] = (float)w.bounty;
		return 0.0f;
	// PayFine: the bounty goes, the stolen goods are taken back and the weapon is put away; PayFineThief only clears
	// the bounty
	case F_PAYFINE: case F_PAYFINETHIEF:
		w.bounty = 0;
		w.globals["pccrimelevel"] = 0.0f;
		if (n.func == F_PAYFINE)
		{
			w.confiscateStolen();
			c.host.sheathe();
		}
		return 0.0f;
	// Factions: the faction argument, or the faction of whoever is speaking / running the script
	case F_PCJOINFACTION: case F_PCRAISERANK: case F_PCLOWERRANK: case F_GETPCRANK: case F_PCEXPELLED:
	case F_MODPCFACREP: case F_GETPCFACREP: case F_SETPCFACREP: case F_PCEXPELL: case F_PCCLEAREXPELLED:
	{
		bool repFunc = n.func == F_MODPCFACREP || n.func == F_SETPCFACREP;
		std::string f = lower(argStr(c, n, repFunc ? 1 : 0));
		if (f.empty() || (f[0] >= '0' && f[0] <= '9') || f[0] == '-')
		{
			const Ref* speaker = r ? r : (c.self >= 0 ? &w.refs[c.self] : nullptr);
			f = speaker && speaker->actor >= 0 ? lower(w.game.actors[speaker->actor].faction) : "";
		}
		if (f.empty())
			return n.func == F_GETPCRANK ? -1.0f : 0.0f;
		int rank = w.pcRankIn(f);
		auto fit = w.game.factions.find(f);
		int top = fit != w.game.factions.end() ? (int)fit->second.ranks.size() - 1 : 9;
		switch (n.func)
		{
		case F_PCJOINFACTION:
			if (rank < 0)
				w.pcRank[f] = 0;
			logf("faction: joined %s", f.c_str());
			return 0.0f;
		case F_PCRAISERANK:
			w.pcRank[f] = rank < 0 ? 0 : std::min(rank + 1, std::max(top, 0));
			logf("faction: %s rank %d", f.c_str(), w.pcRank[f]);
			return 0.0f;
		case F_PCLOWERRANK:
			// Below the lowest rank they leave the faction (and are no longer expelled from it)
			if (rank > 0)
				w.pcRank[f] = rank - 1;
			else if (rank == 0)
			{
				w.pcRank.erase(f);
				w.pcExpelled.erase(f);
			}
			return 0.0f;
		case F_GETPCRANK: return (float)rank;
		case F_PCEXPELLED: return w.pcExpelled.count(f) ? 1.0f : 0.0f;
		case F_PCEXPELL:
			if (w.pcExpelled.insert(f).second)      // told once, the first time
				c.host.notify(w.game.gmst("sexpelledmessage", "You have been expelled from the ") + w.game.factionName(f));
			return 0.0f;
		case F_PCCLEAREXPELLED: w.pcExpelled.erase(f); return 0.0f;
		case F_MODPCFACREP: w.pcFacRep[f] += (int)argNum(c, n, 0); return 0.0f;
		case F_SETPCFACREP: w.pcFacRep[f] = (int)argNum(c, n, 0); return 0.0f;
		case F_GETPCFACREP: return (float)w.pcFacRep[f];
		}
		return 0.0f;
	}
	// An actor's reputation is its record's (r->reputation is the script's change on top of it)
	case F_MODREPUTATION:
		if (onPlayer || !r)
			w.pcReputation += (int)argNum(c, n, 0);
		else
			r->reputation += (int)argNum(c, n, 0);
		return 0.0f;
	case F_SETREPUTATION:
		if (onPlayer || !r)
			w.pcReputation = (int)argNum(c, n, 0);
		else
			r->reputation = (int)argNum(c, n, 0) - (r->actor >= 0 ? w.game.actors[r->actor].reputation : 0);
		return 0.0f;
	case F_GETREPUTATION:
		if (onPlayer || !r)
			return (float)w.pcReputation;
		return (float)(r->reputation + (r->actor >= 0 ? w.game.actors[r->actor].reputation : 0));
	case F_MODFIGHT: case F_SETFIGHT: case F_GETFIGHT:
	{
		if (!r || r->actor < 0)
			return 0.0f;
		int& fight = r->fight;
		if (fight < 0)
			fight = w.game.actors[r->actor].fight;
		// (no clamping, as in OpenMW)
		if (n.func == F_MODFIGHT)
			fight += (int)argNum(c, n, 0);
		else if (n.func == F_SETFIGHT)
			fight = (int)argNum(c, n, 0);
		return (float)fight;
	}
	// AI packages. Arguments as Morrowind has them: AIWander range duration time [idles],
	// AITravel x y z, AIFollow / AIEscort target duration x y z, the -Cell forms with a cell name
	// after the target, AIActivate target
	case F_AIWANDER: case F_AITRAVEL: case F_AIFOLLOW: case F_AIFOLLOWCELL: case F_AIESCORT: case F_AIESCORTCELL:
	case F_AIACTIVATE:
	{
		if (!r || r->actor < 0)
			return 0.0f;
		// AIEscortCell with no such cell is ignored (OpenMW), the actor keeps what it was doing
		if (n.func == F_AIESCORTCELL && w.cellIndex(argStr(c, n, 1)) < 0)
			return 0.0f;
		// A package of its own ends a fight (the new one is stacked on a cleared combat)
		if (r->ai == AI_COMBAT)
			c.host.stopCombat(ref);
		r->aiDone = false;
		r->aiRepeat = false;
		r->aiActive = true;
		r->aiStart = w.gameHour;
		r->aiDuration = 0.0f;
		r->aiTarget.clear();
		r->aiCell.clear();
		r->aiDest[0] = r->aiDest[1] = r->aiDest[2] = 0.0f;
		r->path.clear();
		r->wandering = false;
		switch (n.func)
		{
		case F_AIWANDER:
			r->aiPackage = AIPKG_WANDER;
			r->aiRange = (int)argNum(c, n, 0);
			r->aiDuration = (float)(int)argNum(c, n, 1);
			// after the time of day come the idle chances (the first unused): any of them, or a last argument, repeats
			if (n.kids.size() > 4)
				r->aiRepeat = n.kids.size() > 12 ? argNum(c, n, 12) != 0.0f : true;
			break;
		case F_AITRAVEL:
			r->aiPackage = AIPKG_TRAVEL;
			for (int k = 0; k < 3; k++)
				r->aiDest[k] = argNum(c, n, k);
			r->aiRepeat = n.kids.size() > 3;
			break;
		case F_AIACTIVATE:
			r->aiPackage = AIPKG_ACTIVATE;
			r->aiTarget = lower(argStr(c, n, 0));
			r->aiRepeat = n.kids.size() > 1;
			break;
		default:
		{
			bool cell = n.func == F_AIFOLLOWCELL || n.func == F_AIESCORTCELL;
			r->aiPackage = n.func == F_AIFOLLOW || n.func == F_AIFOLLOWCELL ? AIPKG_FOLLOW : AIPKG_ESCORT;
			r->aiTarget = lower(argStr(c, n, 0));
			if (cell)
				r->aiCell = lower(argStr(c, n, 1));
			int k = cell ? 2 : 1;
			r->aiDuration = argNum(c, n, k);
			// (an escort's hours are a whole number)
			if (r->aiPackage == AIPKG_ESCORT)
				r->aiDuration = (float)(int)r->aiDuration;
			for (int i = 0; i < 3; i++)
				r->aiDest[i] = argNum(c, n, k + 1 + i);
			r->aiRepeat = n.kids.size() > (size_t)(k + 4);
			// a follower waits to see who it follows before it starts (and counts its hours from then)
			r->aiActive = r->aiPackage != AIPKG_FOLLOW;
			break;
		}
		}
		logf("ai: %s package %d target %s", r->idLower.c_str(), r->aiPackage, r->aiTarget.c_str());
		return 0.0f;
	}
	case F_GETAIPACKAGEDONE: return r && r->aiDone ? 1.0f : 0.0f;
	// The package running now: -1 when the last one ended, or once dead and the death animation is over (OpenMW:
	// while it plays the package still counts, so 'GetCurrentAiPackage == 3 ... OnDeath' sees a follower die; the
	// animation taken as 2 seconds); an actor with a wander distance of its own has a wander package (OpenMW lists
	// it from the record)
	case F_GETCURRENTAIPACKAGE:
		if (!r || r->actor < 0 || r->aiPackage == AIPKG_IDLE)
			return -1.0f;
		if (r->dead && (r->diedAt < 0.0f || w.gameHour - r->diedAt >= 2.0f * w.timescale() / 3600.0f))
			return -1.0f;
		return r->aiPackage == AIPKG_NONE ? (w.game.actors[r->actor].wander > 0 ? (float)AIPKG_WANDER : -1.0f)
			: (float)r->aiPackage;
	// The player's cell changed since scripts last ran (a door, or outdoors a new grid cell)
	case F_CELLCHANGED: return w.current != w.scriptCell ? 1.0f : 0.0f;
	// How one faction feels about another: kept (dialogue disposition doesn't use it yet)
	case F_MODFACTIONREACTION: case F_SETFACTIONREACTION: case F_GETFACTIONREACTION:
	{
		// the map holds scripts' changes on top of the factions' own reactions
		std::string a = lower(argStr(c, n, 0)), b = lower(argStr(c, n, 1));
		int& v = w.factionReactions[a + "|" + b];
		if (n.func == F_MODFACTIONREACTION)
			v += (int)argNum(c, n, 2);
		else if (n.func == F_SETFACTIONREACTION)
			v += (int)argNum(c, n, 2) - w.factionReaction(a, b);
		return (float)w.factionReaction(a, b);
	}
	// ShowMap "Vivec": every exterior place whose name starts with that (Vivec, Vivec, Fred's House ...)
	case F_SHOWMAP:
	{
		std::string part = lower(argStr(c, n, 0));
		if (part.empty())
			return 0.0f;
		w.mapKnown.insert(part);       // (a map label named so)
		for (auto& cl : w.cells)
			if (!cl.interior && !cl.name.empty() && lower(cl.name).compare(0, part.size(), part) == 0)
			{
				w.mapKnown.insert(lower(cl.name));
				w.mapCells.insert(w.mapCellKey(cl.gx, cl.gy));
			}
		return 0.0f;
	}
	case F_FALL: return 0.0f;       // (OpenMW runs nothing for Fall)
	// PositionCell x y z zRot "cell" / Position x y z zRot: the player travels there (a loading screen
	// like a door's), anyone else moves there (drawn in that cell from now on)
	case F_GETPOS_: return 0.0f;
	case F_POSITIONCELL: case F_POSITION:
	{
		float pos[3] = { argNum(c, n, 0), argNum(c, n, 1), argNum(c, n, 2) };
		const bool toPlayer = onPlayer || !r;
		float yaw = argNum(c, n, 3) * 3.14159265f / 180.0f;
		// OpenMW copies a Morrowind.exe bug on purpose: anyone but the player has the angle turned into radians twice
		if (!toPlayer)
			yaw *= 3.14159265f / 180.0f;
		int grid = w.gridCell((int)floorf(pos[0] / 8192.0f), (int)floorf(pos[1] / 8192.0f));
		int cell;
		if (n.func == F_POSITION)
			// the player goes to the exterior cell there (from an interior too), anyone else stays in their cell
			cell = toPlayer ? (grid >= 0 ? grid : w.current) : w.placeOf(ref);
		else
		{
			std::string name = argStr(c, n, 4);
			cell = w.cellIndex(name);
			std::string ln = lower(name);
			if (cell < 0 && !toPlayer && !name.empty() && ln != "wilderness" && !w.game.regions.count(ln))
			{
				// not an interior or a named exterior: only the player is moved (to the exterior), OpenMW leaves others
				monitorOnce(("poscell:" + name).c_str(), "PositionCell into %s: not in the level", name.c_str());
				return 0.0f;
			}
			if (cell < 0 || !w.cells[cell].interior)
				cell = grid;
			if (cell < 0)
			{
				logf("script: PositionCell into %s: not in the level", name.c_str());
				monitorOnce(("poscell:" + name).c_str(), "PositionCell into %s: not in the level", name.c_str());
				return 0.0f;
			}
		}
		if (toPlayer)
			c.host.teleportPlayer(cell, pos, yaw);
		else if (w.ensureRefs(cell))
		{
			// set down on the floor there (OpenMW adjustPosition)
			if (r->actor >= 0 && w.cells[cell].live)
				w.snapToFloor(pos);
			w.relocate(ref, cell, pos, yaw);
		}
		return 0.0f;
	}
	case F_STOPSOUND: c.host.loopSound(ref, argStr(c, n, 0), false); return 0.0f;
	case F_FADEOUT: c.host.fadeTo(1.0f, argNum(c, n, 0, 1)); return 0.0f;
	case F_FADEIN: c.host.fadeTo(0.0f, argNum(c, n, 0, 1)); return 0.0f;
	case F_CLEARINFOACTOR:
		// (OpenMW) takes the actor's last answer to the topic out of the journal's Topics index
		if (r && w.currentInfo >= 0)
			w.topicLogRemoveLast(w.currentTopic, r->actor >= 0 ? w.game.actors[r->actor].name : r->id);
		return 0.0f;
	// Equip item: on the player it is used like from the inventory (added first when missing); an NPC wears
	// what it carries, so it is given the item when it has none
	case F_EQUIP:
		if (onPlayer || !r)
			c.host.equipPlayerItem(lower(argStr(c, n, 0)));
		else if (r->actor >= 0)
		{
			std::string id = lower(argStr(c, n, 0));
			if (w.game.object(id) && w.refItemCount(*r, id) == 0)
				w.refAddItem(*r, id, 1);
		}
		return 0.0f;
	case F_STREAMMUSIC:
		c.host.streamMusic(argStr(c, n, 0));
		return 0.0f;
	case F_PLAYBINK:
		if (!c.host.playMovie(argStr(c, n, 0)) && lower(argStr(c, n, 0)).find("end") != std::string::npos)
			c.host.notify("Dagoth Ur is defeated, the Heart of Lorkhan severed. The Blight lifts from Vvardenfell.");
		return 0.0f;
	case F_ENABLETELEPORTING: case F_DISABLETELEPORTING:
		w.teleportDisabled = n.func == F_DISABLETELEPORTING;
		return 0.0f;
	case F_DONTSAVEOBJECT:
		return 0.0f;
	case F_GETSOUNDPLAYING:
		return c.host.soundPlaying(ref, argStr(c, n, 0)) ? 1.0f : 0.0f;
	default:
	{
		float v;
		if (statFunction(c, n, onPlayer, r, ref, &v))
			return v;
		if (s_reportedUnknown.insert(n.b).second)
			logf("script: unsupported function %s", n.b.c_str());
		monitorOnce(("fn:" + n.b).c_str(), "script uses unsupported function %s", n.b.c_str());
		return 0.0f;
	}
	}
}

static float eval(Ctx& c, const Node& n)
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

static void assign(Ctx& c, const Node& target, float value)
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

void scriptsRun(World& w, ScriptHost& host, float dt)
{
	// Index loop: StartScript / AddItem can append instances while we run. The local scripts run first,
	// then the global ones (OpenMW's frame)
	for (int pass = 0; pass < 2; pass++)
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
			// (a global script started on an object runs with it as its reference)
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
