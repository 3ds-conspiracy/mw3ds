#pragma once

#include <3ds.h>
#include <citro3d.h>
#include <string>
#include <vector>

#include "mwworld/player.hpp"
#include "mwrender/renderingmanager.hpp"

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
	int touchX = -1, touchY = -1;     // TOUCH:x:y: a finger held at x, y the whole step (slid there from the last step's)
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
	float dt = 0.0f;            // DT:s: the frame step of the scripted run from here (0.1: a long frame, as after a stall on the console)
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
	int sleepUntil = -1;              // SLEEPUNTIL:h: rest until game hour h comes round (the rest menu's hour picker)
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
	float stickX = 0.0f;              // STICKRIGHT / STICKLEFT: the C-stick pushed right (1) / left (-1), turning the race menu's face
	bool shot = false;                // SHOT: a screenshot (shot_step_NN.bmp) as this step starts
	bool chain = false;               // CHAIN: from here JOURNAL setup only raises (chained chapter runs)
	bool quit = false;                // QUIT (the last step): Save and quit from the game menu once the script ends, so the
	                                  // run leaves through the real exit path
	bool legit = false;               // LEGIT: from here only a player's own actions (TestDriver::playerToken)
	std::vector<std::string> actions; // GOD / WALKTO / KILL / ACTIVATE / PICKUP / EQUIP / EXPECT (testdrive/testdrive.hpp)
	std::string startScript;          // STARTSCRIPT:name: a global script an earlier chapter would have started
	bool saveLegacy = false;          // SAVEV1:name: the save in version 1 (every cell inside the JSON: old saves still load)
	std::string saveAs, loadFrom;     // SAVE:name / LOAD:name: sdmc:/3ds/mw3ds/test_<name>.sav (the sweeps
	                                  // start every quest from the same state)
	int layout = -1;                  // CONTROLS:classic|xbox|goldeneye: the button layout from this step on
	int gyroAim = -1;                 // GYROAIM:<percent>|off: gyro aim's sensitivity from this step on (0 off)
	float gyroYaw = 0.0f, gyroPitch = 0.0f;   // GYRO:yaw:pitch: the console turning (degrees a second) this step
	int skin = -1;                    // SKIN:gpu|cpu: how the actors of cells loaded from here on are skinned
	int frameCap = -1;                // FRAMECAP:on|off: the Old 3DS's 30 / 20 fps cap in a scripted run (on unless a case says off)
	int camSet = -1;                  // CAM:x:y:z:yaw:pitch (1) / CAM:off (0): the view held at that spot (degrees, pitch up positive)
	float cam[5] = {};
	int hud = -1;                     // HUD:off (0) / HUD:on (1): the top screen's HUD
	int halt = -1;                    // HALT:on (1) / HALT:off (0): the world stands still (no time passes) while the script goes on
};

// One step's line into s (false: not a step)
bool parseStep(const char* line, ScriptedInput& s);

// The scripted steps, kept as their lines and each parsed when it is reached (in order, as before). Parsed up front,
// an uber test's 3,000 steps held 3 MB of heap (forty strings each) that an Old 3DS run then didn't have for the game,
// and a test ran out of memory sooner than play would
class ScriptedSteps
{
public:
	std::vector<std::string> lines;
	size_t size() const { return lines.size(); }
	bool empty() const { return lines.empty(); }
	ScriptedInput& operator[](size_t i)
	{
		if (i != at_)
		{
			parseStep(lines[i].c_str(), cur_);
			at_ = i;
		}
		return cur_;
	}

private:
	size_t at_ = (size_t)-1;
	ScriptedInput cur_;
};

// sdmc:/3ds/mw3ds/autoinput.txt's steps (none without the file)
ScriptedSteps loadAutoinput();

struct CamPose
{
	RenderCamera cam;
	std::string cell;          // "" = stay in the current cell
};

// sdmc:/3ds/mw3ds/autocam.txt's poses ("spawn": where the game starts)
std::vector<CamPose> loadAutocam(const RenderCamera& spawn);

// What a scripted run's steps set for the frame loop: the frame cap, the controls' layout, the view held at a spot,
// the HUD hidden, the world standing still (parseKeys)
extern bool g_testFrameCap;
extern int g_testLayout;
extern bool g_camHeld, g_hudOff, g_halt;
extern RenderCamera g_heldCam;
