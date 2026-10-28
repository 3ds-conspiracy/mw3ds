// Test driver: see source/testdrive/testdrive.hpp
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
#include "testdrive/testhelpers.hpp"

bool g_testFailed = false;          // a drive or expect FAIL happened (MW3DS_FAILFAST: the run stops at the first)
// Where SNAP:moved was taken (EXPECT:moved measures from it)
float s_movedFrom[2] = { 0.0f, 0.0f };

// Tokens split at every ':'; a '|' inside a part stands for a colon of the name ("Wolverine_Hall|_Mage's_Guild")
std::vector<std::string> split(const std::string& s)
{
	std::vector<std::string> out;
	size_t p = 0;
	while (true)
	{
		size_t q = s.find(':', p);
		out.push_back(s.substr(p, q == std::string::npos ? std::string::npos : q - p));
		for (auto& c : out.back())
			if (c == '|')
				c = ':';
		if (q == std::string::npos)
			return out;
		p = q + 1;
	}
}

// A cell name against a wanted start: exact, or the wanted text as a whole-word start of it ("Balmora" is not
// "Balmoral"; "Wolverine Hall" is the hall and its rooms). A wanted name ending in '$' must match exactly.
bool cellNameMatches(const std::string& cell, std::string want)
{
	std::string here = lower(cell);
	want = lower(want);
	bool exact = !want.empty() && want.back() == '$';
	if (exact)
		want.pop_back();
	if (here == want)
		return true;
	if (exact || here.compare(0, want.size(), want) != 0)
		return false;
	char next = here[want.size()];
	return !isalnum((unsigned char)next) || !isalnum((unsigned char)want.back());
}

// ---- mechanics tests: names, numbers, snapshots

#include <map>
#include <malloc.h>

// How far a player has to walk to a door: the grid route's length (squared, to compare like a 3D distance squared).
// A door the grid can't lead to (another floor, behind a wall, hidden under a stair) ranks after every reachable one,
// nearest in 3D among those.
float doorWalkRank(Session& s, const Ref& r, float d3)
{
	std::vector<int> route;
	if (s.w.findPath(s.w.player.feet, r.pos, route) && !route.empty())
	{
		const float* q = s.w.pathPoints[route.back()].pos;
		float ex = q[0] - r.pos[0], ey = q[1] - r.pos[1], ez = q[2] - r.pos[2];
		float len = sqrtf(ex * ex + ey * ey + ez * ez);
		if (len < 400.0f)
		{
			const float* a = s.w.player.feet;
			for (int k : route)
			{
				const float* b = s.w.pathPoints[k].pos;
				float sx = b[0] - a[0], sy = b[1] - a[1], sz = b[2] - a[2];
				len += sqrtf(sx * sx + sy * sy + sz * sz);
				a = b;
			}
			return len * len;
		}
	}
	return d3 + 1e12f;
}

// Numbers the checks compare. Kinds with an argument (a[1]) and those without; false: not a number kind
bool hasArg(const std::string& what)
{
	return what == "mapnotehas" || what == "vmbone" || what == "combatspell" || what == "silveron" || what == "labelis" || what == "refmeleereach" || what == "playerdist" || what == "levcandidates" || what == "levpick" || what == "levrolls" || what == "soulcapacity" || what == "stackcount"
		|| what == "refexists" || what == "refenabled" || what == "refstock" || what == "merchantgold" || what == "followset"
		|| what == "journalentries" || what == "questfinished" || what == "skill" || what == "skillprog" || what == "attr" || what == "attrups" || what == "attrdamage" || what == "effect"
		|| what == "refhealth" || what == "refrank" || what == "reflevel" || what == "refspell" || what == "reffatigue" || what == "refmagicka" || what == "refdisp" || what == "refflee"
		|| what == "refattr" || what == "refskill" || what == "refhealthmax" || what == "refmagickamax" || what == "refcommanded"
		|| what == "effectarg" || what == "locklevel" || what == "summoned" || what == "itemhealth" || what == "spellmagicka"
		|| what == "refcell" || what == "refforcesneak" || what == "reflegframes" || what == "refsidemax" || what == "refpkg" || what == "refpkgdone" || what == "refvfx" || what == "refvfxhands" || what == "refvfxhandz" || what == "reflhandz" || what == "refcombat" || what == "refduel" || what == "refrun" || what == "refdying" || what == "reflying" || what == "refknock" || what == "refsidesteps" || what == "refskate" || what == "refyaw" || what == "refidlevoice" || what == "refheadyaw" || what == "refheadpitch" || what == "refsoultrap" || what == "refally" || what == "refdist" || what == "refsight" || what == "refaimed" || what == "refabove" || what == "refhiddenparts" || what == "refweaponrating" || what == "reftorch" || what == "reftorchfire" || what == "refcpuskin" || what == "refblendskin" || what == "refmeshes" || what == "refdressed" || what == "refz" || what == "refclimb" || what == "refwerewolf" || what == "reftorcharm" || what == "refshield" || what == "refglow" || what == "refalpha" || what == "reftint" || what == "refsize" || what == "reflampnow" || what == "refheldlight" || what == "reftalk" || what == "refposes" || what == "refmorpht" || what == "refgroupstarts" || what == "refposehz" || what == "refanimtime" || what == "refabovefeet" || what == "refadditive" || what == "refwater" || what == "refaimable" || what == "refbox" || what == "refitem" || what == "refgap" || what == "reflock" || what == "reftrap" || what == "scripttarget" || what == "reflocal" || what == "reflocalfrac" || what == "scriptlocal" || what == "itemscriptrunning" || what == "refloopvol" || what == "soundstarted" || what == "soundvolume" || what == "voicesecs" || what == "spellcost" || what == "castchance" || what == "brewedmag" || what == "brewedduration" || what == "charge"
		|| what == "count" || what == "repairamount" || what == "rechargegain" || what == "lockchance" || what == "trapchance"
		|| what == "persuadechance" || what == "persuadepart" || what == "enchantcastcost" || what == "resistbase"
		|| what == "crimebounty" || what == "skillneed" || what == "attackterm" || what == "knockodds" || what == "falldamage" || what == "poseevery" || what == "skinevery"
		|| what == "blockchance" || what == "elemshield" || what == "runspeedfor" || what == "jumpspeedfor" || what == "npcwalk"
		|| what == "npcrun" || what == "refeffect" || what == "fleerating" || what == "fightterm" || what == "hagglechance" || what == "barterprice" || what == "trainprice" || what == "travelprice" || what == "mapseen"
		|| what == "wcolor" || what == "cellfog" || what == "fogrange" || what == "savevalid" || what == "saveneeds" || what == "shotluma" || what == "shotlumagain" || what == "shotspread" || what == "shotdark" || what == "shotblack" || what == "cloudtex" || what == "clouduv" || what == "glyph" || what == "glyphsame" || what == "sunside" || what == "saverow" || what == "moonx" || what == "moonz" || what == "moonalpha" || what == "texlow" || what == "sharedmodels" || what == "texside" || what == "texlod" || what == "regionchance" || what == "regionweather"
		|| what == "equipped" || what == "canequip" || what == "armorparts" || what == "pickpocketchance" || what == "tooluses" || what == "lighttime" || what == "gyrodps" || what == "gyroyaw" || what == "gyropitch" || what == "gyrosoft" || what == "gyrodrift" || what == "gyrocal" || what == "gyrolevel" || what == "stick" || what == "vmpiece" || what == "vfxpiece" || what == "vfxfxsprites" || what == "vfxdark" || what == "vfxlife" || what == "refsprites" || what == "gamma" || what == "setting" || what == "savedsetting" || what == "conhas" || what == "mapknown" || what == "precip"
		|| what == "refscale" || what == "refdrawscale" || what == "refradius" || what == "stormdir" || what == "moontint" || what == "subtitlehas" || what == "wloopis";
}

// "@": the value SNAP recorded for the same check; "@1" / "@-2.5": that plus / minus so much
float wanted(const std::string& spec, const std::string& key, bool& ok)
{
	ok = true;
	if (spec.empty() || spec[0] != '@')
		return (float)atof(spec.c_str());
	auto it = s_snap.find(key);
	if (it == s_snap.end())
	{
		ok = false;
		return 0.0f;
	}
	return it->second + (spec.size() > 1 ? (float)atof(spec.c_str() + 1) : 0.0f);
}

int screenByName(const std::string& n)
{
	static const struct { const char* name; int scr; } names[] = {
		{ "none", SCR_NONE }, { "dialogue", SCR_DIALOGUE }, { "book", SCR_BOOK }, { "container", SCR_CONTAINER },
		{ "inventory", SCR_INVENTORY }, { "rest", SCR_REST }, { "barter", SCR_BARTER }, { "levelup", SCR_LEVELUP },
		{ "training", SCR_TRAINING }, { "spellmake", SCR_SPELLMAKE }, { "enchant", SCR_ENCHANT },
		{ "alchemy", SCR_ALCHEMY }, { "recharge", SCR_RECHARGE }, { "persuade", SCR_PERSUADE }, { "spells", SCR_SPELLS },
		{ "race", SCR_RACE }, { "birth", SCR_BIRTH }, { "stats", SCR_STATS }, { "saves", SCR_SAVES }, { "death", SCR_DEATH },
		{ "journal", SCR_JOURNAL }, { "gamemenu", SCR_GAMEMENU }, { "map", SCR_MAP },
		{ "classmethod", SCR_CLASS_METHOD }, { "classquiz", SCR_CLASS_QUIZ }, { "classlist", SCR_CLASS_LIST }, { "classresult", SCR_CLASS_RESULT }, { "classcreate", SCR_CLASS_CREATE }, { "review", SCR_REVIEW }, { "magic", SCR_MAGIC }, { "options", SCR_OPTIONS }, { "display", SCR_DISPLAY }, { "controls", SCR_CONTROLS }, { "look", SCR_LOOK }, { "console", SCR_CONSOLE },
	};
	for (auto& e : names)
		if (n == e.name)
			return e.scr;
	return -1;
}

// What a player has on the Magic screen: a spell they know (race and sign spells too), or "item:<id>" carried
bool TestDriver::canCast(Session& s, const std::string& selected)
{
	if (selected.compare(0, 5, "item:") == 0)
		return s.w.itemCount(lower(selected.substr(5))) > 0;
	std::vector<std::string> known = s.knownSpells();
	return std::find(known.begin(), known.end(), lower(selected)) != known.end()
		|| std::find(known.begin(), known.end(), lower(spaced(selected))) != known.end();
}

// Tokens a LEGIT run may still use: what a player does with the buttons, the touch screen and the menus, and checks
// that only look (EXPECT, SNAP, CHECK, PROBE, SHOT). Anything else sets the world up and is refused after LEGIT
// (an allow list: a new token is refused until it is added here as a player action).
bool TestDriver::playerToken(const std::string& tok)
{
	static const char* const kPlayer[] = {
		"A", "B", "X", "Y", "L", "R", "UP", "DOWN", "LEFT", "RIGHT", "SELECT", "START", "ZL", "ZR", "FRAMECAP", "GYRO", "CONTROLS", "GYROAIM", "TAP", "DRAG", "TOUCH", "STICKDOWN",
		// (SEED: the dice made repeatable from there, nothing given or set: a long run seeds a section whose outcome is a
		// script's roll, so a change before it doesn't flip it)
		"STICKUP", "STICKRIGHT", "STICKLEFT", "MSG", "SHOT", "SAVE", "LOAD", "SAVESEL", "LEGIT", "SEED",
		"EXPECT", "SNAP", "CHECK", "PROBE",
		"WALKTO", "FLYTO", "HOPTO", "ESCORT", "DOORTO", "ACTIVATE", "PICKUP", "LOOT", "PUT", "KILL", "STRIKE", "FACE", "SNEAK",
		"TOPIC", "CHOICE", "CHOICEVAL", "PERSUADE", "TRAVEL", "BARTER", "BARTERSEL", "INVSEL", "BUY", "SELL", "TRAIN", "SPELLMAKE", "MAKEPICK", "MAKETARGET",
		"SHARE", "SHAREPUT", "SHARETAKE", "SHAREDONE", "MAPNOTE",
		"ENCHANTAT", "ENCHANT", "ENCHITEM", "ENCHGEM", "ENCHTYPE", "ADDEFFECT", "CONFIRM", "TYPE", "LEVELUP",
		"EQUIP", "UNEQUIP", "USE", "READ", "DROP", "QUICKSET", "CAST", "CASTAT", "CASTMADESPELL", "USEMADE", "EQUIPMADE",
		"RECHARGEMADE", "RECHARGE", "REPAIR", "REPAIRAT", "REPAIRWITH", "BUYSPELL", "BREW", "DRINKBREWED", "USELOCKPICK", "USEPROBE", "SLEEP", "SLEEPUNTIL", "SLEEPTO", "UNSEEN", "UNTIL", "FLOAT", "HIDE", "SPEEDBOOST",
	};
	size_t colon = tok.find(':');
	std::string name = tok.substr(0, colon);
	// SLEEP:h only: SLEEP:h:1 / :2 decide whether a creature comes
	if (name == "SLEEP" && colon != std::string::npos && tok.find(':', colon + 1) != std::string::npos)
		return false;
	for (const char* k : kPlayer)
		if (name == k)
			return true;
	return false;
}

void TestDriver::fail(const char* why)
{
	logf("drive: FAIL %s %s: %s", kind == KILL ? "kill" : kind == WALK ? "walk to" : kind == UNTIL ? "until" : "activate", id.c_str(), why);
	g_testFailed = true;
	// both screens as they are, for a look at what held it up (fail_<n>.bmp beside the log; native runs draw it with -Draw)
	// (taken as the next frame begins, as a SHOT step's, so the world is drawn in it)
	static int shots = 0;
	if (shots < 20)
	{
		char path[64];
		snprintf(path, sizeof(path), "sdmc:/3ds/mw3ds/fail_%02d.bmp", shots++);
		failShot = path;
	}
	kind = NONE;
}

bool TestDriver::start(Session& s, const std::string& token)
{
	if (!retrying)
	{
		viewTries = talkTries = takeTries = 0;
		thenDo.clear();
		ledAway = false;
		commandTries = 0;
	}
	std::vector<std::string> a = split(token);
	const std::string& verb = a[0];
	if (verb == "UNSEEN")
	{
		// UNSEEN[:secs]: stand until nobody has the player in view (no NPC within fAlarmRadius saw us in the last second),
		// as a player waits for the owner to look away before a crime; at most secs (60)
		unseenLimit = a.size() >= 2 ? (float)atof(a[1].c_str()) : 60.0f;
		unseenTimer = elapsed = 0.0f;
		id = "nobody watching";
		kind = UNSEEN;
		return true;
	}
	if (verb == "GOD")
	{
		// GOD:2: only kept alive; the player's blows roll, hurt and wear as usual (uber quest tests)
		s.testGod = a.size() < 2 || a[1] != "0";
		s.testGodBlows = a.size() < 2 || a[1] != "2";
		logf("test: god mode %s", !s.testGod ? "off" : s.testGodBlows ? "on" : "on (kept alive only)");
		return false;
	}
	if (verb == "HIDE" && a.size() >= 2)
	{
		// HIDE:<seconds> / HIDE:off (v2 kit): Chameleon 300 on the hide item, used again every half second, for that long
		// (v1's potions: 60) or till off; a talk turns it off first (an ACTIVATE of someone)
		bool off = lower(a[1]) == "off";
		hideUntil = off ? -1.0f : s.w.time + (float)atof(a[1].c_str());
		logf("drive: hide %s", off ? "off" : a[1].c_str());
		return false;
	}
	if (verb == "FLOAT" && a.size() >= 2)
	{
		// FLOAT:<seconds> / FLOAT:off (v2 kit): levitate on the fly item through a flight by hand (jumps, R / L, a walk over
		// water), used again every half second whatever the steps do, for that long (v1's potions: 60) or till off; the last
		// use runs out within a second
		bool off = lower(a[1]) == "off";
		floatUntil = off ? -1.0f : s.w.time + (float)atof(a[1].c_str());
		floatWalk = false;
		// (FLOAT:<seconds>:doors: kept through doors too, into a Telvanni tower, whose floors only levitation joins)
		floatCell = a.size() >= 3 && lower(a[2]) == "doors" ? -3 : floatPlace(s.w);
		logf("drive: float %s", off ? "off" : a[1].c_str());
		return false;
	}
	if (verb == "UNTIL" && a.size() >= 2)
	{
		// UNTIL:<check>: the step ends the frame the check (EXPECT's) holds: the cell changed, a talk opened, an effect
		// ended. Its seconds are the most to wait, then "drive: FAIL until"
		untilSpec = token.substr(6);
		id = untilSpec;
		elapsed = 0.0f;
		kind = UNTIL;
		return true;
	}
	if (verb == "EXPECT")
	{
		expect(s, token.substr(7));
		return false;
	}
	if (mechanicsOp(s, a))
		return false;
	// UNEQUIP:<item>: take a worn or held item off, as tapping it on the Inventory page does (a fistfight: the weapon
	// put away)
	if (verb == "UNEQUIP" && a.size() >= 2)
	{
		std::string want = lower(spaced(a[1])), want2 = lower(a[1]);
		for (size_t k = 0; k < s.w.inventory.size(); k++)
			if (s.w.inventory[k].equipped && (lower(s.w.inventory[k].id) == want || lower(s.w.inventory[k].id) == want2))
			{
				s.w.inventory[k].equipped = false;
				s.playSound(-1, "Item Misc Up");
				s.w.refreshStats();
				logf("test: unequipped %s", s.w.inventory[k].id.c_str());
				return false;
			}
		logf("drive: FAIL unequip %s: not worn or held", a[1].c_str());
		return false;
	}
	if (verb == "EQUIP" && a.size() >= 2)
	{
		std::string want = lower(spaced(a[1])), want2 = lower(a[1]);
		for (size_t k = 0; k < s.w.inventory.size(); k++)
			if (lower(s.w.inventory[k].id) == want || lower(s.w.inventory[k].id) == want2)
			{
				const Object* o = s.w.game.object(s.w.inventory[k].id);
				if (o && (o->recType == REC_ALCH || o->recType == REC_INGR))
				{
					// the inventory's Drink / Eat
					std::string item = s.w.inventory[k].id;
					s.consume(item);
					logf("test: drank / ate %s", item.c_str());
					return false;
				}
				if (!s.w.inventory[k].equipped && !s.equipItem((int)k))
				{
					logf("test: %s was refused", s.w.inventory[k].id.c_str());       // (EXPECT:message says why)
					return false;
				}
				logf("test: equipped %s", s.w.inventory[k].id.c_str());
				return false;
			}
		logf("drive: FAIL equip %s: not carried", a[1].c_str());
		return false;
	}
	if (verb == "ESCORT" && a.size() >= 2)
	{
		escort = a[1] == "-" ? "" : a[1];
		logf("drive: escort %s", escort.empty() ? "ended" : escort.c_str());
		return false;
	}
	if ((verb == "FLYTO" || verb == "HOPTO") && a.size() >= 2)
	{
		id = a[1];
		if (a[1][0] != '@' || sscanf(a[1].c_str() + 1, "%f,%f,%f", &goal[0], &goal[1], &goal[2]) != 3)
		{
			logf("drive: FAIL %s %s: wants @x,y,z", verb.c_str(), a[1].c_str());
			return false;
		}
		kind = verb == "FLYTO" ? FLY : HOP;
		flyBegun = false;
		memcpy(aim, goal, sizeof(aim));
		const Player& p = s.w.player;
		if (kind == HOP && p.levitate > 0.0f)
		{
			fail("levitating: no ground to jump from (let it run out, or fly)");
			return false;
		}
		lowFly = a.size() >= 3 && a[2] == "low";
		bool beside = a.size() >= 3 && a[2] == "beside";
		cruise = lowFly ? fmaxf(p.feet[2], goal[2]) + 120.0f : a.size() >= 3 && !beside ? (float)atof(a[2].c_str()) : fmaxf(p.feet[2], goal[2]) + 1500.0f;
		// (FLYTO:@x,y,z:beside:<cruise>: both, a long way over high buildings to a door under a roof)
		if (beside && a.size() >= 4)
			cruise = (float)atof(a[3].c_str());
		// FLYTO:@x,y,z:beside (a spot on a roof, a tower top or a ledge): land a few hundred units out, at the nearest place with
		// clear air down to the spot's height from which it can be seen, and walk the rest (a player does not hover over a roof)
		if (beside)
		{
			float spot[2];
			if (spotBeside(s, goal, cruise, spot))
			{
				logf("drive: FLYTO %s:beside: landing at %.0f %.0f, %.0f from it", id.c_str(), spot[0], spot[1], hypotf(spot[0] - goal[0], spot[1] - goal[1]));
				goal[0] = spot[0];
				goal[1] = spot[1];
			}
		}
		phase = climbs = hops = 0;
		underWaited = false;
		underWait = -1.0f;
		outTimer = 0.0f;
		outTries = 0;
		outSink = false;
		inAir = false;
		phaseTimer = elapsed = progressTimer = 0.0f;
		memcpy(progressAt, p.feet, sizeof(progressAt));
		lastGoal = 1e9f;
		// seconds the leg needs at fly speed (up, over, down), a third more for detours, and a margin
		flyAllow = ((hypotf(goal[0] - p.feet[0], goal[1] - p.feet[1]) + 2.0f * fmaxf(0.0f, cruise - p.feet[2])) / fmaxf(1.0f, p.flySpeed)) * 1.35f + 30.0f;
		logf("drive: %s %s from %.0f %.0f %.0f (Acrobatics %d, Fortify Skill %.0f, Levitate %.0f)", verb.c_str(), id.c_str(), p.feet[0],
			p.feet[1], p.feet[2], s.w.stats.skills[20], s.w.effectTotal(83), p.levitate);
		return true;
	}
	if ((verb == "WALKTO" || verb == "KILL" || verb == "ACTIVATE" || verb == "PICKUP" || verb == "DOORTO" || verb == "LOOT"
		|| verb == "PUT" || verb == "STRIKE") && a.size() >= 2)
	{
		// a talk left open (an answer like "travel together" ends with the dialogue still up): a player says goodbye before
		// walking off, and every walk would time out under it
		if (s.dlg.open && s.screen == SCR_DIALOGUE && !retrying)
		{
			// (a guard's crime greeting is answered first, as a player would: pay the fine when there is gold for it)
			int pay = -1;
			for (size_t i = 0; i < s.dlg.choices.size() && pay < 0; i++)
				if (s.dlg.choices[i].second == 1)
					pay = (int)i;
			if (s.testLegit && s.w.bounty > 0 && pay >= 0 && s.w.itemCount("gold_001") >= s.w.bounty)
			{
				logf("drive: paying the fine (%d) of the crime greeting left open before %s %s", s.w.bounty, verb.c_str(), a[1].c_str());
				dialogueChoose(s.dlg, s.w, s, s.dlg.choices[pay].second);
			}
			if (s.dlg.open && s.screen == SCR_DIALOGUE)
			{
				logf("drive: closing a talk left open before %s %s", verb.c_str(), a[1].c_str());
				s.closeScreen();
			}
		}
		// PUT:<container>:<item>: open it as LOOT does, then put the carried item in (the Inventory page's tap)
		strikeFor = verb == "STRIKE" ? (a.size() >= 3 ? (float)atof(a[2].c_str()) : 5.0f) : 0.0f;
		strikeUntil = verb == "STRIKE" && a.size() >= 4 ? lower(a[3]) : "";
		killUnseen = verb == "KILL" && a.size() >= 3 && lower(a[2]) == "unseen";
		if (leading && verb == "KILL")
		{
			// (back from leading them away: the escort as it was)
			escort = leadEscort;
			leading = false;
		}
		lootItem = (verb == "LOOT" || verb == "PUT") && a.size() >= 3 ? lower(a[2]) : "";
		lootPut = verb == "PUT";
		id = a[1];
		kind = verb == "WALKTO" ? WALK : (verb == "KILL" || verb == "STRIKE") ? KILL : ACTIVATE;
		diveOk = verb == "WALKTO" && a.size() >= 3 && a[2] == "dive";
		ownPiece = verb == "DOORTO" && a.size() >= 3 && lower(a[2]) == "near";
		pickup = verb == "PICKUP";
		if (verb != "WALKTO")
			retryToken = token;
		pointGoal = verb == "WALKTO" && a[1][0] == '@';
		if (pointGoal)
		{
			if (sscanf(a[1].c_str() + 1, "%f,%f,%f", &goal[0], &goal[1], &goal[2]) != 3)
			{
				fail("WALKTO:@x,y,z wants three numbers");
				return false;
			}
			ref = -1;
			path.clear();
			if (planPath(s, s.w.player.feet, goal, true, path))
				logf("drive: WALKTO %s, %d path points", id.c_str(), (int)path.size());
			else
				logf("drive: WALKTO %s, straight", id.c_str());
			memcpy(progressAt, s.w.player.feet, sizeof(progressAt));
			progressTimer = swingTimer = lookTimer = elapsed = repathTimer = 0.0f;
			stuckTries = 0;
			hoverOn = false;
			hoverTimer = 0.0f;
			descTimer = followWait = 0.0f;
			swimRoute.clear();
			floodTries = 0;
			deadEntry = -1;
			floodKeep = false;
			fightRef = -1;
			fightHelp = false;
			unlockTries = 0;
			unlockDoor = -1;
			earlyTried = false;
			if (!retrying)
			{
				lookUpTried = false;
				crouchTried = false;
				if (crouchSet)
					s.testSneak = false;     // (the crouch to look under something: up again)
				crouchSet = false;
			}
			lookUpTimer = 0.0f;
			escortLoose = false;
			backTries = 0;
			fightGaveUp.clear();
			backTimer = 0.0f;
			descGaveUp = false;
			lastGoal = 1e9f;
			sidestepTimer = 0.0f;
			doorTried = false;
			doorLast = -1;
			lineTimer = 0.0f;
			pressA = false;
			return true;
		}
		ref = findTarget(s, verb, a[1]);
		if (ref < 0)
		{
			// maybe not streamed in yet (a neighbouring exterior, just through a door): look again a while
			searchVerb = verb;
			searchArg = a[1];
			searchTimer = 0.0f;
			kind = SEARCH;
			logf("drive: %s %s not in the loaded cells yet, searching", verb.c_str(), a[1].c_str());
			return true;
		}
		path.clear();
		const float* t = s.w.refs[ref].pos;
		if (planPath(s, s.w.player.feet, t, false, path, ownPiece))
			logf("drive: %s %s, %d path points", verb.c_str(), id.c_str(), (int)path.size());
		else
			logf("drive: %s %s, straight", verb.c_str(), id.c_str());
		memcpy(progressAt, s.w.player.feet, sizeof(progressAt));
		progressTimer = swingTimer = lookTimer = elapsed = repathTimer = 0.0f;
		stuckTries = 0;
		hoverOn = false;
		hoverTimer = 0.0f;
		descTimer = followWait = 0.0f;
			swimRoute.clear();
			floodTries = 0;
			deadEntry = -1;
			floodKeep = false;
			fightRef = -1;
			fightHelp = false;
			unlockTries = 0;
			unlockDoor = -1;
			earlyTried = false;
			if (!retrying)
			{
				lookUpTried = false;
				crouchTried = false;
				if (crouchSet)
					s.testSneak = false;     // (the crouch to look under something: up again)
				crouchSet = false;
			}
			lookUpTimer = 0.0f;
			escortLoose = false;
			backTries = 0;
			fightGaveUp.clear();
			backTimer = 0.0f;
		descGaveUp = false;
		lastGoal = 1e9f;
		sidestepTimer = 0.0f;
		doorTried = false;
		doorLast = -1;
		lineTimer = 0.0f;
		pressA = false;
		return true;
	}
	return false;
}

bool TestDriver::update(Session& s, PlayerInput& in, u32& down, float dt)
{
	if (kind == NONE)
		return false;
	elapsed += dt;
	World& w = s.w;
	if (kind == FLY)
		return updateFly(s, in, dt);
	if (kind == HOP)
		return updateHop(s, in, dt);
	// A guard's crime greeting (pay the fine / go to jail / resist) that opens on its own during a step that is not
	// a talk with that guard: in a LEGIT run pay it if there is gold for it, close the dialogue and carry on
	if (s.testLegit && s.dlg.open && s.screen == SCR_DIALOGUE && s.dlg.ref >= 0 && s.dlg.ref != ref && s.dlg.ref < (int)w.refs.size()
		&& w.refs[s.dlg.ref].actor >= 0 && lower(w.game.actors[w.refs[s.dlg.ref].actor].cls) == "guard")
	{
		const std::string who = w.refs[s.dlg.ref].idLower;
		int pay = -1;
		for (size_t i = 0; i < s.dlg.choices.size() && pay < 0; i++)
			if (s.dlg.choices[i].second == 1)
				pay = (int)i;
		if (w.bounty > 0 && pay >= 0 && w.itemCount("gold_001") >= w.bounty)
		{
			logf("drive: %s's crime greeting during %s: paying the fine (%d)", who.c_str(), id.c_str(), w.bounty);
			dialogueChoose(s.dlg, w, s, s.dlg.choices[pay].second);
			return true;
		}
		if (s.dlg.choices.empty() && w.bounty <= 0)
		{
			logf("drive: closing %s's talk, back to %s", who.c_str(), id.c_str());
			s.closeScreen();
			return true;
		}
	}
	// v2 kit: a talk someone opened on their own during a step that is not a talk with them (a ForceGreeting: Ulyne Henim
	// once the necromancer is dead): read and closed, as a player does, then on with the step (with it open nothing moves)
	if (s.kitV2() && s.dlg.open && s.screen == SCR_DIALOGUE && s.dlg.ref >= 0 && s.dlg.ref != ref && kind != TALKWAIT
		&& kind != UNTIL && s.dlg.choices.empty() && !s.dlg.history.empty())
	{
		logf("drive: %s talked to us during %s: closing the talk, back to it", w.refs[s.dlg.ref].id.c_str(), id.c_str());
		s.closeScreen();
		return true;
	}
	if (std::optional<bool> r = updateWaits(s, in, down, dt))
		return *r;
	if (!pointGoal && (ref < 0 || !w.active(ref)))
	{
		fail("gone from the loaded cells");
		return false;
	}
	// a spot to walk to stands in for a reference there (WALKTO:@x,y,z)
	static Ref spot;
	if (pointGoal)
	{
		memcpy(spot.pos, goal, sizeof(goal));
		memcpy(spot.boxMin, goal, sizeof(goal));
		memcpy(spot.boxMax, goal, sizeof(goal));
	}
	Ref& r = pointGoal ? spot : w.refs[ref];
	Player& p = w.player;
	const bool trappedDoor = r.recType == REC_DOOR && !r.trap.empty() && !r.disarmed;
	// they came to us: a ForceGreeting (Dagoth Gares) opened their dialogue on the way
	if ((kind == ACTIVATE || kind == WALK) && s.dlg.open && s.dlg.ref == ref)
	{
		logf("drive: %s greeted us on the way", id.c_str());
		kind = NONE;
		return false;
	}
	// v2 kit: a container that opened itself as we came near (Jeanne's chest: its script activates it within 128): the
	// item is put in or taken from the open screen, as a player would
	if (s.kitV2() && kind == ACTIVATE && !lootItem.empty() && s.screen == SCR_CONTAINER && s.containerRef == ref)
	{
		logf("drive: %s opened on the way", id.c_str());
		kind = LOOTWAIT;
		takeTimer = 0.0f;
		return true;
	}
	// killing: a conversation that opened on its own (ForceGreeting: Dagoth Gares's speech) is left, as a
	// player says goodbye and fights on
	if (kind == KILL && s.dlg.open && s.screen == SCR_DIALOGUE)
	{
		logf("drive: leaving a conversation to fight %s", id.c_str());
		s.closeScreen();
		return true;
	}
	// a message box in the way (a crime noticed, a script's message): click it away, as a player would
	if (!s.messages.empty())
	{
		if (!pressA)
		{
			logf("drive: dismissing \"%.60s\"", s.messages.front().text.c_str());
			down |= KEY_A;
		}
		pressA = !pressA;                // A on one frame, released the next
		return true;
	}
	// paralysed: nothing moves, however long; said so, and not counted as being stuck
	if (w.effectTotal(45) > 0.0f)
	{
		if (paralysedLog <= 0.0f)
		{
			logf("drive: paralysed (%s %s)", kind == KILL ? "killing" : kind == WALK ? "walking to" : "going to", id.c_str());
			paralysedLog = 5.0f;
		}
		paralysedLog -= dt;
		progressTimer = 0.0f;
		memcpy(progressAt, p.feet, sizeof(progressAt));
		return true;
	}
	paralysedLog = 0.0f;
	if (kind == KILL && r.dead)
	{
		logf("drive: killed %s in %.1f s", id.c_str(), elapsed);
		kind = NONE;
		return false;
	}
	float tx = r.pos[0], ty = r.pos[1];
	// a door whose origin sits well outside its own mesh (an Ashlander yurt's: the model is placed 336 units from the
	// reference, inside the tent): walk to where the door shows, its box's middle (other doors keep their origin: a cave
	// door's box takes in the rock round it)
	if (kind == ACTIVATE && !pointGoal && r.recType == REC_DOOR && r.boxMax[0] > r.boxMin[0])
	{
		float ox = fmaxf(r.boxMin[0] - r.pos[0], fmaxf(0.0f, r.pos[0] - r.boxMax[0]));
		float oy = fmaxf(r.boxMin[1] - r.pos[1], fmaxf(0.0f, r.pos[1] - r.boxMax[1]));
		if (ox * ox + oy * oy > 100.0f * 100.0f)
		{
			tx = (r.boxMin[0] + r.boxMax[0]) * 0.5f;
			ty = (r.boxMin[1] + r.boxMax[1]) * 0.5f;
		}
	}
	float dx = tx - p.feet[0], dy = ty - p.feet[1];
	float dist = sqrtf(dx * dx + dy * dy);
	float want = kind == WALK ? (pointGoal ? 30.0f : 120.0f) : kind == KILL ? 90.0f : 100.0f;
	// a big door (a Daedric ruin's oval one): its own mesh stops us at the face, a long way from the origin; at the box is there
	if (kind == ACTIVATE && !pointGoal && r.recType == REC_DOOR && r.hasBox && dist > want && dist < 300.0f)
	{
		float bx = fmaxf(r.boxMin[0] - p.feet[0], fmaxf(0.0f, p.feet[0] - r.boxMax[0]));
		float by = fmaxf(r.boxMin[1] - p.feet[1], fmaxf(0.0f, p.feet[1] - r.boxMax[1]));
		if (bx * bx + by * by < 40.0f * 40.0f)
			dist = want;
	}
	// a big statue (Mehrunes Dagon's in Ald Sotha: a box 570 wide and 700 high, its origin 440 above the floor): its mesh stops
	// us at its foot, far from the origin. Standing at the box, on a floor within its height, is at the statue
	bool atStatue = false;
	if (kind == ACTIVATE && !pointGoal && r.recType == REC_ACTI && r.hasBox && r.boxMax[0] - r.boxMin[0] > 400.0f && dist > want)
	{
		float bx = fmaxf(r.boxMin[0] - p.feet[0], fmaxf(0.0f, p.feet[0] - r.boxMax[0]));
		float by = fmaxf(r.boxMin[1] - p.feet[1], fmaxf(0.0f, p.feet[1] - r.boxMax[1]));
		if (bx * bx + by * by < 60.0f * 60.0f && p.feet[2] >= r.boxMin[2] - 80.0f && p.feet[2] <= r.boxMax[2])
		{
			dist = want;
			atStatue = true;
		}
	}
	// on another floor (ruins, towers): not there yet, however close it looks from above. Things to
	// activate count as reached within Morrowind's activation reach (iMaxActivateDist 192) in 3D.
	float dz = r.pos[2] - p.feet[2];
	if (atStatue)
		dz = 0.0f;
	// (a planned spot outdoors: its height is the land's, give or take a rock: there by the map, within 400 up or down)
	bool otherFloor = kind == ACTIVATE ? dist * dist + dz * dz > 190.0f * 190.0f && fabsf(dz) > 160.0f
		: fabsf(dz) > (pointGoal ? 400.0f : 160.0f);
	// v2 kit: a walk to a thing (a chest on a shelf above) is there within the activation reach too, as an ACTIVATE is: the
	// lockpick and the loot after it work from there (Mebestien's chest, 176 off in 3D, 171 up)
	if (s.kitV2() && kind == WALK && !pointGoal && dist * dist + dz * dz <= 190.0f * 190.0f)
		otherFloor = false;
	// v2 kit: swimming over it (a grate under the canal's water; the land spell after the flight left us at the surface):
	// not there till we dive within reach; the stroke follows the view down to it (the walk below)
	if (s.kitV2() && kind == ACTIVATE && p.swimming && dz < -100.0f)
		otherFloor = true;
	// a hatch in the ceiling (Sorkvild's, over the hall): its origin is a floor up, its box is within the crosshair's reach
	// of the eye from underneath it
	if (otherFloor && kind == ACTIVATE && !pointGoal && r.recType == REC_DOOR && r.hasBox && dz > 0.0f && dist < 60.0f)
	{
		float ez = p.feet[2] + PLAYER_EYE_HEIGHT - p.eyeDrop;
		float gap = fmaxf(0.0f, r.boxMin[2] - ez);
		otherFloor = gap > 150.0f;
	}
	// (levitating over it on a roof that holds us up: the descent at the door gave up, so the hover below brings us off its edge)
	bool heldAbove = kind == ACTIVATE && descGaveUp && p.levitate > 0.0f && !p.onGround && dz < -60.0f;
	// v2 kit: a talk (an ACTIVATE of someone) while hidden on the hide item: off, and the last second left to run out first
	// ("Who's there?" and no services from one talked to unseen)
	if (s.kitV2() && kind == ACTIVATE && (r.recType == REC_NPC_ || r.recType == REC_CREA))
	{
		if (hideUntil >= 0.0f)
		{
			hideUntil = -1.0f;
			logf("drive: hide off to talk to %s", id.c_str());
		}
		bool hidden = false;
		for (const ActiveEffect& e : w.effects)
			hidden |= e.effect == 40 && e.source == "uber hide";
		if (hidden)
			return true;
	}
	// v2 kit: the crosshair on it already, its prompt showing ("A: Open ..."): pressed, however far the walk thinks it has
	// to go (a canton door across its walkway: the walk to its middle stuck 140 off, the prompt up all along)
	// (not a door with the escort far behind: the wait for them at the door below comes first; Dandsa was left in Abernanit)
	if (s.kitV2() && kind == ACTIVATE && !earlyTried && dist > want && s.target == ref && !s.menuOpen() && aimsAt(s, ref)
		&& !(r.hasDest && escortLagging(s, escort, 800.0f)))
	{
		down |= KEY_A;
		earlyTried = true;
		logf("drive: activated %s (crosshair, %.0f off)", id.c_str(), dist);
		memcpy(pressAt, p.feet, sizeof(pressAt));
		doorWasOpen = r.recType == REC_DOOR && r.doorTarget != 0.0f;
		// (a door: watched as a trapped one is; still here after it, the press from there did nothing: walked on)
		kind = pickup ? TAKE : !lootItem.empty() ? LOOTWAIT : (r.recType == REC_NPC_ || r.recType == REC_CREA) ? TALKWAIT : r.recType == REC_DOOR ? DOORWAIT : NONE;
		doorCell = w.current;
		takeTimer = 0.0f;
		return kind != NONE;
	}
	if (dist > want || otherFloor || heldAbove)
		return walkStep(s, in, down, dt, r, p, tx, ty, dx, dy, dist, want, dz);
	// a door with the follower far behind: wait until they are near before going through (they come through behind us)
	if (kind == ACTIVATE && r.hasDest)
	{
		int e = -1;
		if (escortLagging(s, escort, 800.0f, &e))
		{
			if ((int)(elapsed / 5.0f) != (int)((elapsed - dt) / 5.0f))
				logf("drive: waiting for %s at the door (%.0f away)", escort.c_str(), w.distanceToPlayer(e));
			progressTimer = 0.0f;
			return true;
		}
		// a follower the case did not name (the Fighters Guild's Ulyne Henim, hired by a topic): left more than 800 units behind,
		// it stays in the cell (OpenMW's getFollowers), so a player waits for it too, up to 20 s
		if (escort.empty() && followWait < 20.0f)
			for (int i : w.loadedActors)
			{
				float d = w.distanceToPlayer(i);
				if (w.followsPlayer(i) && w.refs[i].aiPackage != AIPKG_ESCORT && !w.refs[i].dead && d > 600.0f && d < 20000.0f)
				{
					if ((int)(followWait / 5.0f) != (int)((followWait - dt) / 5.0f))
						logf("drive: waiting for follower %s at the door (%.0f away)", w.refs[i].id.c_str(), d);
					followWait += dt;
					progressTimer = 0.0f;
					return true;
				}
			}
	}
	// there, and the crosshair already on it (last frame's aim): press A with the aim left as it is (moving it now
	// would put the press on whatever the new aim finds)
	if (kind == ACTIVATE && s.target == ref && !s.menuOpen() && aimsAt(s, ref))
	{
		down |= KEY_A;
		logf("drive: activated %s (crosshair)", id.c_str());
		memcpy(pressAt, p.feet, sizeof(pressAt));
		doorWasOpen = r.recType == REC_DOOR && r.doorTarget != 0.0f;
		kind = pickup ? TAKE : !lootItem.empty() ? LOOTWAIT : (r.recType == REC_NPC_ || r.recType == REC_CREA) ? TALKWAIT : trappedDoor ? DOORWAIT : NONE;
		doorCell = w.current;
		takeTimer = 0.0f;
		return kind != NONE;
	}
	// there: face it
	p.yaw = atan2f(dx, dy);
	if (kind == WALK)
	{
		logf("drive: reached %s in %.1f s", id.c_str(), elapsed);
		kind = NONE;
		if (!thenDo.empty())
		{
			std::string t = thenDo;
			thenDo.clear();
			retrying = true;
			start(s, t);
			retrying = false;
			return kind != NONE;
		}
		return false;
	}
	if (kind == KILL)
	{
		p.pitch = 0.0f;
		// v2 kit: at the kit's speed we reach it before it comes to us, often above or below the eye: the blow aimed at its
		// middle (a level one passed over a crouching rat or under one on a ledge)
		if (s.kitV2())
		{
			// (facing where it stands, not its box's middle: a sitting or crouching one's box lies off to a side, and the
			// swing goes where we face: Dothiel at Saturan, faced away from)
			float ax = r.pos[0] - p.feet[0], ay = r.pos[1] - p.feet[1];
			p.yaw = atan2f(ax, ay);
			float cz = (r.boxMin[2] + r.boxMax[2]) * 0.5f, ez = playerEyeZ(p);
			p.pitch = atan2f(cz - ez, fmaxf(hypotf(ax, ay), 1.0f));
			// a post or a pillar between (no clear line chest to chest nor eye to head, as the blow needs): round it, a
			// strafe one way and then the other, no swing meanwhile
			float c1[3] = { p.feet[0], p.feet[1], p.feet[2] + 100.0f }, c2[3] = { r.pos[0], r.pos[1], r.pos[2] + 100.0f };
			float e1[3] = { p.feet[0], p.feet[1], ez }, e2[3] = { r.pos[0], r.pos[1], r.pos[2] + 110.0f };
			if (!w.lineOfSight(c1, c2, false) && !w.lineOfSight(e1, e2, false))
			{
				if (lineTimer <= 0.0f)
					logf("drive: %s: something between us, going round it", id.c_str());
				lineTimer += dt;
				in.moveX = fmodf(lineTimer, 3.2f) < 1.6f ? -1.0f : 1.0f;
				in.moveY = 0.3f;
				return true;
			}
		}
		// (STRIKE's global: the fight's own end, a duel won: no blow after it)
		auto until = strikeUntil.empty() ? w.globals.end() : w.globals.find(strikeUntil);
		if (strikeFor > 0.0f && (swingTimer >= strikeFor || (until != w.globals.end() && until->second >= 1.0f)))
		{
			logf("drive: struck %s for %.1f s", id.c_str(), swingTimer);
			kind = NONE;
			return false;
		}
		// someone else in the line (the crosshair is on a living person who is not the target): no swing, step aside
		// until the line is clear, as a player does not hit a bystander. One fighting us is no bystander: a player in a
		// melee hits whoever of them is in front (Zainsipilu: Aranwen's guards between her and the player)
		// The swing strikes the nearest actor in its arc, not the one under the crosshair: one nearer than the target in it
		// is in the way too (the wounded bear among the rieklings of the Ritual of Beasts took the blows meant for them)
		int inWay = s.target;
		if (inWay < 0 || inWay == ref)
		{
			int sw = s.swingTarget();
			if (sw >= 0 && sw != ref)
				inWay = sw;
		}
		if (inWay >= 0 && inWay != ref && (w.refs[inWay].recType == REC_NPC_ || w.refs[inWay].recType == REC_CREA) && !w.refs[inWay].dead
			&& !(w.refs[inWay].ai == AI_COMBAT && w.refs[inWay].duelFoe < 0 && !w.refs[inWay].ally))
		{
			if (lineTimer <= 0.0f)
				logf("drive: %s is in the way to %s, stepping aside", w.refs[inWay].id.c_str(), id.c_str());
			lineTimer += dt;
			// (a short step either way does not clear a big one, a queen: after 3 s walk round it, facing the target, one way for
			// 6 s then the other, as a player goes round to another side)
			if (lineTimer < 3.0f)
				in.moveX = fmodf(lineTimer, 1.6f) < 0.8f ? -1.0f : 1.0f;
			else
				in.moveX = fmodf(lineTimer - 3.0f, 12.0f) < 6.0f ? -1.0f : 1.0f;
			return true;
		}
		lineTimer = 0.0f;
		// KILL:<id>:unseen: no blow while another living NPC is within fAlarmRadius (the murder rule counts hearing):
		// keep with the target and strike once it is alone, as a player waits for the street to empty
		if (killUnseen)
		{
			float radius = w.game.gmstf("falarmradius", 2000.0f);
			int near = -1;
			for (int i : w.loadedActors)
			{
				const Ref& o = w.refs[i];
				if (i != ref && o.recType == REC_NPC_ && !o.dead && !o.ally && w.active(i) && w.distanceToPlayer(i) < radius)
				{
					near = i;
					break;
				}
			}
			if (near >= 0)
			{
				if (swingTimer == 0.0f && lookTimer == 0.0f)
					logf("drive: %s: %s within earshot, holding the blow", id.c_str(), w.refs[near].id.c_str());
				else if ((int)(lookTimer / 10.0f) != (int)((lookTimer + dt) / 10.0f))
				{
					const Ref& o = w.refs[near];
					logf("drive: %s: still holding after %.0f s: %s %.0f from us, %.0f from it, at %.0f %.0f %.0f (%s)", id.c_str(), lookTimer + dt,
						o.id.c_str(), w.distanceToPlayer(near), hypotf(o.pos[0] - r.pos[0], o.pos[1] - r.pos[1]), o.pos[0], o.pos[1], o.pos[2],
						o.wandering ? "wandering" : "standing");
				}
				lookTimer += dt;
				// v2 kit: still not alone after 8 s: commanded (the command item, touched) to follow, and led to the spot round
				// about farthest from everyone else, then the blow there, as a player draws a mark out of town
				if (s.kitV2() && !ledAway && lookTimer > 8.0f && w.itemCount("uber_command") > 0)
				{
					if (!(r.aiPackage == AIPKG_FOLLOW && lower(r.aiTarget) == "player"))
					{
						if (++commandTries > 3)
						{
							logf("drive: %s does not follow after the command item, three times: holding on", id.c_str());
							ledAway = true;
							return true;
						}
						p.yaw = atan2f(r.pos[0] - p.feet[0], r.pos[1] - p.feet[1]);
						logf("drive: %s: not alone after %.0f s: the command item on them", id.c_str(), lookTimer);
						s.castItem("uber_command");
						return true;
					}
					ledAway = true;
					float spot[3];
					if (!leadSpot(s, ref, spot))
					{
						logf("drive: %s follows, but nowhere round about is out of everyone's earshot: holding on", id.c_str());
						return true;
					}
					logf("drive: %s follows: leading them to %.0f %.0f %.0f, away from the others", id.c_str(), spot[0], spot[1], spot[2]);
					leadEscort = escort;
					escort = id;
					leading = true;
					char walk[96];
					snprintf(walk, sizeof(walk), "WALKTO:@%.0f,%.0f,%.0f", spot[0], spot[1], spot[2]);
					std::string again = retryToken;
					retrying = true;
					start(s, walk);
					retrying = false;
					thenDo = again;
				}
				return true;
			}
		}
		// A shut door between us and it (it holds them, and no blow goes through it): opened first, as a player would, once
		// (a locked one stays shut). Before shut doors held anyone, the foe came through it
		{
			float eye[3] = { p.feet[0], p.feet[1], playerEyeZ(p) }, mid[3] = { r.pos[0], r.pos[1], r.pos[2] + 60.0f };
			if (!w.lineOfSight(eye, mid) && w.lineOfSight(eye, mid, false))
			{
				int door = -1;
				float bd = 300.0f * 300.0f;
				for (int i : w.loadedDoors)
				{
					const Ref& d = w.refs[i];
					if (d.recType != REC_DOOR || d.hasDest || !d.visible() || d.doorTarget != 0.0f || d.doorAngle != 0.0f || i == doorLast)
						continue;
					float ex = d.pos[0] - p.feet[0], ey = d.pos[1] - p.feet[1];
					if (ex * ex + ey * ey < bd && fabsf(d.pos[2] - p.feet[2]) < 200.0f)
					{
						bd = ex * ex + ey * ey;
						door = i;
					}
				}
				// (v2 kit: a locked one unlocked first with what is carried, as on the way: an Ondusi's Unhinging scroll aimed at it)
				if (door >= 0 && w.refs[door].lockLevel > 0 && w.time < unlockWait)
					return true;
				if (door >= 0 && s.kitV2() && w.refs[door].lockLevel > 0 && unlockTriesLeft(door) && w.itemCount("sc_ondusisunhinging") > 0)
				{
					unlockTries++;
					unlockWait = w.time + 1.5f;
					const Ref& d = w.refs[door];
					float ux = (d.boxMin[0] + d.boxMax[0]) * 0.5f - p.feet[0], uy = (d.boxMin[1] + d.boxMax[1]) * 0.5f - p.feet[1];
					p.yaw = atan2f(ux, uy);
					p.pitch = atan2f((d.boxMin[2] + d.boxMax[2]) * 0.5f - playerEyeZ(p), fmaxf(hypotf(ux, uy), 1.0f));
					logf("drive: %s is behind %s, locked (%d): Ondusi's Unhinging on it", id.c_str(), d.id.c_str(), d.lockLevel);
					s.castItem("sc_ondusisunhinging");
					return true;
				}
				if (door >= 0)
				{
					logf("drive: %s is behind %s: opening it", id.c_str(), w.refs[door].id.c_str());
					s.playerActivate(door);
					doorLast = door;
					return true;
				}
			}
		}
		// (a lockpick or probe in the weapon slot: the best carried weapon first, as a player would)
		std::string inHand = handItem(s, true);
		// wind up (attack held), then let go: the strike
		swingTimer += dt;
		if (swingTimer > 6.0f && swingTimer - dt <= 6.0f)
			logf("drive: %s: 6 s of swinging, in hand: %s (fatigue %.0f of %.0f), %.0f away, %.0f up; ref %d at %.0f %.0f %.0f%s, we face %.0f", id.c_str(), inHand.empty() ? "no weapon" : inHand.c_str(),
				w.stats.fatigue, w.stats.fatigueMax, dist, r.pos[2] - p.feet[2], ref, r.pos[0], r.pos[1], r.pos[2], r.visible() ? "" : " (hidden)",
				p.yaw * 57.29578f);
		in.attack = fmodf(swingTimer, 0.7f) < 0.4f;
		return true;
	}
	// ACTIVATE: look at its middle, then press A on what the crosshair finds. Something in front of the middle (a gem
	// lying on the desk, a basket before the book): look over the rest of it, as a player moves the crosshair
	float cz = (r.boxMin[2] + r.boxMax[2]) * 0.5f;
	if (r.actor >= 0 && r.dead)
		cz = fminf(cz, r.boxMin[2] + kCorpsePickHeight * 0.5f);   // a body: where it lies (worldPick)
	float eyeZ = playerEyeZ(p);      // (sneaking lowers the eye: aim from where it is)
	p.pitch = atan2f(cz - eyeZ, fmaxf(dist, 1.0f));
	// still levitating (the flight's potion not run out) and hanging over it: down (L) to its floor first, as a player
	// does, or it is out of the crosshair's reach from up there (the Ghostgate plateau, right after the FLYTO)
	// (held up by something under us, no lower after 1.5 s: aim from here, the door stacked over another one at Balmora)
	// (not on the v2 float up to look over what hides it: that one is up on purpose)
	bool lookingOver = s.kitV2() && lookUpTried && floatWalk && floatUntil >= 0.0f;
	if (p.levitate > 0.0f && !p.onGround && p.feet[2] > r.boxMin[2] + 40.0f && elapsed < 60.0f && !descGaveUp && !lookingOver)
	{
		descTimer += dt;
		if (descTimer > 1.5f)
		{
			descGaveUp = p.feet[2] > descZ - 10.0f;
			descZ = p.feet[2];
			descTimer = 0.0f;
		}
		if (!descGaveUp)
		{
			in.down = true;
			lookTimer = 0.0f;
			return true;
		}
	}
	// walked into it (a floating atronach, a person who stepped into us): the eye inside its box, where the crosshair's
	// ray starts within it and never meets it: a step back first
	{
		float ez = playerEyeZ(p);
		if (p.feet[0] > r.boxMin[0] - 5.0f && p.feet[0] < r.boxMax[0] + 5.0f && p.feet[1] > r.boxMin[1] - 5.0f && p.feet[1] < r.boxMax[1] + 5.0f
			&& ez > r.boxMin[2] && ez < r.boxMax[2])
		{
			p.yaw = atan2f((r.boxMin[0] + r.boxMax[0]) * 0.5f - p.feet[0], (r.boxMin[1] + r.boxMax[1]) * 0.5f - p.feet[1]);
			in.moveY = -1.0f;
			lookTimer = 0.0f;
			return true;
		}
	}
	// v2 kit: one attacking us in the crosshair, between us and it (a skeleton before Dagoth Vemyn's body): fought off
	// first, as a player would, as on a walk
	if (s.testLegit && s.kitV2() && s.target >= 0 && s.target != ref)
	{
		const Ref& t = w.refs[s.target];
		if (!t.dead && !t.ally && t.ai == AI_COMBAT && t.duelFoe < 0)
		{
			if (!frontFight)
			{
				logf("drive: %s attacks us in front of %s: fighting it", t.id.c_str(), id.c_str());
				fightTimer = 0.0f;
			}
			handItem(s, true);
			// turned to it when it has gone round to the side (a swing reaches only within about 40 degrees of where we face:
			// two twilights circling a corpse in Berandas stayed out of every blow)
			float fdx = t.pos[0] - p.feet[0], fdy = t.pos[1] - p.feet[1], fd = sqrtf(fdx * fdx + fdy * fdy);
			if (fd > 1.0f && (fdx * sinf(p.yaw) + fdy * cosf(p.yaw)) / fd < 0.75f)
				p.yaw = atan2f(fdx, fdy);
			fightTimer += dt;
			in.attack = fmodf(fightTimer, 0.7f) < 0.4f;
			frontFight = true;
			lookTimer = 0.0f;
			return true;
		}
	}
	frontFight = false;
	lookTimer += dt;
	if (lookTimer > 0.3f && !aimsAt(s, ref))
	{
		static const float kAim[][3] = { { 0.5f, 0.5f, 0.85f }, { 0.2f, 0.5f, 0.5f }, { 0.8f, 0.5f, 0.5f }, { 0.5f, 0.2f, 0.5f },
			{ 0.5f, 0.8f, 0.5f }, { 0.2f, 0.2f, 0.85f }, { 0.8f, 0.8f, 0.85f }, { 0.2f, 0.8f, 0.85f }, { 0.8f, 0.2f, 0.85f },
			{ 0.5f, 0.5f, 0.2f }, { 0.2f, 0.5f, 0.2f }, { 0.8f, 0.5f, 0.2f }, { 0.5f, 0.2f, 0.2f }, { 0.5f, 0.8f, 0.2f } };
		// v2 kit: the middles of its four sides near the edge too, low and high: the only part to be seen of a barrel under a
		// hammock's box is a sliver of its side out from under it (Caryarel's)
		static const float kFace[][3] = { { 0.5f, 0.02f, 0.4f }, { 0.5f, 0.98f, 0.4f }, { 0.02f, 0.5f, 0.4f }, { 0.98f, 0.5f, 0.4f },
			{ 0.5f, 0.02f, 0.15f }, { 0.5f, 0.98f, 0.15f }, { 0.02f, 0.5f, 0.15f }, { 0.98f, 0.5f, 0.15f } };
		int aimN = s.kitV2() ? 22 : 14, aimK = (int)((lookTimer - 0.3f) / 0.08f) % aimN;
		const float* f = aimK < 14 ? kAim[aimK] : kFace[aimK - 14];
		float ax = r.boxMin[0] + (r.boxMax[0] - r.boxMin[0]) * f[0], ay = r.boxMin[1] + (r.boxMax[1] - r.boxMin[1]) * f[1];
		float az = r.boxMin[2] + (r.boxMax[2] - r.boxMin[2]) * f[2];
		float hx = ax - p.feet[0], hy = ay - p.feet[1];
		p.yaw = atan2f(hx, hy);
		p.pitch = atan2f(az - eyeZ, fmaxf(sqrtf(hx * hx + hy * hy), 1.0f));
	}
	if (!s.menuOpen() && aimsAt(s, ref))
	{
		down |= KEY_A;
		logf("drive: activated %s (crosshair)", id.c_str());
		memcpy(pressAt, p.feet, sizeof(pressAt));
		doorWasOpen = r.recType == REC_DOOR && r.doorTarget != 0.0f;
		kind = pickup ? TAKE : !lootItem.empty() ? LOOTWAIT : (r.recType == REC_NPC_ || r.recType == REC_CREA) ? TALKWAIT : trappedDoor ? DOORWAIT : NONE;
		doorCell = w.current;
		takeTimer = 0.0f;
		return kind != NONE;
	}
	// v2 kit: hidden under something (a hammock hung over Caryarel's barrel): crouched (sneak lowers the eye), as a player
	// would, and look again under it
	// (not for a person: A pressed crouching is a pickpocket, the container screen, not a talk: Adusamsi Assurnarairan)
	if (lookTimer > 1.6f && s.kitV2() && !crouchTried && !s.testSneak && r.recType != REC_NPC_)
	{
		crouchTried = true;
		crouchSet = true;
		s.testSneak = true;
		logf("drive: %s hidden from here: crouching to look under what hides it", id.c_str());
		lookTimer = 0.0f;
		return true;
	}
	// v2 kit: hidden behind something low (a bed before the barrel, a counter): up a little on the fly item, as a player
	// would, and look again over it (Caryarel's shack; v1 got the barrel only while its potions still held it up)
	if (lookTimer > 1.6f && s.kitV2() && !lookUpTried && s.w.itemCount("uber_fly") > 0)
	{
		lookUpTried = true;
		logf("drive: %s hidden from here: floating up to look over it", id.c_str());
		floatUntil = s.w.time + 8.0f;
		floatWalk = true;
		lookUpTimer = 1.5f;
		lookUpBase = p.feet[2];
		lookTimer = 0.0f;
		return true;
	}
	if (lookUpTimer > 0.0f && p.feet[2] < lookUpBase + 110.0f)
	{
		lookUpTimer -= dt;
		lookTimer = 0.0f;
		vertDuty += 500.0f / fmaxf(500.0f, p.flySpeed);
		if (vertDuty >= 1.0f)
		{
			vertDuty -= 1.0f;
			in.up = p.levitate > 0.0f;
		}
	}
	// (up over it: on over it toward the thing, at v1's pace, till within reach of it from above: Caryarel's barrel stands
	// 150 behind the bed, out of the crosshair's 192 from 110 up where the float began)
	if (lookingOver && p.levitate > 0.0f && p.feet[2] >= lookUpBase + 80.0f)
	{
		float bx = (r.boxMin[0] + r.boxMax[0]) * 0.5f - p.feet[0], by = (r.boxMin[1] + r.boxMax[1]) * 0.5f - p.feet[1];
		if (hypotf(bx, by) > 90.0f && !aimsAt(s, ref))
		{
			p.yaw = atan2f(bx, by);
			in.moveY = fminf(0.3f, 550.0f / fmaxf(1.0f, p.flySpeed));
			lookTimer = 0.0f;
		}
	}
	if (lookTimer > 1.6f && s.testLegit && viewTries < 3)
	{
		// hidden from here: stand on another side of it (a quarter turn round each time) and look again
		viewTries++;
		float bx = (r.boxMin[0] + r.boxMax[0]) * 0.5f, by = (r.boxMin[1] + r.boxMax[1]) * 0.5f;
		float half = fmaxf(r.boxMax[0] - r.boxMin[0], r.boxMax[1] - r.boxMin[1]) * 0.5f;
		// a spot round it with floor at our height from which the crosshair (worldPick, as the game aims) finds it:
		// two rings, sixteen ways round, each looked at over the same points of it as the sweep above
		static const float kLook[][3] = { { 0.5f, 0.5f, 0.85f }, { 0.5f, 0.5f, 0.5f }, { 0.3f, 0.5f, 0.7f }, { 0.7f, 0.5f, 0.7f },
			{ 0.5f, 0.3f, 0.7f }, { 0.5f, 0.7f, 0.7f } };
		float base = atan2f(p.feet[0] - bx, p.feet[1] - by), sx = 0.0f, sy = 0.0f, sz = p.feet[2], reach = 192.0f + w.effectTotal(59) * 22.0f;
		bool found = false, seen = false;
		Scene sc = w.scene();
		// first at our height; then at the door's own (we stand on a roof or a slope above it: the yurt's skirt at Mila-Nipal,
		// whose flap is at the foot of the tent), where the floor in front of it is
		for (int pass = 0; pass < 2 && !found; pass++)
		for (int k = 0; k < 32 && !found; k++)
		{
			float zTop = pass == 0 ? p.feet[2] + 60.0f : r.boxMax[2] + 60.0f, zBot = pass == 0 ? p.feet[2] - 60.0f : r.boxMin[2] - 120.0f;
			float ang = base + 0.3927f * (k % 16) * (viewTries == 2 ? -1.0f : 1.0f), rad = half + (k < 16 ? 70.0f : 130.0f);
			float cx = bx + sinf(ang) * rad, cy = by + cosf(ang) * rad, fz = -1e9f;
			for (Cell* c : sc.cells)
			{
				float z;
				if (collisionFloor(c->collision, cx, cy, zTop, zBot, &z) && z > fz)
					fz = z;
			}
			if (fz < -1e8f)
				continue;
			// (a spot right under our feet, on a roof, is not one to walk to)
			if (pass == 1 && fz < p.feet[2] - 60.0f && (cx - p.feet[0]) * (cx - p.feet[0]) + (cy - p.feet[1]) * (cy - p.feet[1]) < 60.0f * 60.0f)
				continue;
			float eye[3] = { cx, cy, fz + PLAYER_EYE_HEIGHT - p.eyeDrop };
			for (const float* f : kLook)
			{
				float d[3] = { r.boxMin[0] + (r.boxMax[0] - r.boxMin[0]) * f[0] - eye[0], r.boxMin[1] + (r.boxMax[1] - r.boxMin[1]) * f[1] - eye[1],
					r.boxMin[2] + (r.boxMax[2] - r.boxMin[2]) * f[2] - eye[2] };
				float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
				if (len < 1.0f)
					continue;
				for (float& c : d)
					c /= len;
				if (worldPick(w, eye, d, reach) == ref)
				{
					// (one we can walk straight to first: a straight line at knee height, not behind the bed or the
					// table that hid it; the first seen spot is kept for when none is)
					float from[3] = { p.feet[0], p.feet[1], p.feet[2] + 30.0f }, to[3] = { cx, cy, fz + 30.0f };
					bool clear = w.lineOfSight(from, to, false);
					if (!seen || clear)
					{
						sx = cx;
						sy = cy;
						sz = fz;
						seen = true;
					}
					found = clear;
					break;
				}
			}
		}
		found = found || seen;
		if (!found)
		{
			fail("hidden from here, and no spot round it where the crosshair finds it");
			return false;
		}
		char walk[96];
		snprintf(walk, sizeof(walk), "WALKTO:@%.0f,%.0f,%.0f", sx, sy, sz);
		logf("drive: %s hidden from here (the crosshair is on %s): trying from another side", id.c_str(),
			s.target >= 0 ? w.refs[s.target].id.c_str() : "nothing");
		std::string again = retryToken;
		retrying = true;
		start(s, walk);
		retrying = false;
		thenDo = again;
		// v2 kit: a spot up a step (on the bed beside Caryarel's barrel, 38 up): floated onto, as a player with the fly item
		// would; the walk on foot held up against its edge
		if (s.kitV2() && sz > p.feet[2] + 20.0f && s.w.itemCount("uber_fly") > 0)
		{
			logf("drive: the spot is %.0f up: floating there", sz - p.feet[2]);
			floatUntil = s.w.time + 8.0f;
			floatWalk = true;
		}
		return true;
	}
	if (lookTimer > 1.6f && s.testLegit)
	{
		std::string why = std::string("the crosshair is on ") + (s.target >= 0 ? w.refs[s.target].id : "nothing") + ", no direct activation in a LEGIT run";
		fail(why.c_str());
		return false;
	}
	if (lookTimer > 1.0f && !s.testLegit)
	{
		// (a player couldn't have: counted apart in the verdict, as warps are)
		logf("drive: crosshair miss: on %s, not %s: activating it directly", s.target >= 0 ? w.refs[s.target].id.c_str() : "nothing",
			id.c_str());
		s.playerActivate(ref);
		kind = pickup ? TAKE : !lootItem.empty() ? LOOTWAIT : NONE;
		takeTimer = 0.0f;
		return kind != NONE;
	}
	return true;
}
