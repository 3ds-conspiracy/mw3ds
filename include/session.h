#pragma once

#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "dialogue.h"
#include "player.h"
#include "projectile.h"
#include "viewmodel.h"
#include "playerbody.h"
#include "renderer.h"
#include "saves.h"
#include "script.h"
#include "ui.h"
#include "world.h"

// Update profile parts (Session::update; logged by main once a second)
enum { PROF_WORLD, PROF_PLAYER, PROF_STREAM, PROF_AI, PROF_SCRIPTS, PROF_OTHER, PROF_COUNT };
extern float g_profParts[PROF_COUNT];

enum { CONTROLS_XBOX = 0, CONTROLS_CLASSIC = 1 };
extern int g_controlLayout;   // button layout (settings.txt controls=xbox|classic)

enum Screen
{
	SCR_NONE, SCR_DIALOGUE, SCR_BOOK, SCR_CONTAINER, SCR_INVENTORY, SCR_JOURNAL, SCR_STATS,
	SCR_RACE, SCR_CLASS_METHOD, SCR_CLASS_LIST, SCR_CLASS_QUIZ, SCR_CLASS_RESULT, SCR_BIRTH,
	SCR_REVIEW, SCR_REST, SCR_END, SCR_BARTER, SCR_DEATH, SCR_TRAVEL, SCR_MAP, SCR_MAGIC, SCR_SPELLS,
	SCR_ARREST, SCR_TRAINING, SCR_SAVES, SCR_REPAIR, SCR_GAMEMENU, SCR_LEVELUP, SCR_PERSUADE, SCR_ALCHEMY, SCR_SPELLMAKE, SCR_ENCHANT, SCR_RECHARGE,
	SCR_OPTIONS,
	SCR_CONTROLS,
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

// The running game: world state, dialogue, menus, and everything scripts call into.
struct Session : ScriptHost
{
	Session()
	{
		w.awarenessHook = [this](int ref) { return awarenessCheck(ref); };
		w.shouldAttackHook = [this](int ref) {
			return w.refs[ref].ai == AI_COMBAT || fightTermOf(ref, w.distanceToPlayer(ref)) >= 100.0f;
		};
	}
	World w;
	Dialogue dlg;
	Screen screen = SCR_NONE;
	std::vector<MessageState> messages;
	int buttonPressed = -1;
	std::vector<ScriptMenu> pendingMenus;
	bool wantName = false;            // main loop shows the system keyboard between frames
	int travelCell = -1;              // door to another cell: main loop shows a loading screen, then finishTravel()
	float travelPos[3] = {}, travelYaw = 0.0f;
	bool travelToSpawn = false;   // test GOTO into an exterior cell: land at the cell's own spawn spot
	float boundsNoteUntil = 0.0f;
	int waterChannel = -1;            // looping shore ambience in cells with water

	// combat (combat.cpp)
	float attackCharge = -1.0f;       // 0..1 while X is held, -1 idle
	int swingCount = 0, castCount = 0;   // blows let go and spells cast (the tests count them)
	bool castPending = false;         // a spell's animation is playing: it lands at the animation's release mark
	SpellDef castDef;                 // that spell, as chosen when the cast began
	int castSchool = 5;
	float castLock = 0.0f;            // the cast animation's whole group plays out (one upper-body state): nothing else starts
	float castWait = 0.0f;            // no new cast for this long (when there's no animation to wait for)
	int enemyRef = -1;                // last NPC hit: its health bar shows on the top screen
	float enemyUntil = 0.0f;
	float hurtFlash = 0.0f;
	bool playerDead = false;
	bool weaponDrawn = false;         // weapon (or fists) out: the view model shows it
	float drawTimer = 0.0f;           // > 0 while the weapon comes out; the wind-up waits for it
	float sheatheTimer = 0.0f;        // seconds without fighting; the weapon goes away after a while
	int attackKind = 0;               // 0 chop, 1 slash, 2 thrust: picked when the wind-up starts
	float blockUntil = 0.0f;          // shield raised (view model)
	float detectTimer = 0.0f, sneakSkillTimer = 0.0f;
	bool hidden = false;              // sneaking and no one nearby has noticed
	std::vector<Projectile> projectiles;
	ViewModel vm;
	// Third person (SELECT): the player's body drawn, the camera behind them; also character creation's
	// face preview (previewFace)
	PlayerBody body;
	bool thirdPerson = false, previewFace = false;
	float thirdDistance = 0.0f;               // eased camera distance (walls pull it in)
	void viewCamera(RenderCamera& cam);
	// repairing: -1 = with a repair tool (inventory index repairTool), else the smith's ref
	int repairRef = -1, repairTool = -1;
	bool wantReload = false;          // death / Saves screen: main loop restarts the session from a save
	std::string reloadPath;           // which save (empty = the autosave)
	std::vector<SaveInfo> saveList;   // Saves screen
	static const int kBountyAssault = 40, kBountyMurder = 1000;   // iCrimeAttack, iCrimeKilling (defaults: Session::crimeBounty reads the GMSTs)
	// bartering
	int barterRef = -1;
	bool barterSell = false;
	// map
	C3D_Tex mapTex;                   // world map (stays loaded: the HUD shows it)
	bool mapLoaded = false, mapFailed = false;
	// The map screen: zoom step (kMapZooms), the view's centre in map pixels, panned by D-pad or finger
	static constexpr int kMapZoomSteps = 5;
	int mapZoomIdx = 0;
	float mapCX = 0.0f, mapCY = 0.0f;
	bool mapViewSet = false;          // the view has been centred on the player since the screen opened
	float frameDt = 1.0f / 30.0f;     // the last frame's time (D-pad panning)
	// The 2x-detail map's tiles: read only while the map screen shows the zoomed-in view (up to 2 stay
	// when out of view, 4 at most, 512 KB each), freed when it closes
	struct MapTileTex { C3D_Tex tex; bool loaded = false, failed = false; };
	MapTileTex mapTiles[4];
	bool ensureMapTile(int i);
	void freeMapTiles();
	// Local map of the interior: rendered from above on arrival (renderer texture), again when the
	// player changes floor
	int localMapCell = -1;
	float localMin[2] = {}, localSize = 1.0f, localTopZ = 0.0f;
	void updateLocalMap();            // inside a frame, before the screens are drawn
	bool ensureMapTexture();
	void drawMapView(float x, float y, float vw, float vh, float zoom, bool full);
	bool ended = false;
	bool autotest = false;
	bool testGod = false;
	int persuadeRoll = -1;        // tests: the die roll a persuasion or an enchanting uses (-1: random)
	// Monitors (logged "monitor: ...", the harness fails a run on them): someone playing a walk / run cycle
	// without getting anywhere (the female skeleton bug, dialogue without Idle)
	void monitorActors(float dt);
	void monitorCellChanged();
	void noteHudMismatch(int left, int right);   // the HUD drew differently in the two eyes
	struct Watch
	{
		float x = 0, y = 0, still = 0; bool told = false;
		float off = 0; bool toldFloor = false;                       // standing off the floor under them
		float anchorX = 0, anchorY = 0, moveT = 0; bool toldStuck = false;   // going somewhere, getting nowhere
	};
	// Engine-side checks (monitor: lines): the player's fall, doors, memory per cell
	float playerAirT = 0.0f;
	bool toldFall = false;
	int monitorCell = -1;
	std::map<std::string, std::vector<unsigned long>> cellMemory;
	std::unordered_map<int, Watch> watches;
	float monitorTimer = 0.0f;           // harness GOD: the player can't die, every blow lands and kills

	// menu state
	UiList list, list2;
	UiGrid grid;                      // item icons (inventory, containers, barter)
	int invTab = 0;                   // inventory filter: all, weapon, apparel, magic, misc
	int journalTab = 0;               // journal: 0 the entries, 1 the topics index
	std::string journalTopic;         // the topic open in the index
	UiScroll scroll;
	int focus = 0;
	int bookRef = -1, containerRef = -1;
	bool pickpocketing = false;       // the container screen shows a living NPC's pockets (sneaking)
	bool pickpocketCaught(float valueTerm);       // Morrowind's pickpocket roll: true = noticed
	int pickpocketBar(int victim, float valueTerm);   // the roll above is noticed when it is over this (0..75)
	std::string bookItem;
	int quizIndex = 0, quizCounts[3] = {};
	std::string pickedClass;

	// world interaction
	int target = -1;
	struct Note { std::string text; float until; };
	std::vector<Note> notes;
	float aimShift = 0.0f;
	int hudEye = 0;                   // the HUD pass drawing now (0 left / mono, 1 right)
	int hudEyes = 1;                  // passes this frame (2 in 3D)           // this eye's offset of the crosshair / target label (main.cpp, 3D)
	float aimDepth();                 // how far away what the crosshair points at is
	std::string subtitle;
	float subtitleUntil = 0.0f;
	std::unordered_map<int, float> sayUntil;
	float stepTimer = 0.0f;
	bool stepLeft = false;
	float fade = 0.0f;
	float listener[4] = {};           // x, y, z, yaw of the camera for 3D sound
	int uiVoiceChannel = -1;          // voice lines not tied to an actor (class questions)
	float fps = 0.0f, cpuMs = 0.0f, gpuMs = 0.0f;

	// dialogue layout
	std::vector<Word> words;
	std::vector<std::string> linkTopics;
	float wordsHeight = 0.0f, dlgScroll = 0.0f;
	int wordsRevision = -1;

	// savePath: continue from that save when it exists (and no startCell is given)
	bool start(const char* dataDir, const std::string& startCell = "", const char* savePath = nullptr);
	bool save();
	void shutdown();                  // frees the cell and stops its sounds (before deleting the session)
	const char* savePath = nullptr;
	void skipChargen();
	void giveItems(const std::vector<std::pair<int, std::string>>& items);
	void updateAmbience();
	// Sounds that loop on an object (PlayLoopSound3D), mixed by distance each frame, until stopped or left
	struct LoopSound { int ref; std::string id; int channel; float volume, pitch; };
	std::vector<LoopSound> loopSounds;
	struct ScriptSound { int ref; std::string id; float until; };   // a sound a script played (GetSoundPlaying)
	std::vector<ScriptSound> scriptSounds;
	void updateLoopSounds();
	void stopLoopSounds();
	// Weather outdoors: its ambient loop (rain, ash storm), thunder, and rain / ash drawn over the view
	int weatherChannel = -1;
	std::string weatherSound;
	float thunderIn = 10.0f;
	void updateWeatherEffects(float dt);
	void updateSky();
	float ambientTimer = 3.0f;                     // until the region's next ambient sound
	// Spell visuals: a static's piece at a spot for a moment, turning (cast at the hand, hit on the target)
	struct Vfx { std::string piece; float pos[3]; float age, life, spin; };
	std::vector<Vfx> vfx;
	void spawnVfx(const std::string& staticId, const float pos[3], float life = 1.2f);
	void spellVfx(const SpellDef& sp, int kind, const float pos[3]);   // kind 0 cast, 1 hit, 2 area                              // the sun, the moons and the stars for the renderer
	C3D_Tex* skyTex(const std::string& name);      // acquired once, kept
	std::unordered_map<std::string, C3D_Tex*> skyTextures;
	int restInterruptAt = -1;                    // sleeping outdoors: the hour (restDone) a creature comes
	std::string restInterruptList;
	void startRest(int hours);
	void drawPrecipitation();
	float fadeTarget = 0.0f, fadeRate = 2.5f;     // script fades (FadeOut / FadeIn)

	// combat.cpp
	void combatUpdate(const PlayerInput& in, float dt, bool menu);
	float playerSwing(float charge, const PlayerInput& in);   // the blow's strength for its follow-through (0: a miss)
	void npcCombat(int ref, float dt);
	void npcStrike(int ref);
	void npcWander(int ref, float dt);
	bool npcTalkStand(int ref, float dt);
	void npcPackage(int ref, float dt);          // travel / follow / escort / activate packages
	void allyFight(int ref, float dt);            // summoned creatures go for whoever fights the player
	bool npcStep(int ref, Cell& cell, float dirX, float dirY, float step);
	void makeHostile(int ref);
	void damageNpc(int ref, float dmg, bool fatigueOnly);
	void actorGravity(float dt);
	bool npcSayTopic(int ref, const char* voiceTopic);   // a voiced line of Idle / Attack / Hit / Flee / Thief                  // people with nothing under them fall (and land hard)
	void damagePlayer(float dmg, bool fatigueOnly);
	void killNpc(int ref, bool byPlayer = true);   // byPlayer: murder when a peaceful NPC
	const Object* playerWeapon();
	InventoryItem* playerWeaponItem();
	const Object* playerAmmo();           // arrows / bolts matching the bow, or the thrown weapon itself
	float playerArmor();
	int itemCondition(const InventoryItem& it);
	void wearItem(InventoryItem* it, float amount);
	void playerHitsNpc(int target, float damage, int skill, bool fatigueOnly, bool ranged);
	void npcHitsPlayer(int attacker, float damage, bool fatigueOnly, int skill, bool unarmed = false, bool ranged = false);
	bool shieldBlocks(float blockSkill, float agility, float luck, float fatigue, float fatigueMax, bool still,
		float attackSkill, float attackAgility, float attackLuck, float attackFatigue, float attackFatigueMax, float charge);
	bool npcAware(int ref);
	bool awarenessCheck(int ref);                  // OpenMW's awarenessCheck of the player by this observer (cached roll)
	float fatigueTermOf(float fatigue, float fatigueMax);
	float attackTermOf(int skill, int agility, int luck, float fatigue, float fatigueMax, float fortifyAttack, float blind);
	float playerDefense(bool defenseless);         // the player's evasion + Chameleon / Invisibility terms
	float npcDefense(int ref, bool defenseless);   // an actor's evasion (0 when unaware, knocked down or paralyzed)
	float blockChance(float blockSkill, float agility, float luck, float fatigue, float fatigueMax, bool still,
		float attackSkill, float attackAgility, float attackLuck, float attackFatigue, float attackFatigueMax, float swing);
	float knockdownOdds(int agility);
	float fallDamage(float height, int acrobatics, float jump);
	float actorWalkSpeed(int ref);
	float normalWeaponResist(int ref);
	float fleeRatingOf(int ref, float distance);
	float fightTermOf(int ref, float distance);
	float haggleChance(int merchantRef, int price, int offer, bool selling, int* dOut = nullptr);
	float walkSpeedFor(int speed);
	float runSpeedFor(int speed, int athletics);
	float jumpSpeedFor(int acrobatics, float jump, float load, bool running, float fatigueTerm);
	float actorRunSpeed(int ref);              // the roll (0..99) must be at least this
	float elementalShieldDamage(float magnitude, int destruction, int willpower, int luck, float fatigue, float fatigueMax,
		float resistance, int roll);
	void catchDiseases(int carrier);              // the player touched a diseased actor (a blow either way)
	float testFatiguePin = -1.0f;                // SETFATIGUE holds fatigue here (no regeneration) until FILL; -1: free
	bool testNoFatigueRegen = false;             // FATIGUEREGEN:0: fatigue doesn't come back by itself (a test reads the effects alone)
	float testCastTaken = 0.0f;                  // the magicka the last cast took (EXPECT:spellmagicka)
	float playerMoveForward = 0.0f;               // this frame's forward input (blocking: fBlockStillBonus when <= 0)
	void updateDetection(float dt);
	void knockDown(int ref, bool out);
	bool npcMoveTo(int ref, const float target[3], float speed, float dt, bool faceMove);
	void npcCastSpell(int ref);
	void npcFire(int ref);
	void playerFire(float charge);
	// projectile.cpp
	void fireProjectile(int owner, const float from[3], const float dir[3], float speed, const std::string& item,
		const std::string& weapon, float charge, const std::string& spell);
	void updateProjectiles(float dt);
	void projectileHits(Projectile& p, int victim);
	void drawExtras(const float eye[3], bool secondEye = false);

	// Stereo 3D (main.cpp draws each eye): on with the slider in interiors, and outdoors while the
	// frame has room for a second eye; Settings on the Stats screen can force it on or off
	// 3D is the player's call (the menu toggle and the slider): no frame-rate cut-off
	enum { STEREO_AUTO, STEREO_ON, STEREO_OFF };   // AUTO: old settings files, read as ON
	int stereoMode = STEREO_ON;
	// Combat rules (Options): vanilla, or Glancing Blows and Crits (GBAC 1.2.1 by ZullleMW, Nexus 49541):
	// misses become glancing blows, crits come with a high hit chance, the same damage on average
	enum { COMBAT_VANILLA = 0, COMBAT_GBAC = 1 };
	int combatMode = COMBAT_VANILLA;
	struct BlowRoll { bool lands = true; int kind = 1; float damage = 1.0f, skill = 1.0f; };   // kind 0 glance 1 hit 2 crit
	BlowRoll rollBlow(float chance, bool ranged, bool vanilla);
	// Hit & Miss Indicators (MorleyDev, Nexus 55396; Options): floating text by the crosshair for the player's
	// blows: "Miss (NN%)" white, health damage red, fist (fatigue) damage green; rises and fades in 1 s
	bool hitIndicators = false;
	struct Indicator { std::string text; u32 rgb; float x, y, timer; };
	std::vector<Indicator> indicators;
	void addIndicator(const std::string& text, u32 rgb);
	void drawIndicators(float dt);
	void blowSound(int ref, const BlowRoll& b);
	float stereoAmount = 0.0f;        // eases 0..1 so 3D never snaps on or off
	float renderCost = 0.0f;          // smoothed per-frame render time (ms) of one eye
	float eyeCmdUse = 0.0f;           // share of the GPU command buffer one eye's world takes
	bool stereoCmdRoom = true;
	// Adaptive view distance outdoors (fog scale), from the frame's CPU / GPU work
	static constexpr float kMinViewScale = 0.6f;
	float viewScale = 1.0f, workCost = 0.0f;
	void adaptView(float workMs);
	float stereoTarget(float slider, float dt);
	void noteRenderCost(float cpuMs, float gpuMs, bool stereo, float eyeCmd);
	void loadSettings();
	// Options (settings.txt): difficulty -100..100 (Morrowind's slider), effects / music volume 0..100
	int difficulty = 0, effectsVolume = 100, musicVolume = 100;
	float difficultyScale(bool toPlayer) const;   // damage multiplier: to the player / dealt by the player
	void drawOptions();
	void drawControls();
	void saveSettings();
	void viewModelUpdate(const PlayerInput& in, float dt);
	// repair (screens.cpp)
	void drawRepair();
	void openRepair(int smith, int tool);        // smith: the NPC (-1: the player's own tool, an inventory index)
	int repairPrice(int ref, const InventoryItem& it);
	bool isMerchant(int ref);
	bool merchantTrades(int ref, const Object* o);
	int barterPrice(int ref, int value, bool buying);
	void update(const PlayerInput& in, float dt);
	void drawTop();
	// Tooltip: what a menu has selected, in full on the top screen (as Morrowind's hover box);
	// the screens fill it each frame, drawTop shows and clears it
	std::vector<std::pair<std::string, u32>> tip;
	void itemTip(const Object* o, const InventoryItem* it);   // it: that one's wear and charge (nullptr: new)
	void spellTip(const SpellDef& sp);
	std::string effectLine(const SpellEffect& e, bool showRange);   // "Fire Damage 5 to 10 pts for 5 secs on Target"
	void drawBottom();
	bool menuOpen() const { return !messages.empty() || screen != SCR_NONE || dlg.open; }
	bool worldPaused() const;      // a menu that stops the world (not talk, barter and the services: the NPC keeps moving)
	void setName(const std::string& name);
	void finishTravel();

	// ScriptHost
	bool menuMode() override { return menuOpen(); }
	void messageBox(const std::string& text, const std::vector<std::string>& buttons) override;
	int takeButtonPressed() override;
	void say(int ref, const std::string& file, const std::string& text) override;
	bool sayDone(int ref) override;
	void playSound(int ref, const std::string& soundId, float volume = 1.0f, float pitch = 1.0f, bool script = false) override;
	void loopSound(int ref, const std::string& soundId, bool start, float volume = 1.0f, float pitch = 1.0f) override;
	void streamMusic(const std::string& file) override;
	bool soundPlaying(int ref, const std::string& soundId) override;
	// Movies (tools/convert/movies.py: data/movies/<name>.mwv, sound music/movie_<name>.snd): the game waits
	bool playMovie(const std::string& name) override;
	bool talking() override { return dlg.open; }
	bool moviePlaying() const { return movieFile != nullptr; }
	void movieUpdate(float dt);                   // next frame when it's time; the end
	void movieDraw();                             // on the top screen (main.cpp)
	void stopMovie();
	FILE* movieFile = nullptr;
	u32 movieFrames = 0, movieFps100 = 1000, movieW = 0, movieH = 0;
	int movieFrame = -1;
	float movieTime = 0.0f;
	C3D_Tex movieTex;
	bool movieTexOk = false;
	void equipPlayerItem(const std::string& id) override;
	void fadeTo(float target, float seconds) override { fadeTarget = target; fadeRate = seconds > 0.01f ? 1.0f / seconds : 100.0f; }
	void openMenu(ScriptMenu menu) override;
	bool bedRefused(int bed) override;
	void activate(int ref) override;
	void forceGreeting(int ref) override;
	void dialogueChoice(const std::vector<std::pair<std::string, int>>& choices) override;
	void dialogueGoodbye() override;
	void notify(const std::string& text) override;
	void startCombat(int ref) override { makeHostile(ref); }
	void stopCombat(int ref) override;
	bool talkingTo(int ref) override { return dlg.open && dlg.ref == ref; }
	void killActor(int ref) override { if (!w.refs[ref].dead) killNpc(ref, false); }   // scripts (SetHealth 0)
	void knockDownActor(int ref) override { if (!w.refs[ref].dead) knockDown(ref, true); }   // scripts (fatigue to 0)
	void hurtPlayer(float health) override { damagePlayer(health, false); }
	void sheathe() override { sheatheTimer = 99.0f; }
	void goToJail() override;
	void teleportPlayer(int cell, const float pos[3], float yaw) override
	{
		if (cell == w.current || (!w.cells[cell].interior && !w.cells[w.current].interior))
		{
			// Same space: just stand there
			memcpy(w.player.feet, pos, sizeof(w.player.feet));
			w.player.yaw = yaw;
			w.player.vz = 0.0f;
			w.streamExterior(true);
			return;
		}
		travelCell = cell;
		memcpy(travelPos, pos, sizeof(travelPos));
		travelYaw = yaw;
	}
	void castEffect(int ref, const SpellEffect& e, const std::string& source, int casterRef = -1, float spellCost = 0.0f) override
	{
		if (ref < 0)
			applyEffectToPlayer(e, source, casterRef, spellCost);
		else if (w.active(ref))
			applyEffectToActor(ref, e, false, spellCost, 100.0f, source);
	}

	// internals
	void playerActivate(int ref);
	void pickUp(int ref);
	void containerTake(int index, bool checkOwner);   // the open container's stack index into the inventory
	bool containerPut(int inventoryIndex);            // an inventory stack into the open container (false: refused, a message says why)
	bool containerPutting = false;                    // the container screen shows the player's items (to put in)
	void useDoor(int ref);
	// A door's or container's lock and trap, in OpenMW's order: 0 stays shut (locked, no key), 1 its trap went off,
	// 2 free to open
	int lockGate(int ref, const char* lockedSound);
	bool werewolfRefused();                       // a werewolf is refused by everything but doors: the message
	bool readRefused();                           // a book in the pack can't be read in a fight: the message
	// Beast races and broken pieces: OpenMW's canBeEquipped: 0 refused (message in why), 1 ok, 2 a two-hander, 3 a shield with one
	int canEquip(const InventoryItem& it, std::string* why);
	bool testSneak = false;                       // SNEAK:1: the player sneaks (tests)
	bool werewolf = false;                        // WEREWOLF:1: the player is a werewolf (refusals only: no other change of form)
	void openScreen(Screen s);
	void closeScreen();
	void mix3d(const float* pos, float& volume, float& pan, float minDist, float maxDist);
	void greetings(float dt);
	std::string targetLabel(int ref);

	// screens (screens.cpp)
	void drawHud();
	void drawQuickStrip();
	void drawMessage();
	void drawDialogue();
	void layoutDialogue(float width);
	void drawBook();
	void drawContainer();
	void drawInventory();
	void drawJournal();
	void drawStats();
	void drawRace();
	void drawClassMethod();
	void drawClassList();
	void drawClassQuiz();
	void drawClassResult();
	void drawBirth();
	void drawReview();
	void drawEnd();
	void drawBarter();
	void drawDeath();
	void drawTravel();
	void drawMap();
	void drawDetected(const std::function<bool(float, float, float*, float*)>& toScreen);
	void useSkill(int skill, int use, float amount = 1.0f);
	// magic.cpp
	// casterRef >= 0: another actor cast it, so Reflect can bounce it back to them; spellCost: what Spell Absorption
	// gives back as magicka (OpenMW: the spell's cost)
	// castChance: the caster's chance for the spell (resistance: x 50 / castChance); 100 for potions, items, traps
	void applyEffectToPlayer(const SpellEffect& e, const std::string& source, int casterRef = -1, float spellCost = 0.0f,
		float castChance = 100.0f);
	// byPlayer: the player cast it (Reflect sends it back to them); spellCost: what Spell Absorption gives the actor
	void applyEffectToActor(int ref, const SpellEffect& e, bool byPlayer = true, float spellCost = 0.0f,
		float castChance = 100.0f, const std::string& source = "");
	void updateActorEffects(int ref, float dt);   // spells running on an actor, a second at a time
	bool instantEffect(int effect, float magnitude);   // cures, dispel, Mark / Recall, Interventions
	void updateEffects(float dt);
	void endPlayerEffect(size_t i);                    // an active effect ends: its changes undone, it is removed
	std::vector<std::string> knownSpells();
	int spellSchool(const SpellDef& sp, float* lowest = nullptr);
	std::string spellSound(const SpellDef& sp, int kind);
	// A spell's area effects of this range where it lands: everyone within (but who it struck already);
	// owner -1 the player's spell (hits the caster too when close), else an actor's (hits the player)
	void areaBurst(const SpellDef& sp, int range, const float at[3], int struck, int owner);   // 0 cast, 1 bolt, 2 hit, 3 area: the first effect's, else the school's
	int castChance(const SpellDef& sp);
	float enchantCastCost(float cost);
	// Formula pieces (formulas.cpp), also read by the spec tests
	float playerFatigueTerm();
	float repairChance();
	int repairAmount(float quality, int roll);
	float rechargeChance();
	float rechargeGain(int soul, int roll, float chance);
	float lockChance(int lockLevel, float quality);
	float trapChance(float trapCost, float quality);
	float persuadeChance(int npcRef, int kind, float* parts = nullptr);   // parts[8]: p1 p2 p3 n1 n2 n3 d raw
	float resistBase(int effect);                  // resist - weakness (+ shield): percent before the Willpower roll
	float actorResistBase(int ref, int effect);    // an actor's resist - weakness to an effect
	float resistX();                               // the Willpower term of the roll: (Willpower + Luck / 10) x fatigue x 0.5
	int crimeBounty(int kind, int arg);            // iCrime...: what a crime adds to the bounty (arg: the stolen value)
	void castItem(const std::string& itemId);
	void releaseSpell(const SpellDef& sp, int school);
	void beginCast(const SpellDef& sp, int school, bool animate);   // the cast's sound, hands and animation; the spell lands at its release
	void finishCast();                                // the release: the chance roll, then the spell lands
	void castUpdate(float dt);                        // lands a pending spell when its animation gets to the release
	bool castBusy() const { return castPending || castLock > 0.0f || castWait > 0.0f; }
	void strikeEnchantment(int target, const Object* missile = nullptr);   // the player's weapon's (or arrow's) on-strike enchantment
	void npcStrikeEnchantment(int attacker, const Object* item);           // an NPC's weapon or arrow on the player
	bool playerShieldsBurn(int attacker);   // the player's elemental shields on an attacker (false: it died)
	void wearLauncher(const Projectile& p);  // a missed / wall-struck shot wears the bow by 1
	bool npcShieldsBurn(int victim);        // an NPC's elemental shields on the player (false: the player died)
	void castSpell();
	void consume(const std::string& itemId);
	// crime (combat.cpp)
	void reportCrime(int victim, int bounty);
	// What the people who saw it think of it (OpenMW's reading): disposition and fight change, and at
	// fight 100 they attack. kind: CRIME_* (value: a theft's worth)
	enum { CRIME_THEFT, CRIME_PICKPOCKET, CRIME_TRESPASS, CRIME_ASSAULT, CRIME_MURDER };
	void crimeSeen(int kind, int value, int victim);
	void crimeAgainstFaction(const std::string& faction);
	// The player took count of an owner's item: stolen; a crime when an NPC saw it
	void takeOwned(int from, const std::string& item, int count);
	void playerDies();                            // death screen
	float breath = -1.0f;                         // seconds of breath left under water (-1: breathing air)
	bool overEncumbered = false;
	// Music: exploring (shuffled), battle while something fights the player (back 5 s after)
	bool battleMusic = false;
	float calmFor = 0.0f;
	void playMusic(bool battle);
	void updateMusic(float dt);
	int crimeWitness(int except = -1);            // an NPC that saw the player just now (not except), or -1
	InventoryItem* playerToolItem();              // equipped lockpick or probe
	void useTool(InventoryItem& tool);            // pick the target's lock / disarm its trap
	bool springTrap(int ref);                     // opening a trapped door / container: its spell hits the player
	bool equipItem(int inventoryIndex, bool force = false);           // false: refused (a message says why)
	void startGameScripts();
	void givePlayerStartSpells();                 // autocalc PC Start spells (character creation's end)
	bool deathHaveSave = false;                   // the death screen offers loading it
	float playerLoad = 0.0f;                      // carried weight / capacity, 0..1 (fatigue costs)
	int barterCount = 0;                          // barter: how many of the picked stack (0: not choosing yet)
	bool deletingSave = false;                    // Saves screen: asking before deleting the selected save
	int haggle = 0;                               // barter: the player's offer against the price, in 5% steps
	bool haggleAccepted(int price, int offer);    // Morrowind's haggling roll (OpenMW's reading)                      // Main, Startup, VampireCheck (new game / old saves)
	// Quick keys: ZL + D-pad in Inventory / Magic sets one to the highlighted entry; in play it uses it
	bool assignQuickKey(const std::string& entry, const std::string& name);   // true: ZL + D-pad pressed
	void useQuickKey(int k);           // equips, taking off what held that slot
	void guardCheck(int ref, float dt);
	int arrestingGuard = -1;
	// screens
	void drawMagic();
	void drawSpells();
	void drawArrest();
	void drawTraining();
	void drawPersuade();
	void drawAlchemy();
	void drawSpellmaking();
	void drawEnchanting();
	void drawRecharge();
	bool rechargeItem(int item, int gem);          // a soul gem's soul into an enchanted item's charge
	std::string rechargeGem;                       // the soul gem Recharge was chosen on (its soul's creature)
	void openEnchanting(int enchanter);           // enchanter: the NPC (a price, sure to work), -1: the player's own
	void enchantChoices(std::vector<int>& items, std::vector<int>& gems);
	float enchantPoints(int castType, bool precise = false);
	std::string lastBrewed;                       // the potion the last successful brew made (harness checks)
	void makeEnchantment(float points, int soul, float chance, int price);
	struct EnchantCalc { float points = 0.0f, capacity = 0.0f, chance = 0.0f; int price = 0, soul = 0; };
	EnchantCalc enchantCalc();                    // the enchanting screen's numbers for what's chosen
	bool enchantConfirm();                        // its Enchant button (false: refused)
	int spellmakePrice(float* cost);
	bool spellmakeConfirm();                      // the spellmaking screen's Buy button
	void trainerSkills(int best[3]);              // barterRef's three best skills
	int trainPrice(int skill);
	int baseSkill(int skill);                      // the skill without Fortify / Drain / Damage / abilities (OpenMW's base)
	int trainPriceFor(int npcRef, int skill);      // base skill x iTrainingMod at the NPC's barter price
	int travelPriceFor(int npcRef, float distance, int followers, bool interior);
	bool trainSkill(int skill);                   // the training screen's Train button
	void openBarter(int ref);
	bool barterTrade(bool sell, int index, int price, int from = -1, int count = 1);   // count items bought (from's
	                                                                                    // contents: the merchant's or a
	                                                                                    // chest of theirs) / sold
	std::vector<std::pair<int, int>> merchantGoods(int ref);
	int enchanterRef = -1, enchantItem = -1, enchantGem = -1, enchantType = -1;
	std::vector<SpellEffect> makeEffects;        // spellmaking / enchanting: the effects being put together
	int makeSel = -1;
	int effectFlags(int effect);
	float effectCost(const SpellEffect& e);
	float madeSpellCost(const std::vector<SpellEffect>& effects);
	bool effectEditor(std::vector<SpellEffect>& fx, int& sel, int ranges, float y0 = 24.0f, int rows = 8);
	std::vector<SpellEffect> knownEffects();
	SpellEffect newEffect(const SpellEffect& from, int ranges);
	std::vector<std::string> alchemySlots;        // up to four ingredients chosen
	bool brewPotion();                            // makes a potion of the chosen ingredients (they're used up)
	std::string effectLabel(int effect, int skill, int attribute);   // "Fortify Strength", "Restore Health"
	void persuade(int kind);                      // 0 admire, 1 intimidate, 2 taunt, 3..5 bribe 10 / 100 / 1000
	bool serviceRefused();                        // the speaker's Service Refusal, when their disposition is too low
	void drawSaves();
	void drawGameMenu();
	void toggleGameMenu();            // START
	bool wantQuit = false;            // Quit in the game menu: main loop exits
	const char* dataDir = "";
	// rest / wait, levelling, the calendar (rest.cpp)
	int restHours = 1;                // chosen on the rest screen
	bool restSleep = false;           // asleep (health and magicka come back) rather than waiting
	bool restBed = false;             // opened by a bed
	int restDone = -1, restTotal = 0; // hours passed so far while resting (-1: still choosing)
	int restFrames = 0;
	std::vector<int> levelPicks;      // attributes chosen on the level-up screen
	int levelCursor = 0;              // 0..7 attributes, 8 OK
	void openRest(bool bed);
	void drawRest();
	void restHour();
	void finishRest();
	void drawLevelUp();
	void applyLevelUp();
	float skillNeed(int skill);
	void raiseSkill(int skill, bool fromBook = false);
	void readBook(const Object* o);
	int attrMultiplier(int attr);
	int levelUpTotal();
	bool enemiesNear();
	std::string dateText(bool withHour = true);
	// test harness (autoinput tokens)
	void testBoost(int value);
	void testGive(const std::string& id, int count = 0, bool topUp = false);   // count 0: one (50 arrows); topUp: only what's missing
	void testPlace(const std::string& id, float dist);
	void testHit(const std::string& id);
	void testGoto(const std::string& cell);
	int testFindRef(const std::string& id);         // as written, else with '_' read as spaces (the one PLACE moved, even dead)
	int testPlaced = -1;                            // the actor PLACE last put in front of the player
	int travelPrice(int ref, const TravelDest& d);
	// Objects kept in memory: beyond this many, the cells used longest ago leave (World::evictCells)
	static const int kRefBudget = 20000;
	float evictTimer = 0.0f;
	int mapCell = -1;                 // the cell whose name was last put on the map
	int corprusShown = 0;             // Corprus level the stats were last computed at
	bool refInUse(int ri) const;      // something in the session points at that object
	void evictFarCells(int budget = kRefBudget);
};
