#pragma once

// Session's state for the rules: combat, casting, the first-person view model's actions, sneaking, summons
// (OpenMW's MechanicsManager)
// Session derives from it: the members are Session's (s.attackCharge), kept here beside the code that uses them
#include "mwbase/sessiontypes.hpp"

struct MechanicsState
{

	// combat (mwmechanics/combat.cpp)
	float attackCharge = -1.0f;       // 0..1 while X is held, -1 idle
	int swingCount = 0, castCount = 0;   // blows let go and spells cast (the tests count them)
	bool castPending = false;         // a spell's animation is playing: it lands at the animation's release mark
	SpellDef castDef;                 // that spell, as chosen when the cast began
	int castSchool = 5;
	float castLock = 0.0f;            // the cast animation's whole group plays out (one upper-body state): nothing else starts
	float castWait = 0.0f;            // no new cast for this long (when there's no animation to wait for)
	float enemyUntil = 0.0f;
	float hurtFlash = 0.0f;
	bool bodyHit = false;                        // struck: the third-person body plays a Hit group (PlayerBody::update)
	int bloodHits = 0;                // blood splats thrown by blows (tests: EXPECT:bloodhits) ...
	float bloodLow = 2.0f, bloodHigh = -1.0f;   // ... and the lowest / highest fraction of the body's height one went to (bloodlow / bloodhigh)
	u8 bloodModels = 0;               // the blood splat models hits have thrown so far (bits 0..2; the test driver's bloodmodels)
	bool playerDead = false;
	bool weaponDrawn = false;         // weapon (or fists) out: the view model shows it
	float drawTimer = 0.0f;           // > 0 while the weapon comes out; the wind-up waits for it
	bool pickQueued = false;          // a press while the lockpick / probe came up: tried once it's up
	float sheatheTimer = 0.0f;        // seconds without fighting; the weapon goes away after a while
	int attackKind = 0;               // 0 chop, 1 slash, 2 thrust: picked when the wind-up starts
	std::string bodyAttack;           // the third-person body's group for that blow (Attack1hSl ...)
	float blockUntil = 0.0f;          // shield raised (view model)
	float detectTimer = 0.0f, sneakSkillTimer = 0.0f;
	ViewModel vm;
	bool deathView = false;       // the view went to third person for the player's death (back in when they live again)
	float blowStrength = 1.0f;                // the last blow let go (0..1): the body's follow-through
	static const int kBountyAssault = 40, kBountyMurder = 1000;   // iCrimeAttack, iCrimeKilling (defaults: Session::crimeBounty reads the GMSTs)
	bool testGod = false;
	bool testGodBlows = true;     // GOD:2 clears it: the player still can't die, but blows roll, hurt and wear as usual
	int persuadeRoll = -1;        // tests: the die roll a persuasion or an enchanting uses (-1: random)
	int resistDie = -1;           // RESISTROLL: the die (0..99) a creature or NPC resists a spell effect with (-1: random)

	// world interaction
	int target = -1;
	float stepTimer = 0.0f;
	bool stepLeft = false;
	// NPCs' and creatures' own summons (OpenMW's AI casts them in a fight): the caster, the creature, when it goes;
	// a summon being cast (castSpell -2) waits in pendingSummon. Not saved: a load starts with none
	struct NpcSummon { int caster, creature; float ends; };
	std::vector<NpcSummon> npcSummons;
	std::unordered_map<int, std::string> pendingSummon;
	float testFatiguePin = -1.0f;                // SETFATIGUE holds fatigue here (no regeneration) until FILL; -1: free
	bool testNoFatigueRegen = false;             // FATIGUEREGEN:0: fatigue doesn't come back by itself (a test reads the effects alone)
	float testCastTaken = 0.0f;                  // the magicka the last cast took (EXPECT:spellmagicka)
	float playerMoveForward = 0.0f;               // this frame's forward input (blocking: fBlockStillBonus when <= 0)
	std::string deathSave;                        // the newest of the player's saves when they died ("": none)
	float playerLoad = 0.0f;                      // carried weight / capacity, 0..1 (fatigue costs)
	int arrestingGuard = -1;
	std::string lastBrewed;                       // the potion the last successful brew made (harness checks)
	int corprusShown = 0;             // Corprus level the stats were last computed at
};
