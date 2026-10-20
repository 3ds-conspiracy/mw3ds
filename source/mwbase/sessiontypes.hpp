#pragma once

// What Session and the states it derives from (session.hpp) share: the screens, message boxes, the dialogue's
// words, the settings the screens and the rules read

#include <cstdlib>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "mwdialogue/dialoguemanagerimp.hpp"
#include "mwworld/player.hpp"
#include "mwworld/projectilemanager.hpp"
#include "mwrender/viewmodel.hpp"
#include "mwrender/npcanimation.hpp"
#include "mwrender/renderingmanager.hpp"
#include "mwgui/savegamedialog.hpp"
#include "components/interpreter/interpreter.hpp"
#include "mwgui/widgets.hpp"
#include "mwworld/worldimp.hpp"

// Update profile parts (Session::update; logged by main once a second)
enum { PROF_WORLD, PROF_PLAYER, PROF_STREAM, PROF_AI, PROF_SCRIPTS, PROF_OTHER, PROF_COUNT };
extern float g_profParts[PROF_COUNT];

#include "platform/console.hpp"
#include "mwgui/console.hpp"

enum { CONTROLS_XBOX = 0, CONTROLS_CLASSIC = 1, CONTROLS_GOLDENEYE = 2 };
extern int g_controlLayout;   // button layout (settings.txt controls=xbox|classic|goldeneye)
// Gyro aim (settings.txt gyroaim=on|off, gyrosens=50|100|200): turning the console turns the view, as far as it
// turned. The percent of that turn (100: one to one), 0 when off
extern int g_gyroAim;
// Look (Controls > Look; settings.txt lookspeed=, geturn=, inverty=on|off): how fast the C-stick, the D-pad and the
// GoldenEye pad while aiming look (one speed: they were two bars), how fast the GoldenEye pad turns while walking,
// percent (100: as before); look up and down the other way round. Gyro aim keeps its own speed and is never inverted
// (the console's own tilt)
extern int g_lookSpeed, g_geTurnSpeed;
extern bool g_invertY;
static const int kLookMinPct = 50, kLookMaxPct = 200, kLookStepPct = 10;
// A new player's look speed, and Look's Default: quicker than 1.00x, which felt slow on the console (1.20x still did).
// Scripted runs start at 1.00x whatever this is (Session's settings load), so the cases keep their timing
static const int kLookDefaultPct = 150;
// Subtitles (Options > Display; settings.txt subtitles=on|off): spoken lines written on the top screen. Off, a line
// with a voice is only heard; one with no voice file is still written (it would be lost otherwise)
extern bool g_subtitles;

enum Screen
{
	SCR_NONE, SCR_DIALOGUE, SCR_BOOK, SCR_CONTAINER, SCR_INVENTORY, SCR_JOURNAL, SCR_STATS,
	SCR_RACE, SCR_CLASS_METHOD, SCR_CLASS_LIST, SCR_CLASS_QUIZ, SCR_CLASS_RESULT, SCR_BIRTH,
	SCR_REVIEW, SCR_REST, SCR_END, SCR_BARTER, SCR_DEATH, SCR_TRAVEL, SCR_MAP, SCR_MAGIC, SCR_SPELLS,
	SCR_ARREST, SCR_TRAINING, SCR_SAVES, SCR_REPAIR, SCR_GAMEMENU, SCR_LEVELUP, SCR_PERSUADE, SCR_ALCHEMY, SCR_SPELLMAKE, SCR_ENCHANT, SCR_RECHARGE,
	SCR_OPTIONS,
	SCR_CONTROLS,
	SCR_DISPLAY,
	SCR_LOOK,
	SCR_CONSOLE,
	SCR_CLASS_CREATE,
	SCR_AUDIO,
};

struct MessageState
{
	std::string text;
	std::vector<std::string> buttons;
	bool fromScript;        // GetButtonPressed reports the choice
};

struct Word
{
	std::string text;
	float x, y, w;
	u32 color;
	int action;             // -1 none, >= 0 topic index into Session::linkTopics, <= -2 choice (-2 - index)
	C2D_Text glyphs;        // parsed once per layout into the dialogue text buffer
};
