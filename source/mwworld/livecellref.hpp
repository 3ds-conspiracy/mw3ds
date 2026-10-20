#pragma once

// A reference in a loaded cell (OpenMW's LiveCellRef, CellRef and RefData in one)
#include "components/esm/defs.hpp"
#include "mwworld/esmstore.hpp"
#include "mwworld/containerstore.hpp"
#include "mwmechanics/aipackage.hpp"

struct RefRange { int batch; u32 first, count; };
struct cJSON;

// A placed reference in the cell: static data from cells/<cell>.json plus live state
struct Ref
{
	Atom id, idLower, type;
	RecName recType = REC_OTHER;           // type as a number (components/esm/defs.hpp)
	float pos[3], rot[3], scale = 1.0f;
	int count = 1;
	int lockLevel = 0;
	// Owner: an NPC, or a faction (members from ownerRank up may take it); a set ownerGlobal lets the
	// player use it (rented beds). Taking it is theft (World::ownedByOther)
	Atom owner, ownerFaction, ownerGlobal;
	int ownerRank = 0;
	Atom trap;                    // trap spell (doors / containers): cast on whoever opens it, until disarmed
	bool disarmed = false;        // the trap went off or a probe disarmed it (saved)
	Atom key;
	int cell = 0;                 // index into World::cells; -1: a free slot (its cell's objects left memory)
	u32 baseHash = 0;             // refHash as read from the cell file: saves keep only what changed
	int at = -1;                  // moved to another cell (PositionCell): the cell it stands in now
	bool spawnedRef = false;      // placed while playing (World::spawned owns the slot)
	bool dropped = false;         // an item the player (or a script) dropped: `dropItem` is what it is
	bool ally = false;            // summoned: fights for the player
	std::string hitBy;            // HitOnMe: the weapon that hit it last (lowercase), until asked
	bool knockedOut = false;      // OnKnockout pending
	int flee = -1;                // AI flee set by scripts (-1: the record's)
	int alarm = -1;               // AI alarm set by scripts
	bool attacked = false;        // GetAttacked: someone hit them
	float start[4] = {};          // where the level placed it (x, y, z, yaw): SetAtStart / GetStartingPos
	float startRot[2] = {};       // and its x / y rotation there
	float boxStart[6] = {};       // its box there (min, max): a moving object's box follows it
	bool hasDest = false, destEnd = false, destUnconverted = false;
	Atom destCell;
	float chaseStill = 0.0f, chaseWindow = 0.0f, chaseAnchor[2] = {};   // chasing: seconds without getting anywhere
	// In reach in a fight (OpenMW's combat moves): next look in, a side step (-1 left, 1 right) and its time left,
	// backing away (a ranged fighter from a melee one); side steps begun (for tests)
	float combatReact = 0.0f, sideTimer = 0.0f;
	s8 sideStep = 0;
	bool backingUp = false;
	int sideSteps = 0;
	bool destHasGrid = false;     // destination is outdoors: the exterior grid cell it lands in
	int destGrid[2] = {};
	float destPos[3], destRot[3];
	bool hasBox = false;
	float boxMin[3], boxMax[3];
	// The box the crosshair aims at, where the thing is now: its box at the level's placement, moved as
	// far as it has moved since. The one place a box follows its position (moved actors restored from a
	// cell's state kept their old box, and nobody could talk to them)
	void fitBox()
	{
		if (!hasBox)
			return;
		for (int k = 0; k < 3; k++)
		{
			boxMin[k] = boxStart[k] + pos[k] - start[k];
			boxMax[k] = boxStart[3 + k] + pos[k] - start[k];
		}
	}
	std::vector<RefRange> ranges;
	u32 colFirst = 0, colCount = 0;   // its own collision triangles (objects scripts enable / disable)
	std::vector<u32> savedCol;        // their indices while it is hidden
	int actor = -1;               // index into GameData::actors / Cell::actors
	int doorMesh = -1;            // index into Cell::doors
	const Object* obj = nullptr;

	std::vector<ContentItem> contents;   // containers: count, item id, and each stack's wear / soul / charge

	bool enabled = true;          // Enable / Disable
	bool pickedUp = false;        // taken into the inventory
	float doorAngle = 0.0f, doorTarget = 0.0f;
	int script = -1;              // index into World::scripts
	std::vector<u16> savedIndices;

	// actors
	int anim = -1;                // index into World::actors while its cell is loaded
	int cellActor = -1;           // index into Cell::actors (collision cylinder)
	int hello = -1;               // SetHello (-1: the record's; saved)
	struct TimedEffect { int effect; float magnitude, remaining; int key = -1; };   // key: the attribute or skill of a drain / fortify / absorb
	std::vector<TimedEffect> effects;  // spells on them (mwmechanics/spelleffects.cpp applyEffectToActor), not saved
	int awarenessRoll = -1;       // OpenMW's cached awareness roll (0..99), rerolled every 5 seconds
	float awarenessTimer = 0.0f;
	int friendlyHits = 0;         // blows from the player while following them (dialogue: Friendly Hit)
	float diedAt = -1.0f;         // World::gameHour of death (respawning, saved)
	float lastBarter = -1000.0f;  // game hour of the last trade: a day later their gold is back (saved)
	Atom levList;                 // placed from a leveled list: picked by the player's level when first read
	bool relib = false;           // drawn from its mesh library, not the cell's own actor (a leveled pick)
	float health = 0, healthMax = 0, fatigue = 0, fatigueMax = 0;
	int gold = 0;
	bool dead = false, died = false;   // died: OnDeath pending for its script
	bool murdered = false;        // OnMurder pending: the player killed them and it was murder
	float soulTrapUntil = -1.0f;  // Soultrap on them until this World::time: dying, their soul fills a gem
	bool aggressor = false;       // started the fight (killing them isn't murder)
	int duelFoe = -1;             // StartCombat on another actor: the ref it fights (not saved; -1: none)
	bool alarmed = false;         // a guard come to arrest the player (OpenMW's Alarmed, for the dialogue filter)
	bool moved = false;           // pos / rot[2] changed while fighting (placement must follow)
	int ai = 0;                   // AI_IDLE, AI_COMBAT
	float attackTimer = 0.0f, hitAt = -1.0f;
	float home[3] = {};           // where the level placed them (wandering stays near it)
	float wanderTo[2] = {}, wanderTimer = 0.0f;
	float wanderFrom[2] = {}, wanderCheck = 0.0f;   // stuck detection: where they were a second ago
	bool wandering = false;
	int voiceChannel = -1;
	float helloTimer = 0.0f;
	bool greeted = false;
	bool escortWaiting = false;   // escorting: stopped for the escorted one (resumes within 300, stops past 500; OpenMW)         // said its Hello; again only after the player went fGreetDistanceReset away (OpenMW)
	// combat (mwmechanics/aicombataction.cpp)
	float magicka = 0, magickaMax = 0;
	float potionTimer = 0.0f;     // a fight: until it weighs a potion against its weapon again (npcDrinkPotion)
	bool fleeing = false;
	// Gravity for people: nothing under them (a platform a script took away: Tarhiel) and they fall,
	// hurt by the landing as the player is
	bool falling = false;
	float fallVz = 0.0f, fallTop = 0.0f, floorCheck = 0.0f;
	float fleeTimer = 0.0f;       // until the flee choice is made again (OpenMW's ActionFlee cooldown, 3 s)
	u8 fleeState = 0;             // fleeing: 0 standing, 1 running blindly away, 2 running to fleeDest (OpenMW's FleeState)
	float fleeRun = 0.0f;         // seconds run blindly
	float fleeDest[3] = {};       // the path grid point it runs to
	float calmUntil = 0.0f;         // Calm: no attack on sight until this time
	float knockTimer = 0.0f;      // > 0: knocked down (or out) and helpless
	float boxLift = 0.0f;         // drawn this much above pos: its collision box resting on the slope (World::actorBoxLift)
	bool outCold = false;         // the knockdown under way is a knockout (a death then is DeathKnockOut)
	float castTimer = 0.0f;       // until the next spell they may cast
	int castSpell = -1;           // spell being cast (index into ActorDef::combatSpells), fires at hitAt
	bool shooting = false;        // the pending hitAt releases a projectile instead of a blow
	u8 attackKind = 0;            // the blow under way: a weapon's 0 chop, 1 slash, 2 thrust; a creature's attack 0..2
	char deathGroup[16] = "Death1";   // the death it played (a corpse laid down again lies the same way)
	int ammo = 0;                 // arrows / bolts / throwing weapons left
	float lastSeen = -100.0f;     // world time they last noticed the player (sneaking)
	std::vector<int> path;        // path grid points (global indices) still to walk, next first
	float repathTimer = 0.0f;
	float stuckTimer = 0.0f;
	// Getting unstuck (npcMoveTo): the way being taken, the side the last opening was on (1 right, -1 left), and the last
	// of OpenMW's evasion ways used (the next is one on; the first is right and forward)
	float evadeWay[2] = {};
	float evadeSide = 1.0f;
	int evadeDir = 6;
	float evadeAt[2] = {};          // where it last got stuck, and how many times running it has there
	int evadeTries = 0;
	// A follower held up on its way to the one it follows (npcPackage): it keeps to that one's footsteps until
	// trailUntil (World::time); trailSeq is the footstep it walks to, trailTimer the time until it looks again
	float trailUntil = 0.0f, trailTimer = 0.0f, trailBlind = 0.0f;
	unsigned trailSeq = 0;
	float trailGoal[3] = {};
	float gridUntil = -1.0f;        // World::time until which moves follow the path grid (a ledge cut the straight way)
	bool straightWay = false;       // npcMoveTo's last way was the straight one (else the grid's)
	float straightZ = 0.0f;         // its height when it last changed between the two
	float dropUntil = 0.0f;         // World::time until which a step outdoors may go down a ledge (npcMoveTo: the grid's way climbed back)
	float lastPos[2] = {};
	bool talkedToPC = false;
	int disposition = 50;
	int fight = -1;               // AI fight set by scripts (-1: the actor's own)
	int reputation = 0;
	// What scripts changed on an NPC or creature (saved): Set / Mod stats add to the record's, SetLevel and
	// Raise / LowerRank replace it (-1: the record's), AddSpell / RemoveSpell give and take spells (lowercase)
	std::vector<float> statDelta;   // 8 attributes then 27 skills; empty until a script changes one (memory)
	int level = -1, rank = -1;
	std::vector<std::string> spells, noSpells;
	// AI package (AIWander / AITravel / AIFollow / AIEscort / AIActivate, or the record's own)
	int aiPackage = AIPKG_NONE;
	std::string aiTarget;
	std::string aiCell;           // AIFollowCell / AIEscortCell: the destination's cell (lowercase name)         // follow / escort / activate: lowercase id, "player"
	float aiDest[3] = {};         // travel / escort / follow destination (all 0: none)
	float aiDuration = 0.0f;      // game hours, 0 = until done
	float aiStart = 0.0f;         // World::gameHour when it began
	float aiNearTime = 0.0f;      // travelling: seconds spent flat-close to the destination without getting there (AiTravel)
	int aiRange = -1;             // wander distance (-1: the record's)
	bool aiDone = false;          // GetAIPackageDone
	bool aiRepeat = false;        // the script's reset argument: the package starts over when it finishes
	bool aiActive = true;         // AIFollow from a script waits to see its target first (OpenMW's activation)

	bool visible() const { return enabled && !pickedUp; }
	struct InventoryItemLite { int condition = -1; std::string soul; float charge = -1.0f; } dropState;
};
struct Ref;
