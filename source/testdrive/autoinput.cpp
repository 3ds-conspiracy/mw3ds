// The scripted runs' steps (testdrive/autoinput.hpp)
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
#include "components/debug/debuglog.hpp"
#include "testdrive/testdrive.hpp"
#include "mwbase/session.hpp"
#include "testdrive/autoinput.hpp"

// A scripted run keeps the Old 3DS frame cap (30 / 20 fps), as the console would run it. A case that only needs the story
// and not the pacing can say FRAMECAP:off: it steps on a fixed clock, so the cap only slows it to real time
bool g_testFrameCap = true;

// The layout the scripted run plays with (classic unless a CONTROLS: step chose another)
int g_testLayout = CONTROLS_CLASSIC;

// CAM / HUD:off / HALT:on: a scripted run's view held at a spot, the HUD hidden, the world standing still
// (side-by-side screenshots with OpenMW, tools/test/sidebyside.py). The player stays where they are, out of the picture
bool g_camHeld = false, g_hudOff = false, g_halt = false;
RenderCamera g_heldCam;

// Set by the first LEGIT step: every later token must be a player's own action (uber quest tests)
static bool s_legitParsed = false;

static u32 parseKeys(const char* s, int* tapX, int* tapY, ScriptedInput* step)
{
	static const struct { const char* name; u32 key; } names[] = {
		{ "A", KEY_A }, { "B", KEY_B }, { "X", KEY_X }, { "Y", KEY_Y }, { "L", KEY_L }, { "R", KEY_R },
		{ "UP", KEY_DUP }, { "DOWN", KEY_DDOWN }, { "LEFT", KEY_DLEFT }, { "RIGHT", KEY_DRIGHT },
		{ "SELECT", KEY_SELECT }, { "START", KEY_START }, { "ZL", KEY_ZL }, { "ZR", KEY_ZR },
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
		if (sscanf(tok, "TOUCH:%d:%d", &step->touchX, &step->touchY) == 2)
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
		if (sscanf(tok, "GYRO:%f:%f", &step->gyroYaw, &step->gyroPitch) >= 1)
			continue;
		if (strncmp(tok, "GYROAIM:", 8) == 0)
		{
			step->gyroAim = strcmp(tok + 8, "off") == 0 ? 0 : atoi(tok + 8);
			continue;
		}
		if (sscanf(tok, "CAM:%f:%f:%f:%f:%f", &step->cam[0], &step->cam[1], &step->cam[2], &step->cam[3], &step->cam[4]) == 5)
		{
			step->camSet = 1;
			continue;
		}
		if (strcmp(tok, "CAM:off") == 0)
		{
			step->camSet = 0;
			continue;
		}
		if (strncmp(tok, "HUD:", 4) == 0)
		{
			step->hud = strcmp(tok + 4, "off") == 0 ? 0 : 1;
			continue;
		}
		if (strncmp(tok, "HALT:", 5) == 0)
		{
			step->halt = strcmp(tok + 5, "on") == 0 ? 1 : 0;
			continue;
		}
		if (strncmp(tok, "FRAMECAP:", 9) == 0)
		{
			step->frameCap = strcmp(tok + 9, "on") == 0 ? 1 : 0;
			continue;
		}
		if (strncmp(tok, "SKIN:", 5) == 0)
		{
			step->skin = strcmp(tok + 5, "cpu") == 0 ? 0 : 1;
			continue;
		}
		if (strncmp(tok, "CONTROLS:", 9) == 0)
		{
			step->layout = strcmp(tok + 9, "goldeneye") == 0 ? CONTROLS_GOLDENEYE : strcmp(tok + 9, "xbox") == 0 ? CONTROLS_XBOX
				: CONTROLS_CLASSIC;
			continue;
		}
		if (strcmp(tok, "JAIL") == 0)
			step->jail = true;
		if (strcmp(tok, "SHOT") == 0)
			step->shot = true;
		if (strcmp(tok, "STICKDOWN") == 0)
			step->stickY = 1.0f;
		if (strcmp(tok, "STICKUP") == 0)
			step->stickY = -1.0f;
		if (strcmp(tok, "STICKRIGHT") == 0)
			step->stickX = 1.0f;
		if (strcmp(tok, "STICKLEFT") == 0)
			step->stickX = -1.0f;
		if (strcmp(tok, "CHAIN") == 0)
			step->chain = true;
		if (strcmp(tok, "QUIT") == 0)
			step->quit = true;
		if (strncmp(tok, "SAVE:", 5) == 0)
			step->saveAs = tok + 5;
		if (strncmp(tok, "SAVEV1:", 7) == 0)
		{
			step->saveAs = tok + 7;
			step->saveLegacy = true;
		}
		if (strncmp(tok, "STARTSCRIPT:", 12) == 0)
			step->startScript = tok + 12;
		if (strncmp(tok, "LOAD:", 5) == 0)
			step->loadFrom = tok + 5;
		for (const char* verb : { "GOD", "EXPECT:", "WALKTO:", "FLYTO:", "HOPTO:", "ESCORT:", "KILL:", "ACTIVATE:", "PICKUP:", "EQUIP:", "DOORTO:", "LOOT:", "PUT:", "STRIKE:",
				"SNAP:", "CLASS:", "SETSKILL:", "SETATTR:", "SKILLPROG:", "LEVELPROG:", "ENCHANTAT:", "ENCHITEM:", "ENCHGEM:",
				"ENCHTYPE:", "ADDEFFECT:", "CONFIRM", "SPELLMAKE:", "TRAIN:", "BUY:", "SELL:", "LEVELUP:", "READ:", "FACE:",
				"CASTAT:", "POSE:", "BONES:", "SLAY:", "HOSTILE:", "SAYTOPIC:", "TRUNCSAVE:", "REFHEALTH:", "REFADDITEM:", "VMPOSE:", "BODYPOSE:", "SCREEN:", "MAKESPELL:", "USEMADE", "EQUIPMADE", "RECHARGEMADE", "ATTRUPS:", "FILL", "DRINKBREWED", "CASTMADESPELL", "PROBE:", "ACTIVE:", "SETFATIGUE:", "SETMAGICKA:", "HIDEACTORS:", "VFX:", "PLACEAT:", "SPRITEBUDGET:", "NPCCAST:", "POV:", "SETREP:", "SETDISP:", "KNOW:", "SETBOUNTY:", "JOURNALADD:", "SETJOURNALINDEX:", "ADVANCE:", "ENABLE:", "DISABLE:", "ALARM:", "ROLL:", "RESISTROLL:", "REPAIRAT:", "REPAIRWITH:", "BUYSPELL:", "MOVIE:", "BARTER:", "BARTERSEL:", "TRAVEL:", "SAVESEL:", "SETWEATHER:", "CHANGEWEATHER:", "MODREGION:", "SETITEM:", "SETHEALTH:", "SETALARM:", "SETITEMHEALTH:", "SETITEMCHARGE:", "SETDEAD:", "SETTALKED:", "SETRACE:", "KNOCKDOWN:", "SNEAK", "WEREWOLF", "CLOTHVALUE:", "FATIGUEREGEN:", "GIVEPOTION:", "USE:", "USELOCKPICK:", "USEPROBE:", "SEED:", "PCNAME:", "PCRACE:", "PCSEX:", "TYPE:", "TOPICLOG:", "NOTIFY:", "MSGBOX:", "MEMHOLD:", "MEMLEAVE:", "UNSEEN", "ENHANCEV2", "MAKEITEM:", "UNTIL:", "FLOAT:", "HIDE:", "STREAM:", "GAMMA:", "AUTOGAMMA:", "LUX:", "CAMLUMA:", "LOOKSPEED:", "GETURN:", "GEAIM:", "INVERTY:", "SUBTITLES:", "SAYLINE:", "CON:", "CONSEL", "MAPCELL:", "MAPSEEN:", "CLOCKSHIFT:", "VIEWSCALE:", "ADAPTVIEW:", "RESTMENU" })
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
		if (sscanf(tok, "SLEEPUNTIL:%d", &step->sleepUntil) == 1)
			continue;
		if (sscanf(tok, "SLEEP:%d:%d", &step->sleep, &step->sleepForce) >= 1)
			continue;
		if (sscanf(tok, "BOOST:%d", &step->boost) == 1)
			continue;
		if (sscanf(tok, "DT:%f", &step->dt) == 1)
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

// One step's line: false if it isn't a step (fewer than the four numbers)
bool parseStep(const char* line, ScriptedInput& s)
{
	s = ScriptedInput();
	s.tapX = s.tapY = -1;
	s.layout = s.gyroAim = s.skin = s.frameCap = -1;
	char keys[1024] = {};
	int n = sscanf(line, "%f %f %f %f %d %1023s", &s.secs, &s.moveX, &s.moveY, &s.lookX, &s.jump, keys);
	if (n < 4)
		return false;
	if (n >= 6)
		s.keys = parseKeys(keys, &s.tapX, &s.tapY, &s);
	return true;
}

ScriptedSteps loadAutoinput()
{
	ScriptedSteps steps;
	FILE* f = fopen("sdmc:/3ds/mw3ds/autoinput.txt", "r");
	if (!f)
		return steps;
	char line[1024];
	float secs, mx, my, lx;
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "%f %f %f %f", &secs, &mx, &my, &lx) == 4)
		{
			size_t n = strlen(line);
			while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
				line[--n] = 0;
			steps.lines.push_back(line);
		}
	fclose(f);
	steps.lines.shrink_to_fit();
	logf("autoinput: %d steps", (int)steps.size());
	return steps;
}

std::vector<CamPose> loadAutocam(const RenderCamera& spawn)
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
