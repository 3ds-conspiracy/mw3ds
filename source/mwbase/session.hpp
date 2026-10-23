#pragma once

#include "mwbase/sessiontypes.hpp"
#include "mwgui/windowmanagerstate.hpp"
#include "mwmechanics/mechanicsstate.hpp"
#include "mwrender/renderstate.hpp"
#include "mwsound/soundstate.hpp"
#include "testdrive/teststate.hpp"

// The running game: world state, dialogue, menus, and everything scripts call into.
const char* summonCreatureId(int effect);    // the creature a summoning effect calls (mwmechanics/spelleffects.cpp), nullptr if none
float npcClimbSlope(const World& w, const Ref& r);   // the steepest land an actor walks up now, degrees (mwphysics/movementsolver.cpp)

struct Session : ScriptHost, WindowManagerState, MechanicsState, RenderState, SoundState, TestState
{
	Session()
	{
		w.awarenessHook = [this](int ref) { return awarenessCheck(ref); };
		w.shouldAttackHook = [this](int ref) {
			return w.refs[ref].ai == AI_COMBAT || fightTermOf(ref, w.distanceToPlayer(ref)) >= 100.0f;
		};
	}
	World w;
	int buttonPressed = -1;
	std::string typedText;            // tests: what the system keyboard returns (TYPE:)
	float boundsNoteUntil = 0.0f;
	float viewNow() const       // the share of the view drawn: the console's shortest or longest, or the dynamic one
	{
		return viewMode == VIEW_MIN ? g_profile.viewScaleMin : viewMode == VIEW_MAX ? g_profile.viewScaleMax
			: viewMode == VIEW_MID ? 0.5f * (g_profile.viewScaleMin + g_profile.viewScaleMax) : viewDrawn;
	}
	std::vector<Projectile> projectiles;
	// Third person (SELECT): the player's body drawn, the camera behind them; also character creation's
	// face preview (previewFace)
	PlayerBody body;
	void viewCamera(RenderCamera& cam);
	bool saveUnreadable = false;      // start(): the save asked for is there but won't read (nothing started over it)
	void revealLocalMap();            // the squares round the player come out of the fog
	bool drawLocalFog(float ox, float oy, float picture);
	float frameDt = 1.0f / 30.0f;     // this frame's time, menus open or not (main.cpp; the map's D-pad panning)
	bool ensureMapTile(int i);
	void freeMapTiles();
	void updateLocalMap();            // inside a frame, before the screens are drawn
	bool ensureMapTexture();
	void drawMapView(float x, float y, float vw, float vh, float zoom, bool full, bool local);
	bool ended = false;
	bool autotest = false;
	bool kitV2() const { return testKit >= 2 || w.itemCount("uber_ring") > 0; }   // (also a run resumed from a v2 checkpoint)
	bool testLegit = false;       // LEGIT: from here a test may do only what a player can (uber quest tests)
	// Monitors (logged "monitor: ...", the harness fails a run on them): someone playing a walk / run cycle
	// without getting anywhere (the female skeleton bug, dialogue without Idle)
	void monitorActors(float dt);
	void monitorCellChanged();
	void noteHudMismatch(int left, int right);   // the HUD drew differently in the two eyes
	bool pickpocketCaught(float valueTerm);       // Morrowind's pickpocket roll: true = noticed
	int pickpocketBar(int victim, float valueTerm);   // the roll above is noticed when it is over this (0..75)
	float aimShift = 0.0f;
	int hudEyes = 1;                  // passes this frame (2 in 3D)           // this eye's offset of the crosshair / target label (main.cpp, 3D)
	float aimDepth();                 // how far away what the crosshair points at is
	std::unordered_map<int, float> sayUntil;
	float fps = 0.0f, cpuMs = 0.0f, gpuMs = 0.0f;

	// savePath: continue from that save when it exists (and no startCell is given)
	bool start(const char* dataDir, const std::string& startCell = "", const char* savePath = nullptr);
	bool save();
	void shutdown();                  // frees the cell and stops its sounds (before deleting the session)
	const char* savePath = nullptr;
	void skipChargen();
	void giveItems(const std::vector<std::pair<int, std::string>>& items);
	void updateAmbience();
	void updateLoopSounds();
	void stopLoopSounds();
	void updateWeatherEffects(float dt);
	void updateSky();
	void updateDress(float dt);
	void npcSummonRelease(int ri, const SpellDef& sp);   // the creature comes to fight the player; a ward lands on it
	void npcSummonsUpdate();                             // they go when their time is up or the caster dies
	float vfxTrimTimer = 0.0f;
	void updateCreatureFx(float dt);
	void updateWorldFx();                          // placed objects with a world visual (game firstPerson.worldFx) in range
	void playObjectGroup(int ref, const std::string& group, bool loop) override;
	void drawCreatureFx(const float eye[3], bool secondEye);
	void trimVfxPieces();                          // spell visuals' pieces nothing is playing let go (memory)
	// loop: its animation repeats for all of life (a continuous effect's hit: OpenMW's ContinuousVfx)
	// effect: the magic effect it shows (its particle texture on the shapes that take one), -1 none
	// Returns whether it started (it shows only near the player, and so many at once)
	bool spawnVfx(const std::string& staticId, const float pos[3], float life = 1.2f, int follow = -2, float yaw = 0.0f, float scale = 1.0f,
		bool loop = false, int effect = -1);
	bool vfxBonePos(int follow, const std::string& bone, float out[3]);   // a bone of the player's body / an actor in the world
	// kind 0 cast (every model its effects name), 1 hit, 2 area (each effect of that range with an area, at twice the
	// area in feet: OpenMW's explodeSpell); range: the effects kind 2 bursts. An effect with none: the VFX_Default one
	void spellVfx(const SpellDef& sp, int kind, const float pos[3], int follow = -2, float yaw = 0.0f, int range = 2);
	void castVfx(const SpellDef& sp, int caster);  // the cast's visual on its caster (-1 the player), as long as it casts
	std::string boltPiece(const SpellDef& sp, int* effect = nullptr);   // what a bolt of it flies as ("" none: a glow), for which effect
	C3D_Tex* fxTexture(const ActorMesh& m, C3D_Tex* own, int effect);  // a visual's mesh texture: an fx shape the effect's own
	C3D_Tex* skyTex(const std::string& name);      // acquired once, kept
	void startRest(int hours);
	void drawPrecipitation(const float eye[3], bool secondEye);   // after the world, in each eye

	// mwmechanics/aicombat.cpp, mwmechanics/character.cpp, mwmechanics/combat.cpp
	void combatUpdate(const PlayerInput& in, float dt, bool menu);
	float playerSwing(float charge, const PlayerInput& in);   // the blow's strength for its follow-through (0: a miss)
	void npcCombat(int ref, float dt);
	void npcStrike(int ref);
	void npcWander(int ref, float dt);
	bool npcTalkStand(int ref, float dt);
	void npcPackage(int ref, float dt);          // travel / follow / escort / activate packages
	void npcDuel(int ref, float dt);              // StartCombat on another actor: it walks up to its foe and fights it
	void allyFight(int ref, float dt);            // summoned creatures go for whoever fights the player
	int guardFoe(int ref);                        // a guard's hostile creature in a fight nearby (-1: none)
	bool npcStep(int ref, Cell& cell, float dirX, float dirY, float step);
	// In reach in a fight: OpenMW's combat moves (a side step, an archer backing away). True when it moved this frame
	bool combatMove(int ref, Cell& cell, float dist, float facing, bool ranged, const Object* foeWeapon, float dt);
	void makeHostile(int ref);
	// spell: a damage effect (health only, no hit recoil, hurt sound, blood or voice: OpenMW's applyMagicEffect)
	void damageNpc(int ref, float dmg, bool fatigueOnly, bool spell = false);
	bool friendlyHit(int ref);     // the player's blow or harmful spell on one siding with them is forgiven
	void actorGravity(float dt);
	bool inSwing(int ref, float* dist = nullptr);         // a swing now would reach them
	int swingTarget();                                   // who a swing now would strike (-1: nobody)
	void npcKnockUpdate(int ref, float dt);          // down or out: the time on the floor, then up
	bool idleVoiceOk(int ref);                            // may say an Idle line now (not following or escorting)
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
	float actorHalfLength(int ref);                      // collision box half length (OpenMW's getHalfExtents().y())
	float npcWeaponRating(int ref, float distance);      // OpenMW's rateWeapon against the player
	float npcWeaponRatingOf(int ref, const Object* weapon, float distance);   // ... for that weapon (or its arrows)
	float npcEffectRating(int ref, const SpellEffect& e, bool withEnemy);   // OpenMW's rateEffect, the player the enemy (none: a potion)
	float npcEffectsRating(int ref, const std::vector<SpellEffect>& effects, bool withEnemy, bool spellMult = true);   // OpenMW's rateEffects
	bool npcDrinkPotion(int ref, float distance);
	float fightTermOf(int ref, float distance);
	float haggleChance(int merchantRef, int price, int offer, bool selling, int* dOut = nullptr);
	float walkSpeedFor(int speed);
	float runSpeedFor(int speed, int athletics);
	float jumpSpeedFor(int acrobatics, float jump, float load, bool running, float fatigueTerm);
	float actorRunSpeed(int ref);              // the roll (0..99) must be at least this
	float elementalShieldDamage(float magnitude, int destruction, int willpower, int luck, float fatigue, float fatigueMax,
		float resistance, int roll);
	void catchDiseases(int carrier);              // the player touched a diseased actor (a blow either way)
	void updateDetection(float dt);
	void knockDown(int ref, bool out);
	bool npcMoveTo(int ref, const float target[3], float speed, float dt, bool faceMove);
	void npcOpenDoor(int ref, int door);          // a shut door it walked into, opened if it may (OpenMW's openDoors)
	void npcCastSpell(int ref);
	void npcFire(int ref);
	void playerFire(float charge);
	// mwworld/projectilemanager.cpp
	void fireProjectile(int owner, const float from[3], const float dir[3], float speed, const std::string& item,
		const std::string& weapon, float charge, const std::string& spell);
	void updateProjectiles(float dt);
	void projectileHits(Projectile& p, int victim);
	void drawExtras(const float eye[3], bool secondEye = false, bool arms = true);
	struct BlowRoll { bool lands = true; int kind = 1; float damage = 1.0f, skill = 1.0f; };   // kind 0 glance 1 hit 2 crit
	BlowRoll rollBlow(float chance, bool ranged, bool vanilla);
	void addIndicator(const std::string& text, u32 rgb);
	void drawIndicators(float dt);
	void blowSound(int ref, const BlowRoll& b);
	float stereoAmount = 0.0f;        // eases 0..1 so 3D never snaps on or off
	void adaptView(float workMs, float frameMs = 0.0f);   // frameMs 0: measured here
	float stereoTarget(float slider, float dt);
	void noteRenderCost(float cpuMs, float gpuMs, bool stereo, float eyeCmd);
	void loadSettings();
	float difficultyScale(bool toPlayer) const;   // damage multiplier: to the player / dealt by the player
	void drawOptions();               // the Options tabs: Prefs, Audio, Video (SCR_DISPLAY), Controls (SCR_LOOK)
	void drawControls();              // Options > Controls > Buttons: what each button does
	void drawConsole();               // Options > Console: one MWScript line at a time (mwgui/console.cpp)
	void saveSettings();
	void viewModelUpdate(const PlayerInput& in, float dt);
	// repair (mwgui/windowmanagerimp.cpp)
	void drawRepair();
	void openRepair(int smith, int tool);        // smith: the NPC (-1: the player's own tool, an inventory index)
	void repairOne(int item);                    // one repair of that inventory item, as the screen's row does
	bool buySpell(int merchant, const std::string& id);   // the spell merchant's Buy: paid and learned
	int repairPrice(int ref, const InventoryItem& it);
	bool isMerchant(int ref);
	bool merchantTrades(int ref, const Object* o);
	int barterPrice(int ref, int value, bool buying);
	void update(const PlayerInput& in, float dt);
	// update's parts, in the order it runs them (each in the file OpenMW keeps that job in)
	void updateWindows();
	void updateCorprus();
	void openPendingMenu();
	void playerControls(PlayerInput& in, bool menu);
	void playerMove(const PlayerInput& in, float dt, u64& t);
	void aimAndActivate(const PlayerInput& in, bool menu);
	void updateMouths(float dt);
	void updateFootsteps(const PlayerInput& in, bool menu, float dt);
	void updateRegionSounds(float dt);
	void burnHeldLight(float dt);
	void updateLights();
	void ageVfx(float dt);
	void updateFadeAndNotes(float dt);
	void drawTop();
	void drawHurtFlash();
	void itemTip(const Object* o, const InventoryItem* it);   // it: that one's wear and charge (nullptr: new)
	void spellTip(const SpellDef& sp);
	int armorSkill(const Object* o);      // the armor skill a piece of armor trains: by its weight for its slot
	std::string effectLine(const SpellEffect& e, bool showRange);   // "Fire Damage 5 to 10 pts for 5 secs on Target"
	void drawBottom();
	bool menuOpen() const { return !messages.empty() || screen != SCR_NONE || dlg.open; }
	bool worldPaused() const;      // a menu that stops the world (not talk, barter and the services: the NPC keeps moving)
	void setName(const std::string& name);
	void setText(bool confirmed, const std::string& text);   // what the keyboard wantText asked for returned
	std::string textNow();                                     // the text it starts from
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
	void report(const std::string& text) override { console.print(text); }
	bool centerOnCell(const std::string& name) override;
	bool centerOnExterior(int x, int y) override;
	bool toggleGodMode() override { testGod = !testGod; testGodBlows = false; return testGod; }
	bool moviePlaying() const { return movieFile != nullptr; }
	void movieUpdate(float dt);                   // next frame when it's time; the end
	void movieDraw();                             // on the top screen (main.cpp)
	void stopMovie();
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
	void hurtPlayer(float health) override;
	// Harm that goes on (a damage spell or poison, sun damage, drowning, lava): the red flash starts again only once
	// the last has faded (OpenMW's activateHitOverlay(false)); true when it started
	bool hurtFlashOngoing() { if (hurtFlash > 0.0f) return false; hurtFlash = 1.0f; return true; }
	void sheathe() override { sheatheTimer = 99.0f; }
	void goToJail() override;
	void teleportPlayer(int cell, const float pos[3], float yaw) override
	{
		// (outdoors to outdoors only into a cell already loaded round the player: farther, Almsivi or Divine Intervention,
		// Recall, a script's PositionCell, goes through the loading screen as travel does, as OpenMW changes the cell grid
		// with its loading screen; the player stood in the new place before its cells had come in)
		if (cell == w.current || (!w.cells[cell].interior && !w.cells[w.current].interior && w.cells[cell].live))
		{
			// Same space: stand there, set down on the floor as a move into another cell is (OpenMW's PositionCell
			// adjusts the player to the ground: the new game's CharGen put the player 24 units over the ship's deck,
			// and they fell ~130 units onto it). Feet first, so the cells around them stream in before the floor is looked for
			memcpy(w.player.feet, pos, sizeof(w.player.feet));
			w.streamExterior(true);
			float eye[3] = { pos[0], pos[1], pos[2] + PLAYER_EYE_HEIGHT };
			Scene scene = w.scene();
			playerSpawn(w.player, scene, eye, yaw, w.player.pitch);
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
	void mix3d(const float* pos, float& volume, float& pan, float minDist, float maxDist, bool inverse = false);
	void greetings(float dt);
	std::string targetLabel(int ref);
	std::vector<std::string> targetPopupLines(int ref);    // the name over the crosshair, wrapped to fit the top screen
	std::vector<std::string> targetPromptLines(int ref, float* scale = nullptr);   // "A: Open <name>" on the bottom screen: one or two lines, at that scale

	// screens (mwgui/hud.cpp, mwgui/messagebox.cpp)
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
	void drawClassCreate();
	void drawBirth();
	void drawReview();
	void drawEnd();
	void drawBarter();
	void drawDeath();
	void drawTravel();
	void drawMap();
	void drawDetected(const std::function<bool(float, float, float*, float*)>& toScreen);
	void useSkill(int skill, int use, float amount = 1.0f);
	// mwmechanics/spellutil.cpp
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
	// Formula pieces (mwmechanics/formulas.cpp), also read by the spec tests
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
	void strikeEffects(const SpellDef& en, int target);                    // its hit visual and sound on the one struck (-1 the player)
	void rechargeItems(float seconds);                                     // enchanted items win charge back over time
	bool playerShieldsBurn(int attacker);   // the player's elemental shields on an attacker (false: it died)
	void wearLauncher(const Projectile& p);  // a missed / wall-struck shot wears the bow by 1
	bool npcShieldsBurn(int victim);        // an NPC's elemental shields on the player (false: the player died)
	void castSpell();
	void consume(const std::string& itemId);
	// crime (mwmechanics/mechanicsmanagerimp.cpp)
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
	void playMusic(bool battle);
	void updateMusic(float dt);
	int crimeWitness(int except = -1);            // an NPC that saw the player just now (not except), or -1
	InventoryItem* playerToolItem();              // equipped lockpick or probe
	void useTool(InventoryItem& tool);            // pick the target's lock / disarm its trap
	bool springTrap(int ref);                     // opening a trapped door / container: its spell hits the player
	bool equipItem(int inventoryIndex, bool force = false);           // false: refused (a message says why)
	void startGameScripts();
	void givePlayerStartSpells();                 // autocalc PC Start spells (character creation's end)
	bool haggleAccepted(int price, int offer);    // Morrowind's haggling roll (OpenMW's reading)                      // Main, Startup, VampireCheck (new game / old saves)
	// Quick keys: ZL + D-pad in Inventory / Magic sets one to the highlighted entry; in play it uses it
	bool assignQuickKey(const std::string& entry, const std::string& name);   // true: ZL + D-pad pressed
	void useQuickKey(int k);           // equips, taking off what held that slot
	void guardCheck(int ref, float dt);
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
	void openEnchanting(int enchanter);           // enchanter: the NPC (a price, sure to work), -1: the player's own
	void enchantChoices(std::vector<int>& items, std::vector<int>& gems);
	float enchantPoints(int castType, bool precise = false);
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
	int barterBalance();                          // the deal's worth to the player: what the merchant pays, less what they do
	bool barterOffer();                           // the deal's Offer: settled at the haggled balance, or refused
	bool barterTrade(bool sell, int index, int price, int from = -1, int count = 1, bool thanks = true);   // count items bought (from's
	                                                                                    // contents: the merchant's or a
	                                                                                    // chest of theirs) / sold
	std::vector<std::pair<int, int>> merchantGoods(int ref);   // (reference, index into its contents; -1: the reference is the item itself, lying in the shop)
	ContentItem merchantGood(const std::pair<int, int>& g);    // the stack a merchantGoods entry stands for
	void syncEnchantName();
	int makeButtons(const std::string& buy);
	int effectFlags(int effect);
	float effectCost(const SpellEffect& e);
	float madeSpellCost(const std::vector<SpellEffect>& effects);
	bool effectEditor(std::vector<SpellEffect>& fx, int& sel, int ranges, float y0 = 24.0f, int rows = 8);
	std::vector<SpellEffect> knownEffects();
	SpellEffect newEffect(const SpellEffect& from, int ranges);
	bool brewPotion();                            // makes a potion of the chosen ingredients (they're used up)
	std::string effectLabel(int effect, int skill, int attribute);   // "Fortify Strength", "Restore Health"
	void persuade(int kind);                      // 0 admire, 1 intimidate, 2 taunt, 3..5 bribe 10 / 100 / 1000
	bool serviceRefused();                        // the speaker's Service Refusal, when their disposition is too low
	void drawSaves();
	void drawGameMenu();
	void toggleGameMenu();            // START
	bool wantQuit = false;            // Quit in the game menu: main loop exits
	const char* dataDir = "";
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
	void testPlaceAt(const std::string& id, const float at[3], float yaw);
	void testHit(const std::string& id);
	void testGoto(const std::string& cell);
	int testFindRef(const std::string& id);         // as written, else with '_' read as spaces (the one PLACE moved, even dead)
	int travelPrice(int ref, const TravelDest& d);
	void openTravel();                              // the dialogue's Travel button (barterRef = the speaker)
	bool travelGo(int sel);                         // the travel list's Go: pay and leave (false: refused)
	// Objects kept in memory: beyond this many, the cells used longest ago leave (World::evictCells). An Old 3DS
	// keeps fewer (a Ref is about 1 KB: 20000 of them were half its heap)
	int refBudget() const;
	void relieveMemory(bool force = false);             // the heap ran out (g_memoryLow): far cells' objects and map tiles go
	int trimSkeletons();              // skeletons no loaded actor uses go (the eviction timer, relieveMemory, a load)
	// A door's loading screen is the moment to give back what the place left behind, before the next one is read into the gaps
	// (an Old 3DS's heap: far cells' objects, skeletons, decoded sounds, map tiles, the string pool's next block)
	void cleanForLoad();
	float evictTimer = 0.0f;
	bool refInUse(int ri) const;      // something in the session points at that object
	void evictFarCells(int budget = -1, bool now = false);   // -1: refBudget(); now: EVICT (see World::evictCells)
};
