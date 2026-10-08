#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <malloc.h>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "audio.h"
#include "linear.h"
#include "devupdate.h"
#include "log.h"
#include "player.h"
#include "renderer.h"
#include "screenshot.h"
#include "testdrive.h"
#include "zfile.h"
#include "saves.h"
#include "session.h"
#include "ui.h"

extern "C" u32 __ctru_heap_size, __ctru_linear_heap_size;

// Game data: SD card copy wins (fast dev loop), else the copy packed into the CIA RomFS
static const char* kSdDataDir = "sdmc:/3ds/mw3ds/data";
static const char* kRomfsDataDir = "romfs:/data";

#define DISPLAY_TRANSFER_FLAGS \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
	GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
	GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

// ---- Test harness
// sdmc:/3ds/mw3ds/autocam.txt: "x y z yawDeg pitchDeg" or "spawn" per line -> shot_NN.bmp each, then "autocam done".
// sdmc:/3ds/mw3ds/autoinput.txt: "seconds moveX moveY lookX [jump] [KEYS]" per line, KEYS like A, B+DOWN,
//   TAP:x:y (touch) - replayed as input; logs the player every 0.25 s and ends with shot.bmp.
// sdmc:/3ds/mw3ds/autoshot: shot.bmp after 3 s.

struct ScriptedInput
{
	float secs, moveX, moveY, lookX;
	int jump;
	u32 keys;
	int tapX = -1, tapY = -1;
	bool drag = false;                // DRAG:dx:dy: a finger dragged that far across the touch screen this step's first frame
	int dragX = 0, dragY = 0;
	bool teleport = false;
	bool tpInside = false;     // TPIN: to that spot in the interior the player is in (TP from inside goes out)
	float tp[4];            // x y z yawDegrees
	bool waitMessage = false;   // MSG: wait up to `secs` for a message box, then press A on it
	std::string topic;          // TOPIC:name (underscores for spaces): pick a dialogue topic
	std::string door;           // DOOR:id (underscores for spaces): use that door in the current cell
	std::string place;          // PLACE:id:dist: move that actor dist units in front of the player, facing them
	float placeDist = 150.0f;
	int giveCount = 0;                // GIVE:id:count
	int boost = 0;              // BOOST:n: combat skills and attributes to n, health / fatigue / magicka full
	std::string give;           // GIVE:id: add the item (ammunition: 50) and equip it
	std::string gotoCell;       // GOTO:cell: enter that cell through a door that leads there
	std::string journal;        // JOURNAL:quest:index: set a journal stage
	int journalIndex = 0;
	int choice = 0;             // CHOICE:n: answer the open dialogue's choice n
	int choiceValue = 0;        // CHOICEVAL:v: answer the choice whose value is v (what conditions test)
	int skillUp = -1, skillUps = 0;   // SKILLUP:skill:n: raise a skill n points (level progress)
	int evict = -1;                   // EVICT:n: keep at most n objects in memory (drop far cells now)
	std::string check;                // CHECK:id: log that object's state
	std::string follow;               // FOLLOW:id: that NPC follows the player (AI package)
	std::string placePC;              // PLACEPC:id: PlaceAtPC id 1 256 0
	std::string hit;                  // HIT:id: the player's weapon strikes it (a blow that lands)
	bool jail = false;                // JAIL: the player goes to jail (as after an arrest)
	bool enchant = false;             // ENCHANT: the enchanting screen, by the player's own hand
	bool repair = false;              // REPAIR: the repair screen with the first repair tool carried
	std::string wear;                 // WEAR:id:condition: that carried item is worn down to it
	int wearTo = 1;
	std::string tool;                 // TOOL:id: the equipped lockpick / probe is used on it
	std::string brew;                 // BREW:a:b[:c[:d]]: alchemy with those ingredients
	std::string soulKill;             // SOULKILL:id: Soultrap on it, then it dies
	std::string cast;                 // CAST:spell or CAST:item:id: selects it and casts
	std::string join;                 // JOIN:faction[:rank]: the player joins it (rank 0)
	int joinRank = 0;
	std::string talk;                 // TALK:id: step up to that NPC (in memory) and open talk, disposition 100
	std::string setGlobal;            // SETGLOBAL:name:value
	float globalValue = 0.0f;
	std::string recharge;             // RECHARGE:item: with the first soul gem holding a soul
	int persuade = -1;                // PERSUADE:k: in a talk, admire 0 / intimidate 1 / taunt 2 / bribe 3..5
	int weather = -1;                 // WEATHER:k: the current region's weather becomes k (at once)
	int level = 0;                    // LEVEL:n: the player's level becomes n
	int sleep = 0, sleepForce = 0;    // SLEEP:h[:1|2]: sleep h hours here (1: a creature is sure to come, 2: none comes)
	std::string moveTo;               // MOVETO:id: that reference moves 200 in front of the player (PositionCell)
	int effect = -1, effectMag = 0, effectSecs = 0, effectAttr = -1;   // EFFECT:id:magnitude:seconds[:attribute]
	float hour = -1.0f;               // HOUR:h: the time of day
	int days = 0;                     // DAYS:n: n days pass
	std::string spell;                // SPELL:id: the player gets it (AddSpell: diseases, abilities)
	std::string quickSet;             // QUICKSET:k:entry: quick key k set (inv:<item> / magic:<spell>)
	std::string drop;                 // DROP:id: the player drops one on the floor in front
	std::string setLocal;             // SETLOCAL:id:var:value: a script local of that reference
	int rep = -1;                     // REP:n: the player's reputation
	float stickY = 0.0f;              // STICKDOWN / STICKUP: the C-stick pushed down (1) / up (-1) in a menu's text pane
	bool shot = false;                // SHOT: a screenshot (shot_step_NN.bmp) as this step starts
	bool chain = false;               // CHAIN: from here JOURNAL setup only raises (chained chapter runs)
	bool legit = false;               // LEGIT: from here only a player's own actions (TestDriver::playerToken)
	std::vector<std::string> actions; // GOD / WALKTO / KILL / ACTIVATE / PICKUP / EQUIP / EXPECT (testdrive.h)
	std::string startScript;          // STARTSCRIPT:name: a global script an earlier chapter would have started
	std::string saveAs, loadFrom;     // SAVE:name / LOAD:name: sdmc:/3ds/mw3ds/test_<name>.sav (the sweeps
	                                  // start every quest from the same state)
};

// How far (pixels) a finger may wander between press and release and still tap
static const int kTapSlop = 12;

// Set by the first LEGIT step: every later token must be a player's own action (uber quest tests)
static bool s_legitParsed = false;

static u32 parseKeys(const char* s, int* tapX, int* tapY, ScriptedInput* step)
{
	static const struct { const char* name; u32 key; } names[] = {
		{ "A", KEY_A }, { "B", KEY_B }, { "X", KEY_X }, { "Y", KEY_Y }, { "L", KEY_L }, { "R", KEY_R },
		{ "UP", KEY_DUP }, { "DOWN", KEY_DDOWN }, { "LEFT", KEY_DLEFT }, { "RIGHT", KEY_DRIGHT },
		{ "SELECT", KEY_SELECT }, { "START", KEY_START }, { "ZL", KEY_ZL },
	};
	u32 keys = 0;
	char buf[1024];
	strncpy(buf, s, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = 0;
	for (char* tok = strtok(buf, "+"); tok; tok = strtok(nullptr, "+"))
	{
		// '%' stands for a space in ids that also have underscores ("bk_Boethiah's%Glory_unique")
		for (char* c = tok; *c; c++)
			if (*c == '%')
				*c = ' ';
		if (strcmp(tok, "LEGIT") == 0)
			step->legit = s_legitParsed = true;
		else if (s_legitParsed && !TestDriver::playerToken(tok))
			logf("drive: FAIL %s: not a player action (after LEGIT)", tok);
		if (sscanf(tok, "TAP:%d:%d", tapX, tapY) == 2)
			continue;
		if (sscanf(tok, "DRAG:%d:%d", &step->dragX, &step->dragY) == 2)
		{
			step->drag = true;
			continue;
		}
		if (sscanf(tok, "TP:%f:%f:%f:%f", &step->tp[0], &step->tp[1], &step->tp[2], &step->tp[3]) == 4)
		{
			step->teleport = true;
			continue;
		}
		if (sscanf(tok, "TPIN:%f:%f:%f:%f", &step->tp[0], &step->tp[1], &step->tp[2], &step->tp[3]) == 4)
		{
			step->teleport = step->tpInside = true;
			continue;
		}
		if (strcmp(tok, "MSG") == 0)
			step->waitMessage = true;
		if (strcmp(tok, "JAIL") == 0)
			step->jail = true;
		if (strcmp(tok, "SHOT") == 0)
			step->shot = true;
		if (strcmp(tok, "STICKDOWN") == 0)
			step->stickY = 1.0f;
		if (strcmp(tok, "STICKUP") == 0)
			step->stickY = -1.0f;
		if (strcmp(tok, "CHAIN") == 0)
			step->chain = true;
		if (strncmp(tok, "SAVE:", 5) == 0)
			step->saveAs = tok + 5;
		if (strncmp(tok, "STARTSCRIPT:", 12) == 0)
			step->startScript = tok + 12;
		if (strncmp(tok, "LOAD:", 5) == 0)
			step->loadFrom = tok + 5;
		for (const char* verb : { "GOD", "EXPECT:", "WALKTO:", "FLYTO:", "HOPTO:", "ESCORT:", "KILL:", "ACTIVATE:", "PICKUP:", "EQUIP:", "DOORTO:", "LOOT:", "PUT:", "STRIKE:",
				"SNAP:", "CLASS:", "SETSKILL:", "SETATTR:", "SKILLPROG:", "LEVELPROG:", "ENCHANTAT:", "ENCHITEM:", "ENCHGEM:",
				"ENCHTYPE:", "ADDEFFECT:", "CONFIRM", "SPELLMAKE:", "TRAIN:", "BUY:", "SELL:", "LEVELUP:", "READ:", "FACE:",
				"CASTAT:", "SCREEN:", "MAKESPELL:", "USEMADE", "EQUIPMADE", "RECHARGEMADE", "ATTRUPS:", "FILL", "DRINKBREWED", "CASTMADESPELL", "PROBE:", "ACTIVE:", "SETFATIGUE:", "SETREP:", "SETDISP:", "KNOW:", "SETBOUNTY:", "JOURNALADD:", "SETJOURNALINDEX:", "ADVANCE:", "ENABLE:", "DISABLE:", "ALARM:", "ROLL:", "MOVIE:", "BARTER:", "BARTERSEL:", "TRAVEL:", "SAVESEL:", "SETWEATHER:", "CHANGEWEATHER:", "MODREGION:", "SETITEM:", "SETHEALTH:", "SETALARM:", "SETITEMHEALTH:", "SETITEMCHARGE:", "SETDEAD:", "SETTALKED:", "SETRACE:", "KNOCKDOWN:", "SNEAK", "WEREWOLF", "CLOTHVALUE:", "FATIGUEREGEN:", "GIVEPOTION:", "USE:", "USELOCKPICK:", "USEPROBE:", "SEED:", "PCNAME:", "PCRACE:", "PCSEX:", "TYPE:", "TOPICLOG:", "NOTIFY:", "MSGBOX:", "MEMHOLD:" })
			if (strncmp(tok, verb, strlen(verb)) == 0)
				step->actions.push_back(tok);
		if (strcmp(tok, "ENCHANT") == 0)
			step->enchant = true;
		if (strcmp(tok, "REPAIR") == 0)
			step->repair = true;
		if (strncmp(tok, "WEAR:", 5) == 0)
		{
			std::string a = tok + 5;
			size_t colon = a.find(':');
			step->wear = a.substr(0, colon);
			step->wearTo = colon == std::string::npos ? 1 : atoi(a.c_str() + colon + 1);
			for (auto& ch : step->wear)
				ch = ch == '_' ? ' ' : tolower(ch);
			continue;
		}
		if (sscanf(tok, "PERSUADE:%d", &step->persuade) == 1)
			continue;
		if (sscanf(tok, "WEATHER:%d", &step->weather) == 1)
			continue;
		if (sscanf(tok, "LEVEL:%d", &step->level) == 1)
			continue;
		if (sscanf(tok, "SLEEP:%d:%d", &step->sleep, &step->sleepForce) >= 1)
			continue;
		if (sscanf(tok, "BOOST:%d", &step->boost) == 1)
			continue;
		if (sscanf(tok, "CHOICEVAL:%d", &step->choiceValue) == 1)
			continue;
		if (sscanf(tok, "CHOICE:%d", &step->choice) == 1)
			continue;
		if (sscanf(tok, "SKILLUP:%d:%d", &step->skillUp, &step->skillUps) == 2)
			continue;
		if (sscanf(tok, "EVICT:%d", &step->evict) == 1)
			continue;
		if (strncmp(tok, "CHECK:", 6) == 0)
		{
			step->check = tok + 6;
			continue;
		}
		if (sscanf(tok, "EFFECT:%d:%d:%d:%d", &step->effect, &step->effectMag, &step->effectSecs, &step->effectAttr) >= 3)
			continue;
		if (sscanf(tok, "HOUR:%f", &step->hour) == 1)
			continue;
		if (sscanf(tok, "DAYS:%d", &step->days) == 1)
			continue;
		if (strncmp(tok, "SPELL:", 6) == 0)
		{
			step->spell = tok + 6;       // (the id as given: a '_' is read as a space only when no spell has the id so)
			continue;
		}
		if (strncmp(tok, "MOVETO:", 7) == 0)
		{
			step->moveTo = tok + 7;
			continue;
		}
		if (strncmp(tok, "HIT:", 4) == 0)
		{
			step->hit = tok + 4;
			continue;
		}
		if (strncmp(tok, "RECHARGE:", 9) == 0)
		{
			step->recharge = tok + 9;
			for (auto& ch : step->recharge)
				ch = ch == '_' ? ' ' : tolower(ch);
			continue;
		}
		if (strncmp(tok, "JOIN:", 5) == 0)
		{
			step->join = tok + 5;
			size_t colon = step->join.find(':');
			if (colon != std::string::npos)
			{
				step->joinRank = atoi(step->join.c_str() + colon + 1);
				step->join.resize(colon);
			}
			for (auto& ch : step->join)
				ch = ch == '_' ? ' ' : tolower(ch);
			continue;
		}
		if (strncmp(tok, "TALK:", 5) == 0)
		{
			step->talk = tok + 5;
			continue;
		}
		if (strncmp(tok, "SETGLOBAL:", 10) == 0)
		{
			step->setGlobal = tok + 10;
			size_t colon = step->setGlobal.find(':');
			if (colon != std::string::npos)
			{
				step->globalValue = atof(step->setGlobal.c_str() + colon + 1);
				step->setGlobal.resize(colon);
			}
			for (auto& ch : step->setGlobal)
				ch = tolower(ch);
			continue;
		}
		if (strncmp(tok, "CAST:", 5) == 0)
		{
			step->cast = tok + 5;
			continue;
		}
		if (strncmp(tok, "SOULKILL:", 9) == 0)
		{
			step->soulKill = tok + 9;
			continue;
		}
		if (strncmp(tok, "BREW:", 5) == 0)
		{
			step->brew = tok + 5;
			continue;
		}
		if (strncmp(tok, "TOOL:", 5) == 0)
		{
			step->tool = tok + 5;
			continue;
		}
		if (strncmp(tok, "PLACEPC:", 8) == 0)
		{
			step->placePC = tok + 8;
			continue;
		}
		if (strncmp(tok, "FOLLOW:", 7) == 0)
		{
			step->follow = tok + 7;
			continue;
		}
		if (strncmp(tok, "GOTO:", 5) == 0 || strncmp(tok, "JOURNAL:", 8) == 0)
		{
			std::string v = strchr(tok, ':') + 1;
			for (auto& ch : v)
				if (ch == '_' && tok[0] == 'G')
					ch = ' ';
			if (tok[0] == 'G')
				step->gotoCell = v;
			else
			{
				size_t colon = v.rfind(':');
				step->journal = v.substr(0, colon);
				step->journalIndex = colon == std::string::npos ? 0 : atoi(v.c_str() + colon + 1);
			}
			continue;
		}
		if (strncmp(tok, "SETLOCAL:", 9) == 0)
		{
			step->setLocal = tok + 9;
			continue;
		}
		if (strncmp(tok, "REP:", 4) == 0)
		{
			step->rep = atoi(tok + 4);
			continue;
		}
		if (strncmp(tok, "DROP:", 5) == 0)
		{
			step->drop = tok + 5;
			continue;
		}
		if (strncmp(tok, "QUICKSET:", 9) == 0)
		{
			step->quickSet = tok + 9;
			continue;
		}
		if (strncmp(tok, "PLACE:", 6) == 0 || strncmp(tok, "GIVE:", 5) == 0)
		{
			std::string& out = tok[0] == 'P' ? step->place : step->give;
			out = strchr(tok, ':') + 1;
			size_t colon = out.find(':');
			if (colon != std::string::npos)
			{
				step->placeDist = atof(out.c_str() + colon + 1);
				step->giveCount = atoi(out.c_str() + colon + 1);
				out.resize(colon);
			}
			continue;
		}
		if (strncmp(tok, "TOPIC:", 6) == 0 || strncmp(tok, "DOOR:", 5) == 0)
		{
			std::string& out = tok[0] == 'T' ? step->topic : step->door;
			out = strchr(tok, ':') + 1;
			// Topics have spaces; object ids may have underscores (tried both ways when used)
			for (auto& ch : out)
				if (ch == '_' && tok[0] == 'T')
					ch = ' ';
		}
		for (auto& n : names)
			if (strcmp(tok, n.name) == 0)
				keys |= n.key;
	}
	return keys;
}

static std::vector<ScriptedInput> loadAutoinput()
{
	std::vector<ScriptedInput> steps;
	FILE* f = fopen("sdmc:/3ds/mw3ds/autoinput.txt", "r");
	if (!f)
		return steps;
	char line[1024];
	while (fgets(line, sizeof(line), f))
	{
		ScriptedInput s = {};
		s.tapX = s.tapY = -1;
		char keys[1024] = {};
		int n = sscanf(line, "%f %f %f %f %d %1023s", &s.secs, &s.moveX, &s.moveY, &s.lookX, &s.jump, keys);
		if (n >= 4)
		{
			if (n >= 6)
				s.keys = parseKeys(keys, &s.tapX, &s.tapY, &s);
			steps.push_back(s);
		}
	}
	fclose(f);
	logf("autoinput: %d steps", (int)steps.size());
	return steps;
}

struct CamPose
{
	RenderCamera cam;
	std::string cell;          // "" = stay in the current cell
};

static std::vector<CamPose> loadAutocam(const RenderCamera& spawn)
{
	std::vector<CamPose> poses;
	std::string cell;
	FILE* f = fopen("sdmc:/3ds/mw3ds/autocam.txt", "r");
	if (!f)
		return poses;
	char line[128];
	while (fgets(line, sizeof(line), f))
	{
		RenderCamera c;
		float yawDeg, pitchDeg;
		line[strcspn(line, "\r\n")] = 0;
		if (line[0] == '@')
			cell = line + 1;
		else if (strncmp(line, "spawn", 5) == 0)
			poses.push_back({ spawn, cell });
		else if (sscanf(line, "%f %f %f %f %f", &c.pos[0], &c.pos[1], &c.pos[2], &yawDeg, &pitchDeg) == 5)
		{
			c.yaw = C3D_AngleFromDegrees(yawDeg);
			c.pitch = C3D_AngleFromDegrees(pitchDeg);
			poses.push_back({ c, cell });
		}
	}
	fclose(f);
	logf("autocam: %d poses", (int)poses.size());
	return poses;
}

// The C-stick up / down for the menus' text panes: -1 (up) to 1 (down), nothing inside the dead zone
static float readStickScroll()
{
	circlePosition cs;
	hidCstickRead(&cs);
	return fabsf(cs.dy) > 20 ? fmaxf(-1.0f, fminf(1.0f, -cs.dy / 150.0f)) : 0.0f;
}

// Two button layouts (Options > Controls; START and SELECT the same in both):
//   Xbox (Morrowind on the Xbox): R attack, L cast, X ready / put away the weapon, Y jump, B run,
//     ZL sneak (down when levitating), ZR up when levitating, D-pad quick keys
//   Classic: X attack, Y cast, B jump, ZR run, L sneak, ZL + D-pad quick keys, D-pad look, ZL + X put away
static PlayerInput readInput(u32 held, u32 down)
{
	circlePosition cp, cs;
	hidCircleRead(&cp);
	hidCstickRead(&cs);
	PlayerInput in = {};
	in.moveX = fabsf(cp.dx) > 15 ? fmaxf(-1.0f, fminf(1.0f, cp.dx / 150.0f)) : 0.0f;
	in.moveY = fabsf(cp.dy) > 15 ? fmaxf(-1.0f, fminf(1.0f, cp.dy / 150.0f)) : 0.0f;
	in.lookX = fabsf(cs.dx) > 20 ? cs.dx / 150.0f : 0.0f;
	in.lookY = fabsf(cs.dy) > 20 ? cs.dy / 150.0f : 0.0f;
	in.quick = -1;
	if (g_controlLayout == CONTROLS_XBOX)
	{
		if (down & KEY_DUP) in.quick = 0;
		if (down & KEY_DRIGHT) in.quick = 1;
		if (down & KEY_DDOWN) in.quick = 2;
		if (down & KEY_DLEFT) in.quick = 3;
		in.attack = held & KEY_R;
		in.cast = down & KEY_L;
		in.readyToggle = down & KEY_X;
		in.jump = down & KEY_Y;
		in.fast = held & KEY_B;
		in.togglePov = down & KEY_SELECT;
		in.up = held & KEY_ZR;
		in.down = held & KEY_ZL;
		in.sneak = held & KEY_ZL;
		return in;
	}
	if (held & KEY_ZL)
	{
		// quick keys: ZL + a direction
		if (down & KEY_DUP) in.quick = 0;
		if (down & KEY_DRIGHT) in.quick = 1;
		if (down & KEY_DDOWN) in.quick = 2;
		if (down & KEY_DLEFT) in.quick = 3;
	}
	else
	{
		if (held & KEY_DRIGHT) in.lookX += 1.0f;
		if (held & KEY_DLEFT)  in.lookX -= 1.0f;
		if (held & KEY_DUP)    in.lookY += 1.0f;
		if (held & KEY_DDOWN)  in.lookY -= 1.0f;
	}
	in.jump = down & KEY_B;
	in.togglePov = down & KEY_SELECT;
	in.fast = held & KEY_ZR;
	in.up = held & KEY_R;
	in.down = held & KEY_L;
	// ZL + X puts the weapon away (Morrowind's Ready Weapon toggle); X alone draws and strikes
	in.sheathe = (held & KEY_ZL) && (down & KEY_X);
	in.attack = (held & KEY_X) && !(held & KEY_ZL);
	in.cast = down & KEY_Y;
	in.sneak = held & KEY_L;
	return in;
}

// Loading screens show one of Morrowind's splash pictures (a new one for each load)
static std::vector<ArtRef> s_splashes;
static std::string s_splashFor;
static int s_splash = -1;

static void drawLoading(C3D_RenderTarget* top, C3D_RenderTarget* bottom, const char* text,
	const char* detail = nullptr, float seconds = -1.0f)
{
	UiInput none = {};
	uiBeginFrame(none);
	C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
	linearRetire();
	C2D_Prepare();
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
	C2D_TargetClear(top, C2D_Color32(0, 0, 0, 255));
	C2D_SceneBegin(top);
	if (!s_splashes.empty())
	{
		if (s_splashFor != text)
		{
			s_splashFor = text;
			s_splash = rand() % s_splashes.size();
		}
		const ArtRef& a = s_splashes[s_splash];
		if (uiArt(a.file, a.w, a.h, 0, 0, 400, 240))
		{
			uiRect(0, 204, 400, 36, 0xA0000000);
			uiTextCentered(200, 212, 0.5f, col::text, text);
		}
		else
		{
			uiTextCentered(200, 100, 0.8f, col::header, "Morrowind");
			uiTextCentered(200, 132, 0.5f, col::textDim, text);
		}
	}
	else
	{
		uiTextCentered(200, 100, 0.8f, col::header, "Morrowind");
		uiTextCentered(200, 132, 0.5f, col::textDim, text);
	}
	C2D_TargetClear(bottom, C2D_Color32(10, 8, 6, 255));
	C2D_SceneBegin(bottom);
	uiTextCentered(160, 40, 0.5f, col::textDim, "MW3DS");
	// What the loader is doing (its last log line), so a hang shows where it stopped
	if (seconds >= 0.0f)
	{
		char buf[32];
		snprintf(buf, sizeof(buf), "%.0f s", seconds);
		uiTextCentered(160, 80, 0.45f, col::textDim, buf);
	}
	if (detail)
	{
		float y = 110;
		for (auto& line : uiWrap(detail, 300, 0.42f))
		{
			uiText(10, y, 0.42f, col::text, line);
			y += uiLineHeight(0.42f);
			if (y > 220)
				break;
		}
	}
	C3D_FrameEnd(0);
}

// While a load runs, each log line the main thread writes redraws the loading screen with it
// (a few times a second at most)
static struct
{
	C3D_RenderTarget *top, *bottom;
	std::string title;
	u64 start, drawn;
	u32 mainThread;
	std::string last;
} s_progress;

static void loadingProgress(const char* line)
{
	u32 id = 0;
	svcGetThreadId(&id, CUR_THREAD_HANDLE);
	if (id != s_progress.mainThread)
		return;
	s_progress.last = line;
	u64 now = osGetTime();
	if (now - s_progress.drawn < 150)
		return;
	s_progress.drawn = now;
	drawLoading(s_progress.top, s_progress.bottom, s_progress.title.c_str(), line, (now - s_progress.start) / 1000.0f);
}

struct LoadingProgress
{
	LoadingProgress(C3D_RenderTarget* top, C3D_RenderTarget* bottom, const std::string& title)
	{
		s_progress.top = top;
		s_progress.bottom = bottom;
		s_progress.title = title;
		s_progress.start = s_progress.drawn = osGetTime();
		svcGetThreadId(&s_progress.mainThread, CUR_THREAD_HANDLE);
		logSetHook(loadingProgress);
	}
	// The last step, whatever the throttle skipped, and that the first frame comes next: if the
	// screen stays like this, the first frame hung (the GPU)
	~LoadingProgress()
	{
		logSetHook(nullptr);
		std::string text = s_progress.last + "\n\nLoaded. Starting the first frame...";
		drawLoading(s_progress.top, s_progress.bottom, s_progress.title.c_str(), text.c_str(),
			(osGetTime() - s_progress.start) / 1000.0f);
	}
};

// Title screen: new game or any save. Returns the save path, "" for a new game, "quit" on START.
static std::string titleScreen(C3D_RenderTarget* top, C3D_RenderTarget* bottom, const char* dataDir)
{
	std::vector<SaveInfo> saves = savesList(dataDir);
	if (saves.empty())
		return "";
	UiList list;
	int focus = 1;
	bool wasTouching = false, dragged = false;
	int lastX = 0, lastY = 0, pressX = 0, pressY = 0;
	// Test hook (run-emu.ps1 without -Cams/-Inputs): screenshot the title screen, then load the first save
	struct stat st;
	bool shotTest = stat("sdmc:/3ds/mw3ds/autoshot", &st) == 0;
	int frame = 0;
	while (aptMainLoop())
	{
		if (shotTest && ++frame == 60)
		{
			C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
			linearRetire();
			logf("title shot: %s", screenshotSave("sdmc:/3ds/mw3ds/shot_title.bmp") ? "saved" : "FAILED");
			C3D_FrameEnd(0);
			return saves[0].path;
		}
		hidScanInput();
		u32 down = hidKeysDown(), held = hidKeysHeld();
		if (down & KEY_START)
			return "quit";
		UiInput ui = {};
		touchPosition tp;
		hidTouchRead(&tp);
		if (held & KEY_TOUCH)
		{
			ui.touching = true;
			ui.touchX = tp.px;
			ui.touchY = tp.py;
			if (wasTouching)
			{
				ui.dragDX = tp.px - lastX;
				ui.dragDY = tp.py - lastY;
				if (abs(tp.px - pressX) + abs(tp.py - pressY) > kTapSlop)
					dragged = true;
			}
			else
			{
				dragged = false;
				pressX = tp.px;
				pressY = tp.py;
			}
			lastX = tp.px;
			lastY = tp.py;
		}
		else if (wasTouching && !dragged)
		{
			ui.tapped = true;
			ui.tapX = lastX;
			ui.tapY = lastY;
		}
		wasTouching = held & KEY_TOUCH;
		ui.down = down;
		ui.held = held;
		ui.stickY = readStickScroll();
		uiBeginFrame(ui);

		std::string result = "\x01";
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		linearRetire();
		C2D_Prepare();
		C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
		C2D_TargetClear(top, C2D_Color32(0, 0, 0, 255));
		C2D_SceneBegin(top);
		uiTextCentered(200, 80, 0.9f, col::header, "Morrowind");
		uiTextCentered(200, 120, 0.5f, col::textDim, "Choose a save on the touch screen, or start a new game");
		C2D_TargetClear(bottom, C2D_Color32(10, 8, 6, 255));
		C2D_SceneBegin(bottom);
		uiRect(0, 0, 320, 20, col::panelLight);
		uiRect(0, 20, 320, 1, col::border);
		uiText(6, 2, 0.55f, col::header, "Load game");
		int pick = uiSaveList(list, saves, 4, 24, 312, 176);
		if (down & KEY_RIGHT) focus = (focus + 1) % 2;
		if (down & KEY_LEFT) focus = (focus + 1) % 2;
		if (uiButton(4, 208, 154, 28, "New game", focus == 0))
			result = "";
		if (uiButton(162, 208, 154, 28, "Load", focus == 1) || (pick >= 0 && (down & KEY_A) && focus == 1))
			result = saves[list.selected].path;
		C3D_FrameEnd(0);
		if (result != "\x01")
			return result;
	}
	return "quit";
}

int main()
{
	bool isNew3ds = false;
	APT_CheckNew3DS(&isNew3ds);
	if (isNew3ds)
		osSetSpeedupEnable(true);

	gfxInitDefault();
	// GPU commands of a whole frame (both eyes, bottom screen): overflowing it is a panic, so it is
	// generous, the renderer stops drawing near the end and 3D backs off when one eye uses too much
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 8);   // room for both eyes of a busy town
	C2D_Init(4096);
	C2D_Prepare();
	irrstInit();
	logInit();
	crashHandlerInstall(0);
	watchdogStart();
	linearLockInit();
	logf("MW3DS start, %s, heap %lu KB, linear %lu KB", isNew3ds ? "New 3DS" : "Old 3DS",
		__ctru_heap_size / 1024, __ctru_linear_heap_size / 1024);
	bool romfs = R_SUCCEEDED(romfsInit());
	struct stat st;
	// The PC that built the CIA listens for the log (tools/build/make-cia.ps1 writes romfs loghost.txt)
	logNetStart(stat("sdmc:/3ds/mw3ds/loghost.txt", &st) == 0 ? "sdmc:/3ds/mw3ds/loghost.txt" : "romfs:/loghost.txt");
	logf("memory: %s, heap %lu KB, linear %lu KB (%lu KB free)", isNew3ds ? "New 3DS" : "Old 3DS",
		__ctru_heap_size / 1024, __ctru_linear_heap_size / 1024, linearSpaceFree() / 1024);

	C3D_RenderTarget* top = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	C3D_RenderTargetSetOutput(top, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
	// Right eye for stereo 3D (only drawn while the 3D mode is on)
	C3D_RenderTarget* topRight = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	C3D_RenderTargetSetOutput(topRight, GFX_TOP, GFX_RIGHT, DISPLAY_TRANSFER_FLAGS);
	bool stereoOn = false;
	C3D_RenderTarget* bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
	rendererInit();
	uiInit();
	// Development builds bring their data and code up to date from the PC first (devupdate.h)
	DevUpdateResult dev = devUpdate([&](const std::string& text, const std::string& detail) {
		drawLoading(top, bottom, text.c_str(), detail.c_str());
	});
	if (dev == DEV_RELAUNCH || dev == DEV_QUIT)
	{
		// The system closes this build and starts the new one
		for (int i = 0; i < 600 && aptMainLoop(); i++)
			gspWaitForVBlank();
		uiExit();
		rendererExit();
		C2D_Fini();
		C3D_Fini();
		if (romfs)
			romfsExit();
		irrstExit();
		logExit();
		gfxExit();
		return 0;
	}
	devEmptyTrash();
	// A CIA with its data inside uses that (whatever an older copy left on the card); the dev build and
	// the program-only CIA read the card's
	const char* dataDir = romfs && stat("romfs:/data/game.json", &st) == 0 ? kRomfsDataDir
		: stat(kSdDataDir, &st) == 0 ? kSdDataDir : kRomfsDataDir;
	logf("romfs %s, data dir %s", romfs ? "mounted" : "absent", dataDir);
	audioInit(dataDir);
	drawLoading(top, bottom, "Loading...");

	Session* session = new Session();
	std::vector<ScriptedInput> autoinput = loadAutoinput();
	session->autotest = !autoinput.empty();
	u64 loadStart = osGetTime();
	// sdmc:/3ds/mw3ds/start.txt: name of the cell to start in (e.g. "Seyda Neen"), skipping chargen,
	// or "save:<path>" to continue from that save (test runs skip the title screen)
	std::string startCell, startSave;
	if (FILE* sf = fopen("sdmc:/3ds/mw3ds/start.txt", "r"))
	{
		char line[128] = {};
		if (fgets(line, sizeof(line), sf))
		{
			line[strcspn(line, "\r\n")] = 0;
			startCell = line;
		}
		fclose(sf);
		logf("start.txt: %s", startCell.c_str());
		if (startCell.compare(0, 5, "save:") == 0)
		{
			startSave = startCell.substr(5);
			startCell.clear();
		}
	}
	// Normal play: title screen to pick a save (autosave, own saves, bundled starting points) or a new
	// game. Scripted test runs and start.txt skip it and start fresh.
	static const char* kSavePath = kAutosavePath;
	bool testRun = session->autotest || stat("sdmc:/3ds/mw3ds/autocam.txt", &st) == 0;
	if (!testRun && startCell.empty() && startSave.empty())
	{
		startSave = titleScreen(top, bottom, dataDir);
		if (startSave == "quit")
		{
			delete session;
			audioExit();
			uiExit();
			rendererExit();
			C2D_Fini();
			C3D_Fini();
			if (romfs)
				romfsExit();
			irrstExit();
			logExit();
			gfxExit();
			return 0;
		}
		logf("title: %s", startSave.empty() ? "new game" : startSave.c_str());
		drawLoading(top, bottom, "Loading...");
		loadStart = osGetTime();
	}
	session->dataDir = dataDir;
	bool loaded;
	{
		LoadingProgress progress(top, bottom, "Loading...");
		loaded = session->start(dataDir, startCell, startSave.empty() ? nullptr : startSave.c_str());
		if (loaded)
		{
			logf("heap: session %d KB", mallinfo().uordblks / 1024);
			session->vm.load(session->w);
			logf("heap: + view model %d KB", mallinfo().uordblks / 1024);
			uiLoadTheme(dataDir, session->w.game.ui);
			logf("heap: + ui theme %d KB", mallinfo().uordblks / 1024);
			s_splashes = session->w.game.art.splash;
		}
	}
	session->savePath = kSavePath;      // test runs still save (at the end of scripted input)
	logf("load %s in %llu ms, linear free %lu KB, heap used %d KB", loaded ? "ok" : "FAILED",
		osGetTime() - loadStart, linearSpaceFree() / 1024, mallinfo().uordblks / 1024);

	RenderCamera spawnCam = { { session->w.player.feet[0], session->w.player.feet[1],
		session->w.player.feet[2] + PLAYER_EYE_HEIGHT }, session->w.player.yaw, 0.0f };
	std::vector<CamPose> autocam = loadAutocam(spawnCam);
	size_t poseIndex = 0, inputIndex = 0;
	int poseFrames = 0;
	float inputTime = 0.0f;
	bool autoShot = autocam.empty() && autoinput.empty() && stat("sdmc:/3ds/mw3ds/autoshot", &st) == 0;
	bool stepShot = false;
	int stepShots = 0;
	TestDriver driver;
	bool chainMode = false;
	bool stepBlocking = false;           // this step waits for the driver (a walk, a kill, an activation)

	u64 lastTick = svcGetSystemTick(), statTick = lastTick;
	int frames = 0;
	float cmdPeak = 0.0f;          // most of the GPU command buffer a frame used this second
	float t = 0.0f;
	bool touchWasDown = false, dragged = false;
	int lastTouchX = 0, lastTouchY = 0, pressX = 0, pressY = 0;

	while (aptMainLoop())
	{
		hidScanInput();
		irrstScanInput();
		u32 down = hidKeysDown(), held = hidKeysHeld(), up = hidKeysUp();
		if (!loaded && (down & KEY_START))
			break;
		if (loaded && session->wantQuit)
			break;

		u64 now = svcGetSystemTick();
		float dt = fminf((float)(now - lastTick) / SYSCLOCK_ARM11, 0.1f);
		lastTick = now;
		t += dt;

		// Touch: taps and drags for the bottom-screen UI
		UiInput ui = {};
		touchPosition tp;
		hidTouchRead(&tp);
		bool touching = held & KEY_TOUCH;
		if (session->autotest)
			g_controlLayout = CONTROLS_CLASSIC;     // the scripted tests press the classic buttons
		PlayerInput in = readInput(held, down);
		bool autoShotNow = false;

		if (inputIndex < autoinput.size() && loaded)
		{
			dt = 1.0f / 30.0f;       // fixed steps so scripted runs are repeatable
			ScriptedInput& s = autoinput[inputIndex];
			in = {};
			in.moveX = s.moveX;
			in.moveY = s.moveY;
			in.lookX = s.lookX;
			in.attack = (s.keys & KEY_X) && !(s.keys & KEY_ZL);     // ZL + X puts the weapon away
			in.cast = inputTime == 0.0f && (s.keys & KEY_Y);
			in.sneak = s.keys & KEY_L;
			in.up = s.keys & KEY_R;
			in.down = s.keys & KEY_L;
			bool first = inputTime == 0.0f;
			in.jump = s.jump && first;
			down = first ? s.keys : 0;
			held = s.keys;
			in.sheathe = (held & KEY_ZL) && (down & KEY_X);
			if (held & KEY_ZL)
			{
				const u32 dirs[4] = { KEY_DUP, KEY_DRIGHT, KEY_DDOWN, KEY_DLEFT };
				for (int k = 0; k < 4; k++)
					if (down & dirs[k])
						in.quick = k;
			}
			else
			{
				if (held & KEY_DUP) in.lookY += 1.0f;
				if (held & KEY_DDOWN) in.lookY -= 1.0f;
			}
			if (first && !s.door.empty())
			{
				int ref = session->testFindRef(s.door);
				logf("autoinput: door %s %s", s.door.c_str(), session->w.active(ref) ? "used" : "not in this cell");
				if (session->w.active(ref))
					session->playerActivate(ref);
			}
			if (first && s.boost > 0)
				session->testBoost(s.boost);
			if (first && s.jail)
				session->goToJail();
			if (first && s.shot)
				stepShot = true;
			if (first && s.legit)
			{
				session->testLegit = true;
				logf("test: LEGIT: from here only what a player can do");
			}
			if (first && s.chain)
			{
				chainMode = true;
				logf("test: chained run (journal setup only raises)");
				// A bounty the last chapter's driving ran up (a book taken in sight) is paid, as a player would
				// between chapters: else the first guard's arrest screen holds every step after it
				if (session->w.bounty > 0)
				{
					logf("test: chained run: bounty %d paid", session->w.bounty);
					session->w.bounty = 0;
					session->w.arrestDeclined = -1;
				}
				if (session->screen == SCR_ARREST)
					session->closeScreen();
			}
			if (first && !s.startScript.empty())
			{
				// STARTSCRIPT:name:ref: started on an object, as ref->StartScript does
				std::string nm = s.startScript, on;
				size_t colon = nm.find(':');
				if (colon != std::string::npos)
				{
					on = nm.substr(colon + 1);
					nm = nm.substr(0, colon);
				}
				session->w.startGlobalScript(nm, on.empty() ? -1 : session->w.findRef(on));
				logf("test: started script %s", s.startScript.c_str());
			}
			if (first && !s.saveAs.empty())
			{
				std::string p = "sdmc:/3ds/mw3ds/test_" + s.saveAs + ".sav";
				logf("test: saved %s: %s", p.c_str(), session->w.saveGame(p.c_str()) ? "ok" : "FAILED");
			}
			if (first && !s.loadFrom.empty())
			{
				session->reloadPath = "sdmc:/3ds/mw3ds/test_" + s.loadFrom + ".sav";
				session->wantReload = true;
				logf("test: loading %s", session->reloadPath.c_str());
			}
			if (first)
			{
				stepBlocking = false;
				for (auto& act : s.actions)
					stepBlocking |= driver.start(*session, act);
			}
			if (first && s.enchant)
				session->openEnchanting(-1);
			if (first && !s.wear.empty())
				for (auto& it : session->w.inventory)
					if (it.id == s.wear)
						it.condition = s.wearTo;
			if (first && s.repair)
				for (size_t i = 0; i < session->w.inventory.size(); i++)
					if (const Object* o = session->w.game.object(session->w.inventory[i].id))
						if (o->type == "REPA")
						{
							session->openRepair(-1, i);
							break;
						}
			if (first && !s.recharge.empty())
			{
				int item = -1, gem = -1;
				for (size_t i = 0; i < session->w.inventory.size(); i++)
				{
					if (session->w.inventory[i].id == s.recharge)
						item = i;
					if (!session->w.inventory[i].soul.empty() && gem < 0)
						gem = i;
				}
				if (item >= 0 && gem >= 0)
					session->rechargeItem(item, gem);
				else
					logf("test: nothing to recharge %s with", s.recharge.c_str());
			}
			if (first && !s.join.empty())
			{
				if (s.joinRank < 0)
				{
					// JOIN:faction:-1 leaves it (the sweeps undo a quest's setup)
					session->w.pcRank.erase(s.join);
					session->w.pcExpelled.erase(s.join);
					logf("test: left %s", s.join.c_str());
				}
				else
				{
					// (chained: never below the rank the last chapter's story gave)
					auto had = session->w.pcRank.find(s.join);
					if (!chainMode || had == session->w.pcRank.end() || had->second < s.joinRank)
						session->w.pcRank[s.join] = s.joinRank;
					logf("test: joined %s at rank %d", s.join.c_str(), session->w.pcRank[s.join]);
				}
			}
			if (first && !s.cast.empty())
			{
				if (session->testLegit && !TestDriver::canCast(*session, s.cast))
					logf("drive: FAIL cast %s: not a known spell or a carried item (LEGIT)", s.cast.c_str());
				else
				{
					// the id as given when there is such a spell (or item), else with '_' as spaces
					std::string sel = s.cast;
					if (sel.compare(0, 5, "item:") != 0 && !session->w.game.spells.count(lower(sel)))
					{
						std::string sp = lower(sel);
						std::replace(sp.begin(), sp.end(), '_', ' ');
						if (session->w.game.spells.count(sp))
							sel = sp;
					}
					session->w.stats.selectedSpell = sel;
					session->castSpell();
				}
			}
			if (first && !s.soulKill.empty())
			{
				int ref = session->testFindRef(s.soulKill);
				// a living one (several can share the id)
				std::string want = session->w.refs.size() && ref >= 0 ? session->w.refs[ref].idLower : "";
				for (size_t i = 0; i < session->w.refs.size() && ref >= 0; i++)
					if (session->w.refs[i].idLower == want && !session->w.refs[i].dead && session->w.active(i))
					{
						ref = i;
						break;
					}
				if (session->w.active(ref) && !session->w.refs[ref].dead)
				{
					session->w.refs[ref].soulTrapUntil = session->w.time + 30.0f;
					session->killNpc(ref, false);          // no witness, no crime: the test isn't about the law
				}
				logf("test: soulkill %s %s", s.soulKill.c_str(), session->w.active(ref) ? "done" : "not loaded");
			}
			if (first && !s.brew.empty())
			{
				session->alchemySlots.clear();
				std::string rest = s.brew;
				while (!rest.empty())
				{
					size_t c = rest.find(':');
					session->alchemySlots.push_back(rest.substr(0, c));
					rest = c == std::string::npos ? "" : rest.substr(c + 1);
				}
				session->brewPotion();
			}
			// (a rest still running from the step before, whose seconds were shorter: not started over)
			if (first && s.sleep > 0 && session->restDone >= 0 && session->screen == SCR_REST)
				logf("drive: SLEEP %d ignored: a rest is still running (%d of %d hours)", s.sleep, session->restDone, session->restTotal);
			else if (first && s.sleep > 0)
			{
				session->openRest(false);
				session->startRest(s.sleep);
				if (s.sleepForce == 2)
					session->restInterruptAt = -1;     // 2: nothing comes
				else if (s.sleepForce && session->restInterruptList.empty())
				{
					const LevelCell& lc = session->w.cells[session->w.current];
					auto rg = session->w.game.regions.find(lc.region);
					if (rg != session->w.game.regions.end())
						session->restInterruptList = rg->second.sleep;
				}
				if (s.sleepForce == 1)
					session->restInterruptAt = 1;
			}
			if (first && s.level > 0)
				session->w.stats.level = s.level;
			if (first && s.weather >= 0 && session->w.current >= 0)
			{
				World& ww = session->w;
				ww.setWeatherHere(s.weather);
				logf("test: weather %d in %s", s.weather, ww.cells[ww.current].region.c_str());
			}
			if (first && s.persuade >= 0 && session->dlg.open && !session->dlg.choices.empty())
				logf("drive: FAIL persuade %d ignored: a choice is open (%d choices): answer it first (CHOICE)", s.persuade, (int)session->dlg.choices.size());
			else if (first && s.persuade >= 0 && session->dlg.open)
				session->persuade(s.persuade);
			if (first && !s.tool.empty())
			{
				int ref = session->testFindRef(s.tool);
				InventoryItem* t = session->playerToolItem();
				if (session->w.active(ref) && t)
				{
					session->target = ref;
					session->useTool(*t);
				}
				else
					logf("test: no tool or %s not loaded", s.tool.c_str());
			}
			for (int k = 0; first && k < s.skillUps; k++)
				session->raiseSkill(s.skillUp);
			if (first && s.days > 0)
				session->w.gameHour += 24.0f * s.days;
			if (first && s.hour >= 0.0f)
			{
				session->w.gameHour = floorf(session->w.gameHour / 24.0f) * 24.0f + s.hour;
				session->w.weatherSeen = session->w.gameHour;      // (setting the hour is not time passing)
			}
			if (first && s.rep >= 0)
				session->w.pcReputation = s.rep;
			if (first && !s.setLocal.empty())
			{
				// id:var:value (id may hold underscores for spaces)
				size_t a = s.setLocal.find(':'), b = s.setLocal.rfind(':');
				if (a != std::string::npos && b > a)
				{
					std::string id = s.setLocal.substr(0, a), var = s.setLocal.substr(a + 1, b - a - 1);
					float v = atof(s.setLocal.c_str() + b + 1);
					int ri = session->w.findRefAnywhere(id);
					if (ri < 0)
					{
						std::string spaced = id;
						std::replace(spaced.begin(), spaced.end(), '_', ' ');
						ri = session->w.findRefAnywhere(spaced);
					}
					float* p = ri >= 0 && session->w.refs[ri].script >= 0 ? session->w.scripts[session->w.refs[ri].script].local(var) : nullptr;
					if (p)
						*p = v;
					logf("test: local %s.%s = %g%s", id.c_str(), var.c_str(), v, p ? "" : " (no such local)");
				}
			}
			if (first && !s.drop.empty())
			{
				World& ww = session->w;
				std::string id = lower(s.drop);
				if (ww.itemCount(id) > 0)
				{
					ww.removeItem(id, 1);
					ww.dropItemAt({ id, 1, false }, ww.player.feet, ww.player.yaw, 50.0f);
				}
				else
					logf("test: no %s to drop", id.c_str());
			}
			if (first && !s.setGlobal.empty())
			{
				session->w.setGlobal(s.setGlobal, s.globalValue);
				logf("test: global %s = %g", s.setGlobal.c_str(), s.globalValue);
			}
			if (first && !s.talk.empty())
			{
				World& ww = session->w;
				int ri = session->testFindRef(s.talk);
				if (ri < 0 || !ww.active(ri))
					logf("test: talk %s: not here", s.talk.c_str());
				else if (!ww.refs[ri].visible() || ww.refs[ri].dead)
					logf("test: talk %s: disabled or dead", s.talk.c_str());
				else
				{
					Ref& r = ww.refs[ri];
					if (r.ai == AI_COMBAT)
					{
						// an earlier test left them fighting (the sweep runs quests back to back)
						logf("test: talk %s: was fighting, calmed", s.talk.c_str());
						r.ai = AI_IDLE;
						r.fight = 0;
					}
					float fx = sinf(r.rot[2]), fy = cosf(r.rot[2]);
					float to[4] = { r.pos[0] + fx * 90.0f, r.pos[1] + fy * 90.0f, r.pos[2] + 10.0f, 0.0f };
					Player& p = ww.player;
					memcpy(p.feet, to, sizeof(p.feet));
					p.yaw = atan2f(r.pos[0] - to[0], r.pos[1] - to[1]);
					p.vz = 0.0f;
					p.fallTop = p.feet[2];
					p.landedFall = 0.0f;
					r.disposition = 100;
					session->playerActivate(ri);
					logf("test: talk %s (%s)", s.talk.c_str(), session->dlg.open ? "talking" : "no talk");
				}
			}
			if (first && s.quickSet.size() > 2)
			{
				session->w.stats.quickKeys[(s.quickSet[0] - '0') & 3] = s.quickSet.substr(2);
				logf("test: quick key %s", s.quickSet.c_str());
			}
			if (first && !s.spell.empty())
			{
				PlayerStats& ps = session->w.stats;
				std::string sid = lower(s.spell);
				if (!session->w.game.spells.count(sid))
					std::replace(sid.begin(), sid.end(), '_', ' ');    // ids with spaces: "vampire_blood_aundae"
				if (std::find(ps.spells.begin(), ps.spells.end(), sid) == ps.spells.end())     // (known once, as AddSpell)
					ps.spells.push_back(sid);
				float hp = ps.health;
				session->w.recomputeStats();
				ps.health = fminf(ps.healthMax, hp);
				logf("test: spell %s", sid.c_str());
			}
			if (first && s.effect >= 0)
			{
				// (the last number is the skill for the skill effects: fortify, drain, damage, restore, absorb skill)
				bool skillEffect = s.effect == 83 || s.effect == 21 || s.effect == 26 || s.effect == 78 || s.effect == 89;
				SpellEffect e = { s.effect, skillEffect ? s.effectAttr : -1, skillEffect ? -1 : s.effectAttr, s.effectMag, s.effectMag, s.effectSecs, 0 };
				session->applyEffectToPlayer(e, "test");
			}
			if (first && !s.moveTo.empty())
			{
				World& w = session->w;
				int ri = w.findRefAnywhere(s.moveTo);
				if (ri < 0)
					ri = w.findRefAnywhere(std::string(s.moveTo).replace(s.moveTo.find('_') == std::string::npos ? 0 : s.moveTo.find('_'), s.moveTo.find('_') == std::string::npos ? 0 : 1, " "));
				float pos[3] = { w.player.feet[0] + sinf(w.player.yaw) * 200.0f, w.player.feet[1] + cosf(w.player.yaw) * 200.0f,
					w.player.feet[2] };
				if (ri >= 0)
					w.relocate(ri, w.current, pos, w.player.yaw + 3.14159265f);
				else
					logf("test: %s not found", s.moveTo.c_str());
			}
			if (first && !s.placePC.empty())
				session->w.placeAt(s.placePC, 1, 256.0f, 0, session->w.player.feet, session->w.player.yaw);
			if (first && !s.follow.empty())
			{
				int ri = session->testFindRef(s.follow);
				if (ri >= 0)
				{
					session->w.refs[ri].aiPackage = AIPKG_FOLLOW;
					session->w.refs[ri].aiTarget = "player";
					session->w.refs[ri].aiDone = false;
					session->w.refs[ri].fight = 0;       // a companion, not a creature about to attack
					session->w.refs[ri].ai = AI_IDLE;
				}
			}
			if (first && s.evict >= 0)
				session->evictFarCells(s.evict);
			if (first && !s.check.empty())
			{
				int ri = session->testFindRef(s.check);
				if (s.check.compare(0, 8, "journal:") == 0)
				{
					std::string q = s.check.substr(8);
					for (auto& ch : q)
						ch = tolower(ch);
					auto it = session->w.journalIndex.find(q);
					logf("check: journal %s = %d", q.c_str(), it != session->w.journalIndex.end() ? it->second : 0);
				}
				else if (s.check.compare(0, 7, "global:") == 0)
				{
					std::string g = s.check.substr(7);
					for (auto& ch : g)
						ch = tolower(ch);
					auto it = session->w.globals.find(g);
					logf("check: global %s = %g", g.c_str(), it != session->w.globals.end() ? it->second : 0.0f);
				}
				else if (s.check == "player")
				{
					const PlayerStats& ps = session->w.stats;
					logf("check: player in %s at %.0f %.0f %.0f str %d int %d health %.0f/%.0f effects %d",
						session->w.cellName().c_str(), session->w.player.feet[0], session->w.player.feet[1],
						session->w.player.feet[2], ps.attributes[0], ps.attributes[1], ps.health, ps.healthMax,
						(int)session->w.effects.size());
				}
				else if (ri < 0)
					logf("check: %s not in memory", s.check.c_str());
				else
					logf("check: %s cell %s enabled %d taken %d dead %d at %.0f %.0f %.0f rot %.2f %.2f %.2f ai %d done %d", s.check.c_str(),
						session->w.cells[session->w.refs[ri].cell].file.c_str(), session->w.refs[ri].enabled,
						session->w.refs[ri].pickedUp, session->w.refs[ri].dead, session->w.refs[ri].pos[0],
						session->w.refs[ri].pos[1], session->w.refs[ri].pos[2], session->w.refs[ri].rot[0], session->w.refs[ri].rot[1],
						session->w.refs[ri].rot[2], session->w.refs[ri].aiPackage,
						session->w.refs[ri].aiDone);
				if (ri >= 0 && session->w.refs[ri].actor >= 0)
				{
					const Ref& cr = session->w.refs[ri];
					std::string p;
					for (int k : cr.path)
						p += " " + std::to_string((int)session->w.pathPoints[k].pos[0]) + "," + std::to_string((int)session->w.pathPoints[k].pos[1]);
					logf("check: %s moving: path%s stuck %.2f grid %d ai-state %d", s.check.c_str(), p.empty() ? " (straight)" : p.c_str(),
						cr.stuckTimer, session->w.time < cr.gridUntil, cr.ai);
				}
				// What it holds (containers, NPCs)
				if (ri >= 0 && !session->w.refs[ri].contents.empty())
				{
					std::string held;
					for (auto& c : session->w.refs[ri].contents)
						held += " " + std::to_string(c.first) + "x" + c.second;
					logf("check: %s holds%s", s.check.c_str(), held.substr(0, 200).c_str());
				}
				// Its script's locals (what a scripted scene counted)
				int sc = ri >= 0 ? session->w.refs[ri].script : -1;
				if (sc >= 0 && sc < (int)session->w.scripts.size() && session->w.scripts[sc].script)
				{
					const ScriptInstance& si = session->w.scripts[sc];
					std::string text;
					for (size_t k = 0; k < si.locals.size() && k < si.script->localNames.size(); k++)
						text += " " + si.script->localNames[k] + "=" + std::to_string((int)si.locals[k]);
					logf("check: %s script %s%s", s.check.c_str(), si.script->name.c_str(), text.c_str());
				}
			}
			if (first && !s.give.empty())
				session->testGive(s.give, s.giveCount, chainMode);
			if (first && !s.place.empty())
				session->testPlace(s.place, s.placeDist);
			if (first && !s.hit.empty())
				session->testHit(s.hit);
			if (first && !s.journal.empty())
			{
				// CHAIN (a chapter run from the last one's save): setup only raises, never undoes progress
				if (!chainMode || session->w.getJournal(s.journal) < s.journalIndex)
					session->w.setJournal(s.journal, s.journalIndex);
				logf("test: journal %s %d", s.journal.c_str(), s.journalIndex);
			}
			if (first && !s.gotoCell.empty())
				session->testGoto(s.gotoCell);
			if (first && s.choiceValue > 0 && session->dlg.open)
			{
				bool found = false;
				for (auto& ch : session->dlg.choices)
					if (ch.second == s.choiceValue)
					{
						logf("autoinput: choice value %d (%s)", s.choiceValue, ch.first.c_str());
						dialogueChoose(session->dlg, session->w, *session, ch.second);
						found = true;
						break;
					}
				if (!found)
					logf("autoinput: choice value %d not offered (%d choices)", s.choiceValue, (int)session->dlg.choices.size());
			}
			if (first && s.choice > 0 && session->dlg.open)
			{
				// As in the game: only a choice the talk offers (the k-th button)
				if (s.choice <= (int)session->dlg.choices.size())
				{
					int value = session->dlg.choices[s.choice - 1].second;
					logf("autoinput: choice %d (%s)", s.choice, session->dlg.choices[s.choice - 1].first.c_str());
					dialogueChoose(session->dlg, session->w, *session, value);
				}
				else
					logf("autoinput: choice %d not offered (%d choices)", s.choice, (int)session->dlg.choices.size());
			}
			if (first && !s.topic.empty() && session->dlg.open)
			{
				logf("autoinput: topic %s (available: %d)", s.topic.c_str(), (int)session->dlg.topics.size());
				// A player can only pick what the list shows: a topic not in it is a failure (asked anyway, so
				// the rest of the test still runs)
				const Topic* t = session->w.game.topic(s.topic);
				bool listed = false;
				for (auto& name : session->dlg.topics)
					listed |= t && name == t->name;
				if (t && !listed)
				{
					std::string by = dialogueNamedBy(t->lower);
					logf("drive: FAIL topic %s not offered (%s%s%s, %s)", t->name.c_str(),
						session->w.knownTopics.count(t->lower) ? "known" : "never learned",
						by.empty() ? "" : "; named by ", by.empty() ? "" : (by + ", who had no answer").c_str(),
						dialogueFindInfo(session->w, *t, session->dlg.ref, -1) ? "the speaker has an answer" : "no answer from the speaker");
				}
				if (!session->dlg.choices.empty())
					logf("drive: FAIL topic %s ignored: a choice is open (%d choices): answer it first (CHOICE)", s.topic.c_str(),
						(int)session->dlg.choices.size());
				// (LEGIT: not asked; a player has no way to)
				if (listed || !t || !session->testLegit)
					dialogueTopic(session->dlg, session->w, *session, s.topic);
			}
			if (s.waitMessage && !session->messages.empty())
			{
				logf("autoinput: dismiss \"%.50s\"", session->messages.front().text.c_str());
				down = held = KEY_A;
				inputTime = s.secs;
			}
			if (first && s.teleport && !s.tpInside && session->w.current >= 0 && session->w.cells[session->w.current].interior)
			{
				// from inside to world coordinates: out to the exterior cell there (as a door would)
				int dest = session->w.gridCell((int)floorf(s.tp[0] / 8192.0f), (int)floorf(s.tp[1] / 8192.0f));
				if (dest >= 0)
				{
					session->travelCell = dest;
					memcpy(session->travelPos, s.tp, sizeof(session->travelPos));
					session->travelYaw = C3D_AngleFromDegrees(s.tp[3]);
					logf("test: out to %s", session->w.cells[dest].name.c_str());
				}
			}
			else if (first && s.teleport)
			{
				Player& p = session->w.player;
				p.feet[0] = s.tp[0];
				p.feet[1] = s.tp[1];
				p.feet[2] = s.tp[2];
				p.yaw = C3D_AngleFromDegrees(s.tp[3]);
				p.pitch = 0.0f;
				p.vz = 0.0f;
				p.fallTop = p.feet[2];         // a test teleport isn't a fall
				p.landedFall = 0.0f;
			}
			if (driver.busy())
				driver.update(*session, in, down, dt);
			touching = false;
			if (first && s.tapX >= 0)
			{
				ui.tapped = true;
				ui.tapX = s.tapX;
				ui.tapY = s.tapY;
			}
			if (first && s.drag)
			{
				ui.touching = true;
				ui.touchX = 160;
				ui.touchY = 100;
				ui.dragDX = s.dragX;
				ui.dragDY = s.dragY;
			}
			inputTime += dt;
			if (first)
			{
				Player& p = session->w.player;
				logf("autoinput: step %d [%s] feet %.0f %.0f %.0f %s screen %d msgs %d ctrl %d target %s pending %d", (int)inputIndex,
					session->w.cellName().c_str(), p.feet[0], p.feet[1], p.feet[2],
					p.swimming ? "water" : p.onGround ? "ground" : "air", session->screen, (int)session->messages.size(),
					session->w.controlsEnabled, session->target >= 0 ? session->w.refs[session->target].id.c_str() : "-",
					(int)session->pendingMenus.size());
			}
			// (a flight takes as long as its distance needs: the step's seconds are a guess, FLYTO knows better)
			float stepSecs = s.secs;
			if (driver.busy() && driver.kind == TestDriver::FLY && driver.flyAllow > stepSecs)
				stepSecs = driver.flyAllow;
			if (driver.busy() && inputTime >= stepSecs)
				driver.fail("timeout");
			if (inputTime >= stepSecs || (stepBlocking && !driver.busy()))
			{
				stepBlocking = false;
				inputIndex++;
				inputTime = 0.0f;
				if (inputIndex == autoinput.size())
				{
					logf("autoinput: end feet %.0f %.0f %.0f screen %d", session->w.player.feet[0], session->w.player.feet[1],
						session->w.player.feet[2], session->screen);
					autoShotNow = true;
					session->save();
				}
			}
		}
		else if (touching)
		{
			ui.touching = true;
			ui.touchX = tp.px;
			ui.touchY = tp.py;
			if (touchWasDown)
			{
				ui.dragDX = tp.px - lastTouchX;
				ui.dragDY = tp.py - lastTouchY;
				if (abs(tp.px - pressX) + abs(tp.py - pressY) > kTapSlop)
					dragged = true;
			}
			else
			{
				dragged = false;
				pressX = tp.px;
				pressY = tp.py;
			}
			lastTouchX = tp.px;
			lastTouchY = tp.py;
		}
		else if (touchWasDown && !dragged)
		{
			ui.tapped = true;
			ui.tapX = lastTouchX;
			ui.tapY = lastTouchY;
		}
		touchWasDown = touching;
		ui.down = down;
		ui.held = held;
		ui.up = up;
		ui.stickY = inputIndex < autoinput.size() && loaded ? autoinput[inputIndex].stickY : readStickScroll();
		uiBeginFrame(ui);

		if (session->wantName)
		{
			if (session->autotest)
				session->setName("Tester");
			else
			{
				SwkbdState kb;
				char name[32] = {};
				swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, 24);
				swkbdSetHintText(&kb, "What is your name?");
				swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
				SwkbdButton button = swkbdInputText(&kb, name, sizeof(name));
				session->setName(button == SWKBD_BUTTON_CONFIRM ? name : "Stranger");
				lastTick = svcGetSystemTick();
			}
		}
		// The name of the spell / item being made, or the journal's search: the system keyboard, starting from the
		// text there now (a name can't be left blank; an empty search shows everything again)
		if (session->wantText)
		{
			if (session->autotest)
				session->setText(true, session->typedText);
			else
			{
				SwkbdState kb;
				char text[32] = {};
				bool search = session->wantText == Session::TEXT_SEARCH;
				swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, 31);
				swkbdSetHintText(&kb, search ? "Search" : session->screen == SCR_ENCHANT ? "Name the item" : "Name the spell");
				swkbdSetInitialText(&kb, session->textNow().c_str());
				if (!search)
					swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
				SwkbdButton button = swkbdInputText(&kb, text, sizeof(text));
				session->setText(button == SWKBD_BUTTON_CONFIRM, text);
				lastTick = svcGetSystemTick();
			}
		}
		// A movie: the game waits; A, B or START skips it
		if (loaded && session->moviePlaying())
		{
			if (down & (KEY_A | KEY_B | KEY_START))
				session->stopMovie();
			else
				session->movieUpdate(dt);
			audioUpdate();                    // (the movie's sound streams on the music channel: the game's update that feeds it isn't running)
			C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
			linearRetire();
			C2D_Prepare();
			C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
			C2D_TargetClear(top, C2D_Color32(0, 0, 0, 255));
			C2D_SceneBegin(top);
			session->movieDraw();
			C2D_TargetClear(bottom, C2D_Color32(0, 0, 0, 255));
			C2D_SceneBegin(bottom);
			uiTextCentered(160, 110, 0.45f, col::textDim, "A: skip");
			C2D_Flush();
			MARK("frame end");
			g_mainFrames++;
			C3D_FrameEnd(0);
			continue;
		}
		if (loaded)
		{
			if (down & KEY_START)
				session->toggleGameMenu();
			u64 updStart = svcGetSystemTick();
			MARK("session update");
			session->update(in, session->worldPaused() ? 0.0f : dt);      // (a menu stops the world; the test driver keeps its own clock)
			MARK("after update");
			extern float g_profWorldMs;
			g_profWorldMs += (svcGetSystemTick() - updStart) * 1000.0f / SYSCLOCK_ARM11;
		}

		// Death screen asked to load the last save: start the session over from it
		if (loaded && session->wantReload)
		{
			bool testGodKept = session->testGod, testGodBlowsKept = session->testGodBlows, testLegitKept = session->testLegit;
			drawLoading(top, bottom, "Loading...");
			std::string path = session->reloadPath.empty() ? kSavePath : session->reloadPath;
			logf("reload from %s, heap used %d KB", path.c_str(), mallinfo().uordblks / 1024);
			session->shutdown();
			logf("reload: after shutdown, heap used %d KB", mallinfo().uordblks / 1024);
			delete session;
			logf("reload: after the old session, heap used %d KB, %d zopen streams open", mallinfo().uordblks / 1024, zOpenCount());
			session = new Session();
			session->dataDir = dataDir;
			{
				LoadingProgress progress(top, bottom, "Loading...");
				loaded = session->start(dataDir, "", path.c_str());
				if (loaded)
				{
					session->vm.load(session->w);
					uiLoadTheme(dataDir, session->w.game.ui);
			s_splashes = session->w.game.art.splash;
				}
			}
			session->savePath = kSavePath;
			// a test reload: still a test (and still invincible if it was)
			session->autotest = !autoinput.empty();
			session->testGod = testGodKept;
			session->testGodBlows = testGodBlowsKept;
			session->testLegit = testLegitKept;
			lastTick = svcGetSystemTick();
			continue;
		}

		// A door to another cell: show the loading screen for a frame, then load
		if (loaded && session->travelCell >= 0)
		{
			std::string text = "Loading " + session->w.cells[session->travelCell].name + "...";
			drawLoading(top, bottom, text.c_str());
			LoadingProgress progress(top, bottom, text);
			session->finishTravel();
			lastTick = svcGetSystemTick();
			continue;
		}

		RenderCamera cam = { { session->w.player.feet[0], session->w.player.feet[1],
			playerEyeZ(session->w.player) }, session->w.player.yaw, session->w.player.pitch };
		if (loaded)
			session->viewCamera(cam);            // third person: behind the player
		bool autocamShot = false;
		if (poseIndex < autocam.size() && loaded)
		{
			const std::string& want = autocam[poseIndex].cell;
			int ci = want.empty() ? session->w.current : session->w.cellIndex(want);
			if (ci >= 0 && ci != session->w.current)
			{
				drawLoading(top, bottom, ("Loading " + want + "...").c_str());
				const LevelCell& lc = session->w.cells[ci];
				if (!lc.interior)
				{
					session->w.player.feet[0] = (lc.gx + 0.5f) * 8192.0f;
					session->w.player.feet[1] = (lc.gy + 0.5f) * 8192.0f;
				}
				session->w.enterCell(ci);
				lastTick = svcGetSystemTick();
				continue;
			}
			cam = autocam[poseIndex].cam;
			if (++poseFrames == 4)
				autocamShot = true;
		}

		u64 workEnd = svcGetSystemTick();       // the frame's update, before waiting for the GPU
		// The first frames after a load say how far they get (a GPU hang would stop in FrameBegin)
		static int traced = 0;
		bool trace = loaded && traced < 3;
		if (trace)
			logf("frame %d: begin", traced);
		MARK("frame begin");
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		linearRetire();                         // the GPU is done with what the update freed
		if (trace)
			logf("frame %d: GPU done with the previous frame", traced);
		if (autocamShot)
		{
			char path[64];
			snprintf(path, sizeof(path), "sdmc:/3ds/mw3ds/shot_%02d.bmp", (int)poseIndex);
			screenshotSave(path);
			poseIndex++;
			poseFrames = 0;
			if (poseIndex == autocam.size())
				logf("autocam done");
		}
		if ((autoShot && t >= 3.0f) || autoShotNow)
		{
			autoShot = false;
			logf("autoshot: %s", screenshotSave("sdmc:/3ds/mw3ds/shot.bmp") ? "saved" : "FAILED");
		}
		if (stepShot)
		{
			stepShot = false;
			char path[64];
			snprintf(path, sizeof(path), "sdmc:/3ds/mw3ds/shot_step_%02d.bmp", stepShots++);
			logf("stepshot: %s %s", path, screenshotSave(path) ? "saved" : "FAILED");
		}

		if (loaded)
			session->updateLocalMap();

		// Stereo 3D: eases in and out; the screen leaves 3D mode only once the depth is gone
		float slider = osGet3DSliderState();
		float target = loaded && !autoShotNow && poseIndex >= autocam.size() ? session->stereoTarget(slider, dt) : 0.0f;
		float& amount = session->stereoAmount;
		amount = target > amount ? fminf(target, amount + dt * 2.0f) : fmaxf(target, amount - dt * 2.0f);
		bool wantStereo = amount > 0.0f || target > 0.0f;
		if (wantStereo != stereoOn)
		{
			gfxSet3D(wantStereo);
			stereoOn = wantStereo;
		}
		// Half the eye separation in world units at full slider (a person's is ~4.5 units): depth
		// between near and far scales with it. The focus (renderer.cpp) sets what sits on the screen
		const float kMaxEyeShift = 3.0f;
		float shift = slider * kMaxEyeShift * amount;
		// Outdoors the fog closes in while frames run long, and with the weather
		float fogScale = 1.0f;
		if (loaded && !session->w.cells[session->w.current].interior)
			fogScale = session->viewScale * session->w.weatherFogScale();
		if (loaded)
			session->w.drawScale = fogScale;

		const u8* fog = loaded ? session->w.here().fog : (const u8*)"\0\0\0\xff";
		u32 clear = (fog[0] << 24) | (fog[1] << 16) | (fog[2] << 8) | 0xFF;
		if (loaded)
		{
			// Day and night outdoors: the light, sky and fog for this hour
			float land[3], sky[3], dayFog[3];
			session->w.daylight(land, sky, dayFog);
			bool outdoors = !session->w.cells[session->w.current].interior && session->w.game.hasWeather;
			// Night Eye (magnitude percent) and Light brighten what the player sees
			float boost = 1.0f + fminf(1.0f, session->w.effectTotal(43) / 100.0f) * 0.8f
				+ (session->w.effectTotal(41) > 0.0f ? 0.25f : 0.0f);
			if (boost > 1.0f)
			{
				for (int c = 0; c < 3; c++)
					land[c] = fminf(2.0f, land[c] * boost);
				outdoors = true;           // the tint applies indoors too while it lasts
			}
			// Under water: murky blue-green, close fog, dimmer light
			Cell& here = session->w.here();
			if (here.hasWater() && playerEyeZ(session->w.player) < here.waterZ)
			{
				static const float murk[3] = { 0.08f, 0.2f, 0.24f }, dim[3] = { 0.45f, 0.65f, 0.75f };
				for (int c = 0; c < 3; c++)
				{
					dayFog[c] = murk[c];
					land[c] *= dim[c];
				}
				outdoors = true;
				fogScale *= 0.15f;
			}
			rendererSetDaylight(land, sky, dayFog, outdoors);
			if (outdoors)
				clear = ((u32)(dayFog[0] * 255) << 24) | ((u32)(dayFog[1] * 255) << 16) | ((u32)(dayFog[2] * 255) << 8) | 0xFF;
		}
		float eyeCmd = 0.0f;
		float aimDepth = stereoOn && loaded ? session->aimDepth() : 0.0f;
		int hudDrawsLeft = 0;
		for (int eye = 0; eye < (stereoOn ? 2 : 1); eye++)
		{
			C3D_RenderTarget* target3d = eye ? topRight : top;
			C3D_RenderTargetClear(target3d, C3D_CLEAR_ALL, clear, 0);
			C3D_FrameDrawOn(target3d);
			float cmdBefore = C3D_GetCmdBufUsage();
			if (loaded)
				rendererDrawWorld(session->w, cam, stereoOn ? (eye ? shift : -shift) : 0.0f, eye == 1, fogScale);
			if (loaded && poseIndex >= autocam.size())
				session->drawExtras(cam.pos, eye == 1);
			if (eye == 0)
				eyeCmd = C3D_GetCmdBufUsage() - cmdBefore;

			C2D_Prepare();
			C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
			// The world's last draw leaves its blend / alpha test / culling behind: the UI needs
			// blending (glyphs are alpha-only), no alpha test, both faces
			C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA,
				GPU_ONE_MINUS_SRC_ALPHA);
			C3D_AlphaTest(false, GPU_ALWAYS, 0);
			C3D_CullFace(GPU_CULL_NONE);
			C2D_SceneBegin(target3d);
			// The HUD (subtitles, notes, crosshair) floats a little in front of the world: flat on the screen
			// surface, it sat behind what's nearer than the stereo focus (the speaker, the weapon) while
			// covering it, a depth clash that makes text hard to read. Left eye right, right eye left
			float hudShift = stereoOn ? slider * 3.0f : 0.0f;
			C2D_ViewReset();
			C2D_ViewTranslate(eye ? -hudShift : hudShift, 0.0f);
			// The crosshair and the label of what it's on sit at the depth of what it points at (drawn
			// flat, they doubled while the eyes looked at the target): its stereo offset, less the HUD's
			session->aimShift = 0.0f;
			session->hudEye = eye;
			session->hudEyes = stereoOn ? 2 : 1;
			if (stereoOn && loaded)
				session->aimShift = rendererStereoPixels(aimDepth, eye ? shift : -shift) - (eye ? -hudShift : hudShift);
			extern int g_uiDraws;
			g_uiDraws = 0;
			if (loaded)
				session->drawTop();
			else
				uiTextCentered(200, 110, 0.6f, col::header, "Load failed - see sdmc:/3ds/mw3ds/log.txt");
			// The same HUD in both eyes: a pass that drew a different number of things is a one-eye element
			if (loaded && stereoOn)
			{
				if (eye == 0)
					hudDrawsLeft = g_uiDraws;
				else if (g_uiDraws != hudDrawsLeft)
					session->noteHudMismatch(hudDrawsLeft, g_uiDraws);
			}
			// citro2d draws what it queued only when flushed, into whatever target is bound then: without this
			// the left eye's HUD went out after the right eye's target was bound, and both HUDs landed in one eye
			C2D_Flush();
			C2D_ViewReset();
		}
		C2D_TargetClear(bottom, C2D_Color32(10, 8, 6, 255));
		C2D_SceneBegin(bottom);
		if (loaded)
			session->drawBottom();
		cmdPeak = fmaxf(cmdPeak, C3D_GetCmdBufUsage());
		if (trace)
			logf("frame %d: built (%d%% of the command buffer), submitting", traced, (int)(C3D_GetCmdBufUsage() * 100.0f));
		MARK("frame end");
		g_mainFrames++;
		C3D_FrameEnd(0);
		if (trace)
			logf("frame %d: submitted", traced++);
		if (loaded)
		{
			session->noteRenderCost(C3D_GetProcessingTime(), C3D_GetDrawingTime(), stereoOn, eyeCmd);
			float updateMs = (float)(workEnd - now) * 1000.0f / SYSCLOCK_ARM11;
			if (poseIndex >= autocam.size() && inputIndex >= autoinput.size())
				session->adaptView(fmaxf(updateMs + C3D_GetProcessingTime(), C3D_GetDrawingTime()));
		}

		frames++;
		float statSecs = (float)(now - statTick) / SYSCLOCK_ARM11;
		if (statSecs >= 1.0f)
		{
			session->fps = frames / statSecs;
			session->cpuMs = C3D_GetProcessingTime();
			session->gpuMs = C3D_GetDrawingTime();
			extern float g_profActorsMs, g_profWorldMs;
			float talk[3] = {};
			extern int g_drawnBatches, g_culledBatches;
			(void)talk;
			logf("profile per frame: update %.2fms (world %.2f player %.2f stream %.2f ai %.2f scripts %.2f other %.2f) "
				"actors drawn %.2fms, batches %d drawn %d culled, 3D %s",
				g_profWorldMs / frames, g_profParts[PROF_WORLD] / frames, g_profParts[PROF_PLAYER] / frames,
				g_profParts[PROF_STREAM] / frames, g_profParts[PROF_AI] / frames, g_profParts[PROF_SCRIPTS] / frames,
				g_profParts[PROF_OTHER] / frames, g_profActorsMs / frames, g_drawnBatches, g_culledBatches,
				stereoOn ? "on" : "off");
			logf("view distance %d%%, frame work %.2fms", (int)(session->viewScale * 100.0f), session->workCost);
			g_profActorsMs = g_profWorldMs = 0.0f;
			for (auto& p : g_profParts)
				p = 0.0f;
			extern int g_skippedDraws;
			logf("fps %.1f cpu %.2fms gpu %.2fms linearFree %luKB heapUsed %dKB gpu commands peak %d%%%s",
				session->fps, session->cpuMs, session->gpuMs, linearSpaceFree() / 1024, mallinfo().uordblks / 1024,
				(int)(cmdPeak * 100.0f), g_skippedDraws ? " (buffer full: draws skipped)" : "");
			cmdPeak = 0.0f;
			g_skippedDraws = 0;
			uiCheckTheme();                   // the UI's textures still as loaded (a monitor)
			frames = 0;
			statTick = now;
		}
	}

	logf("exit");
	session->shutdown();
	delete session;
	audioExit();
	uiExit();
	rendererExit();
	C2D_Fini();
	C3D_Fini();
	if (romfs)
		romfsExit();
	irrstExit();
	logExit();
	gfxExit();
	return 0;
}
