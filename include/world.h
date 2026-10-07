#pragma once

#include <algorithm>
#include <functional>
#include <string>
#include <unordered_map>
#include <map>
#include <unordered_set>
#include <deque>
#include <vector>

#include "actors.h"
#include "cell.h"
#include "game.h"
#include "player.h"

struct RefRange { int batch; u32 first, count; };
struct cJSON;

// One stack in a container's or an actor's contents: (count, id) as the record lists it (a negative count restocks),
// with the same per-item state an inventory stack carries (InventoryItem): wear, a soul, a charge
struct ContentItem : std::pair<int, std::string>
{
	using std::pair<int, std::string>::pair;
	int condition = -1;           // -1: as new
	std::string soul;             // a soul gem's soul (the creature's id)
	float charge = -1.0f;         // -1: full
	bool plain() const { return condition < 0 && soul.empty() && charge < 0.0f; }
};

// AI packages, numbered as GetCurrentAIPackage reports them
enum { AIPKG_NONE = -1, AIPKG_WANDER = 0, AIPKG_TRAVEL = 1, AIPKG_ESCORT = 2, AIPKG_FOLLOW = 3, AIPKG_ACTIVATE = 4,
	AIPKG_IDLE = 5 };   // AIPKG_IDLE: the script's package ended and nothing replaced it (OpenMW: an empty sequence, the actor stands)

// A placed reference in the cell: static data from cells/<cell>.json plus live state
struct Ref
{
	std::string id, idLower, type;
	float pos[3], rot[3], scale = 1.0f;
	int count = 1;
	int lockLevel = 0;
	// Owner: an NPC, or a faction (members from ownerRank up may take it); a set ownerGlobal lets the
	// player use it (rented beds). Taking it is theft (World::ownedByOther)
	std::string owner, ownerFaction, ownerGlobal;
	int ownerRank = 0;
	std::string trap;             // trap spell (doors / containers): cast on whoever opens it, until disarmed
	bool disarmed = false;        // the trap went off or a probe disarmed it (saved)
	std::string key;
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
	std::string destCell;
	float chaseStill = 0.0f, chaseWindow = 0.0f, chaseAnchor[2] = {};   // chasing: seconds without getting anywhere
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
	std::vector<TimedEffect> effects;  // spells on them (magic.cpp applyEffectToActor), not saved
	int awarenessRoll = -1;       // OpenMW's cached awareness roll (0..99), rerolled every 5 seconds
	float awarenessTimer = 0.0f;
	int friendlyHits = 0;         // blows from the player while following them (dialogue: Friendly Hit)
	float diedAt = -1.0f;         // World::gameHour of death (respawning, saved)
	float lastBarter = -1000.0f;  // game hour of the last trade: a day later their gold is back (saved)
	std::string levList;          // placed from a leveled list: picked by the player's level when first read
	bool relib = false;           // drawn from its mesh library, not the cell's own actor (a leveled pick)
	float health = 0, healthMax = 0, fatigue = 0, fatigueMax = 0;
	int gold = 0;
	bool dead = false, died = false;   // died: OnDeath pending for its script
	bool murdered = false;        // OnMurder pending: the player killed them and it was murder
	float soulTrapUntil = -1.0f;  // Soultrap on them until this World::time: dying, their soul fills a gem
	bool aggressor = false;       // started the fight (killing them isn't murder)
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
	// combat (combat.cpp)
	float magicka = 0, magickaMax = 0;
	bool fleeing = false;
	// Gravity for people: nothing under them (a platform a script took away: Tarhiel) and they fall,
	// hurt by the landing as the player is
	bool falling = false;
	float fallVz = 0.0f, fallTop = 0.0f, floorCheck = 0.0f;
	float fleeTimer = 0.0f;
	float calmUntil = 0.0f;         // Calm: no attack on sight until this time
	float knockTimer = 0.0f;      // > 0: knocked down (or out) and helpless
	float castTimer = 0.0f;       // until the next spell they may cast
	int castSpell = -1;           // spell being cast (index into ActorDef::combatSpells), fires at hitAt
	bool shooting = false;        // the pending hitAt releases a projectile instead of a blow
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
	float gridUntil = -1.0f;        // World::time until which moves follow the path grid (a ledge cut the straight way)
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
	int aiRange = -1;             // wander distance (-1: the record's)
	bool aiDone = false;          // GetAIPackageDone
	bool aiRepeat = false;        // the script's reset argument: the package starts over when it finishes
	bool aiActive = true;         // AIFollow from a script waits to see its target first (OpenMW's activation)

	bool visible() const { return enabled && !pickedUp; }
	struct InventoryItemLite { int condition = -1; std::string soul; float charge = -1.0f; } dropState;
};

struct ScriptInstance
{
	const Script* script = nullptr;
	std::vector<float> locals;
	int ref = -1;                 // owning reference; -1 for global scripts
	int target = -1;              // a global script's implicit reference (ref->StartScript): its calls without one act on it
	std::string item;             // item id for scripts carried in the inventory
	bool running = true;
	bool activated = false;       // OnActivate pending for this frame
	bool suppress = false;        // its script asked OnActivate: the activation is held back until the script calls Activate (OpenMW)
	bool buffered = false;        // the player tried to activate it while its script held the activation (Activate then works)
	float lastSayEnd = 0.0f;

	float* local(const std::string& lowerName);
};

// condition: wear of weapons / armor (-1 = as new; stacks with different wear are separate entries)
// soul: soul gems holding one (the creature's id); such entries stack only with the same soul
// charge: enchanted items' charge left (-1: full)
struct InventoryItem { std::string id; int count; bool equipped; int condition = -1; std::string soul; float charge = -1.0f; };

// A spell / potion effect running on the player (magic.cpp)
struct ActiveEffect
{
	int effect, attribute, skill;
	float magnitude, remaining;       // remaining seconds
	std::string source;
	int ref = -1;                     // summons: the creature, gone when the effect ends
	std::string item, prev;           // bound items: the item given, and what it replaced
};
struct JournalLine { std::string quest; int index; std::string text; };

enum Attribute { ATTR_STRENGTH, ATTR_INTELLIGENCE, ATTR_WILLPOWER, ATTR_AGILITY, ATTR_SPEED, ATTR_ENDURANCE, ATTR_PERSONALITY, ATTR_LUCK };

struct PlayerStats
{
	std::string name = "Stranger";
	std::string race = "imperial", cls, birthsign;
	std::string head, hair;           // body part ids chosen at character creation ("" = the race's first)
	bool female = false;
	int attributes[8] = {};
	int skills[27] = {};
	int level = 1;
	float health = 50, healthMax = 50, magicka = 50, magickaMax = 50, fatigue = 100, fatigueMax = 100;
	// Growth since character creation (recomputeStats adds it on top of race / class / sign)
	float skillProgress[27] = {};
	int skillBonus[27] = {};
	int attrBonus[8] = {};
	int attrSkillUps[8] = {};          // major / minor skill increases per governing attribute this level
	float attrDamage[8] = {}, skillDamage[27] = {};   // Damage Attribute / Skill until restored
	// What lasting effects took below 0 (OpenMW keeps the modifier whole and shows max(0, base + modifier)):
	// owed back before the stat rises again, so an effect's end restores exactly what it took (recomputeStats: 0)
	int attrBelow[8] = {}, skillBelow[27] = {};
	int levelProgress = 0;             // major / minor skill increases since the last level
	float healthBonus = 0.0f;
	int skillCreation[27] = {};        // race + class skills (recomputeStats), before use, training, books, abilities and drains
	int attrCreation[8] = {};          // race + class attributes (recomputeStats), before level-up raises, abilities and drains
	std::vector<std::string> spells;   // bought / given by scripts (race and sign spells are implied)
	std::unordered_set<std::string> booksRead;   // skill books already read (lowercase ids)
	std::string selectedSpell;
	// Quick keys (ZL + D-pad up / right / down / left): "inv:<item>" or "magic:<spell or item:id>"
	std::string quickKeys[4];
	std::unordered_map<std::string, int> powerUsedDay;
};

enum { AI_IDLE = 0, AI_COMBAT = 1 };

// Menus the chargen scripts switch on one at a time
enum { MENU_STATS = 1, MENU_INVENTORY = 2, MENU_MAGIC = 4, MENU_MAP = 8, MENU_REST = 16 };

// Geometry, textures and animated NPCs of a cell that is in memory
struct LoadedCell
{
	int index = -1;               // into World::cells
	Cell cell;
	ActorSet actors;
	std::vector<std::pair<int, u32>> glowRefs;   // its enchanted things lying about and their colours (the shimmer)
};

// One cell of the level: its references stay loaded, its geometry only while the player is in or
// (outdoors) next to it. Exterior cells are 8192-unit grid squares.
struct LevelCell
{
	std::string name, file;
	bool interior = true;
	int gx = 0, gy = 0;               // exterior grid position
	int refBase = 0, refCount = 0;    // its references are refs[refBase .. refBase + refCount)
	LoadedCell* live = nullptr;
	int pathBase = 0, pathCount = 0;  // its path grid points are World::pathPoints[pathBase ..)
	bool refsLoaded = false;          // its objects are in memory (read when needed; evictCells drops far ones)
	u32 lastUsed = 0;                 // World::useTick when last read or loaded
	float sea = 0.0f;                 // exterior: share of its ground under open water (converter)
	bool noSleep = false;             // resting is illegal here (waiting only, or a bed)
	std::string region;               // exterior: its region (lowercase id)
};

float angleDelta(float a, float b);    // a - b wrapped into -pi..pi
struct Ref;
void fillContents(const GameData& g, Ref& ref, int level, unsigned seed);   // record items, leveled lists picked
// A merchant's stack with a negative count is a restocking quantity: it shows |count| and buying never depletes it
inline int stockCount(int count) { return count < 0 ? -count : count; }
// A leveled line's separate rolls: one per unit with the each flag and more than one, else one for the whole count
inline int levRolls(bool each, int count) { return each && stockCount(count) > 1 ? stockCount(count) : 1; }
// Followers go through a door with the player when within 800 units (squared distance given), and not when they stay
// outside, are outdoors now and the way leads in (OpenMW's ActionTeleport::getFollowers)
inline bool followerTaken(float dist2, bool stayOutside, bool outdoors, bool toExterior)
{
	return !(!toExterior && stayOutside && outdoors) && dist2 <= 800.0f * 800.0f;
}

// Morrowind's calendar (months of 28..31 days): what the Day / Month / Year / DaysPassed globals hold
struct GameDate
{
	int day, month, year;             // month 0 = Morning Star
	int daysPassed;
	float hour;                       // 0..24
};

// Path grid point (PGRD): where NPCs can walk, with the points it connects to
struct PathPoint
{
	float pos[3];
	std::vector<int> links;           // global indices
	int cell;
};

struct World
{
	GameData game;
	const char* dataDir = "";
	std::vector<LevelCell> cells;
	int current = -1;             // index into cells: where the player is
	std::vector<LoadedCell*> loaded;   // an interior alone, or the 3 x 3 exterior cells around the player
	TextureCache textures;
	struct StreamJob* streamJob = nullptr;   // an exterior cell loading on the streaming thread
	std::unordered_map<long long, int> gridIndex;   // exterior grid -> index into cells
	// every cell's references. A deque: it grows without moving them (a vector's doubling needed one
	// block twice the size, tens of MB, and ran the heap out after many cells were visited)
	std::deque<Ref> refs;
	std::vector<PathPoint> pathPoints;   // every cell's path grid (pathfind.cpp)
	std::vector<ScriptInstance> scripts;
	std::unordered_map<std::string, float> globals;
	std::unordered_map<std::string, int> journalIndex;
	std::vector<JournalLine> journal;
	std::vector<InventoryItem> inventory;
	std::unordered_set<std::string> knownTopics;
	// What people said about each topic (the journal's Topics index): topic name -> "Speaker: text" (saved)
	std::map<std::string, std::vector<std::string>> topicLog;
	PlayerStats stats;
	Player player;

	std::vector<ActiveEffect> effects;
	std::vector<ActiveEffect> abilities;   // race / birthsign abilities' other effects (resistances ...), for good
	float corprusSince = -1.0f;   // gameHour the player caught Corprus (-1: hasn't); it worsens by the day
	int corprusLevel() const { return corprusSince < 0.0f ? 1 : std::min(20, 1 + (int)((gameHour - corprusSince) / 24.0f)); }
	int markCell = -1;            // Mark: where Recall goes
	float markPos[3] = {}, markYaw = 0.0f;
	float lastOutside[3] = {};    // the player's last spot outdoors (Intervention from inside)
	int bounty = 0;               // crime: gold the guards want
	// Factions the player belongs to: lowercase faction id -> rank (0 lowest); reputation in each
	std::unordered_map<std::string, int> pcRank, pcFacRep;
	int pcReputation = 0;
	int pcRankIn(const std::string& faction) const      // -1 = not a member
	{
		auto it = pcRank.find(faction);
		return it == pcRank.end() ? -1 : it->second;
	}
	int arrestDeclined = -1;      // bounty the player resisted arrest at (guards attack instead of asking)
	std::unordered_set<std::string> pcExpelled;                  // factions that threw the player out
	std::unordered_set<std::string> mapKnown;                    // places on the map: visited or ShowMap (lowercase)
	std::unordered_set<int> mapCells;                            // exterior cells the player has been in or ShowMap revealed (mapCellKey)
	static int mapCellKey(int gx, int gy) { return (gx + 512) * 1024 + (gy + 512); }
	bool mapCellSeen(int gx, int gy) const { return mapCells.count(mapCellKey(gx, gy)) != 0; }
	std::unordered_map<std::string, int> factionReactions;       // "a|b" -> ModFactionReaction total
	int scriptCell = -1;          // the player's cell when scripts last ran (CellChanged)
	bool forceSneak = false;      // ForceSneak: the player crouches (scripts)
	// Force flags of the other actors (ForceRun 1, ForceJump 2, ForceMoveJump 4, ForceSneak 8): ref -> bits, -1 = the player
	std::unordered_map<int, unsigned> moveFlags;
	unsigned controlsOff = 0;     // DisablePlayerLooking 1, DisablePlayerViewSwitch 2, DisableVanityMode 4 (scripts)
	// The player's diseases (spells of type 3 common, 2 blight; Corprus carries effect 132)
	bool pcHasDisease(int type) const;
	bool pcHasCorprus() const;

	bool controlsEnabled = true;
	bool jumpingEnabled = true;
	bool fightingEnabled = false, magicEnabled = false;
	unsigned menusEnabled = 0;
	float time = 0.0f;            // seconds since the level started
	float drawScale = 1.0f;       // share of the fog distance being drawn (adaptive view, 3D)
	float gameHour = 9.0f;            // hours since midnight of the first day (runs past 24)
	GameDate date() const;
	// The GameHour / Day / Month / Year / DaysPassed globals follow gameHour; a script that sets GameHour
	// moves the clock to that hour of the same day
	void syncTime();
	float hourWritten = -1.0f;        // GameHour as syncTime last wrote it (-1: not yet)
	int dayCounted = -1;              // whole days of gameHour the Day / Month / Year / DaysPassed globals have moved for
	float timescale() const;          // game hours = real seconds x this / 3600 (the TimeScale global, 30)
	// A global set the way scripts and OpenMW's setters do: Day wraps through the months, Month clamps the day and
	// carries years, GameHour past 24 moves the day (not DaysPassed), the rest as is
	void setGlobal(const std::string& lowerName, float value);
	void restockGold(int ref);        // opening dialogue: a merchant's purse is full again after fBarterGoldResetDelay hours
	bool pcWeaponDrawn = false;       // the player's weapon is out (fDispWeaponDrawn), set by the session each frame
	void deleteRef(int ref);          // gone for good (a corpse cleared): OpenMW's deleteObject
	bool pcSleeping = false;          // GetPCSleep: resting in bed or on the ground
	bool wakeUp = false;              // WakeUpPC: a script ended the rest

	// startCell: cell to begin in ("" = the level's starting cell)
	bool load(const char* dataDir, const std::string& startCell = "");
	void keepGame();                  // hand the game data to the next session (Session::shutdown)
	// Moves the player's cell to cells[index] and loads what that needs (the interior alone, or the
	// exterior cells around it), freeing the rest; references keep their state.
	bool enterCell(int index);
	// Outdoors: follows the player across grid cells and loads missing neighbours (all at once, or one
	// per call to spread the work over frames).
	void streamExterior(bool all);
	void retryMissingTextures(float dt);    // loaded cells' textures that failed, tried again every few seconds
	float retryTimer = 0.0f;
	int cellIndex(const std::string& name) const;       // by name or file; -1 when the level doesn't include it
	int gridCell(int gx, int gy) const;                 // exterior cell at that grid position, or -1
	const std::string& cellName() const { return cells[current].name; }
	bool isStartCell() const;
	// The cell the player is in (an empty stand-in if it failed to load, so nothing crashes)
	Cell& here() { static Cell none; return current >= 0 && cells[current].live ? cells[current].live->cell : none; }
	// The cell a reference stands in: its own, or where PositionCell moved it
	int placeOf(int ref) const { return refs[ref].at >= 0 ? refs[ref].at : refs[ref].cell; }
	bool active(int ref) const { return ref >= 0 && refs[ref].cell >= 0 && cells[placeOf(ref)].live; }
	// Companions and summons: following the player (they go through doors with them)
	bool followsPlayer(int ref) const;
	bool applyLevPick(int ref, const std::string& pick);   // a leveled spot becomes that creature
	// Owned by someone else: taking it (or what it holds) is theft, sleeping in it isn't allowed
	bool ownedByOther(int ref) const;
	// What an NPC's disposition to the player is now: their own (with persuasion's changes) plus race,
	// Personality, faction reactions, disease (Morrowind's derived disposition, as OpenMW has it)
	int disposition(int ref, bool clamp = true) const;
	bool actorHasSpell(const Ref& r, const std::string& id) const;   // an NPC's or creature's spells (scripts' changes too)
	bool actorHasSpellType(const Ref& r, int type) const;                // ... of a type (2 blight, 3 common disease)
	int factionReaction(const std::string& a, const std::string& b) const;   // base + ModFactionReaction
	std::unordered_map<std::string, int> stolen;   // items the player took from owners (lowercase id -> count)
	// whom they were taken from (an NPC id or a faction): they won't buy them back
	std::unordered_map<std::string, std::vector<std::string>> stolenFrom;
	void markStolen(const std::string& item, int count, const std::string& owner);
	void confiscateStolen();                                   // PayFine / jail: the stolen goods leave the inventory
	// Weather as in OpenMW's WeatherManager: the weather now, the one it is changing to (weatherNext, the same as
	// weatherNow when none) with weatherBlend 0..1 of the way there (real seconds, the next weather's delta), and
	// one queued behind it (-1: none). Every region holds a weather (forcedWeather; rolled from its chances when
	// needed) until weatherTimer (game hours) runs out and all are forgotten
	int weatherNow = 0, weatherNext = 0, weatherQueued = -1;
	float weatherBlend = 1.0f, weatherTimer = 20.0f, weatherSeen = -1.0f;   // weatherSeen: gameHour last looked at
	int weatherCell = -1;                         // where the player was last time (walking / arriving)
	float windCur = 0.0f, windNxt = 0.0f, windNow = 0.0f;   // each weather's gusting wind speed; the one felt
	std::string weatherRegion;
	std::unordered_map<std::string, int> forcedWeather;          // region -> its weather now
	std::unordered_map<std::string, std::vector<int>> regionChances;   // ModRegion's chances
	void updateWeather(float dt);
	int rollWeather(const std::string& region);
	int regionWeather(const std::string& region);                 // the region's weather (rolled if it has none)
	void addWeatherTransition(int weather);                       // starts at once, or waits its turn
	void forceWeather(int weather);                               // at once, nothing changing or queued
	void setWeatherHere(int weather);                             // (tests) this region's weather, at once
	int regionChance(const std::string& region, int weather) const;
	// ChangeWeather: the region's weather from now, until the next expiry. ModRegion: new chances; a stored
	// weather they rule out is rerolled
	void changeWeather(const std::string& region, int weather);
	void modRegion(const std::string& region, const std::vector<int>& chances);
	float weatherFogScale() const;                // view distance factor: 1 in clear weather
	float weatherColour(int kind, int c) const;   // [sky, fog, ambient, sun] channel now (0..1), blended while changing
	float weatherFogDepth() const;                // land fog depth now
	int weatherShown() const;                     // the weather whose rain, storm and loops show (OpenMW's threshold)
	bool weatherPrecip() const;                   // rain, snow or blight falling (not an ash storm)
	bool weatherStorm() const;
	float sunVisibility() const;                  // glare view, blended as OpenMW does
	float glareFade() const;                      // 0 at night, 1 at midday
	bool sunOn() const;
	bool useTorches() const;                      // dark and not precipitating
	int nightDay() const;                         // 0 default, 1 interior in daylight, 2 exterior at night
	bool underWater(float z) const;               // below the water of a loaded cell
	bool teleportDisabled = false;
	// (no longer written: ClearInfoActor only edits the Topics index) "actor id|info uid"
	std::unordered_set<std::string> clearedInfos;
	// Set by the session: whether an actor notices the player (dialogue "Detected"), and whether it attacks on sight
	std::function<bool(int)> awarenessHook, shouldAttackHook;
	int currentInfo = -1;         // the response being said (its result script runs), and its topic's name
	std::string currentTopic;
	// ClearInfoActor: that speaker's last line about the topic leaves the Topics index
	void topicLogRemoveLast(const std::string& topic, const std::string& speaker);
	// Cells come back to life every iMonthsToRespawn months (OpenMW's reading): respawning containers
	// restock, respawning actors dead longer than fCorpseRespawnDelay hours get up again (saved)
	std::unordered_map<std::string, float> cellRespawnAt;   // cell file -> gameHour of its last refresh
	void respawnCell(int cellIndex);
	std::unordered_map<std::string, int> cellLevel;   // the player's level when a cell's objects were first read (saved)                // DisableTeleporting: no Mark / Recall / Interventions (saved)
	std::vector<std::string> madeItems;           // items the player enchanted (objects added to game.objects; saved)
	std::vector<std::string> madeSpells;          // spells the player made (ids added to game.spells; saved)
	std::vector<std::string> brewed;              // potions the player made (ids of objects added to game.objects; saved)
	// Moves a reference to a spot in another (or the same) cell: PositionCell / Position
	void relocate(int ri, int cell, const float pos[3], float yaw);
	void detachActor(int ri);         // its mesh and collision cylinder leave the cell drawing them

	// Objects of cells that aren't in memory: what changed in each (save-file items), applied when the
	// cell's objects are read again. Saves read and write the same thing, so loading a save doesn't read
	// every cell it mentions, and evictCells can drop cells the player left far behind.
	std::unordered_map<std::string, std::string> cellStates;   // cell file -> {"refs": [...], "scripts": [...]}
	std::unordered_map<std::string, int> deadCounts;           // lowercase id -> killed (GetDeadCount)
	std::vector<std::pair<int, int>> freeRefs, freePaths;      // slot ranges evicted cells left (first, count)
	u32 useTick = 0;
	// References placed while playing (PlaceAtPC / PlaceAtMe): outside any cell's range, drawn while
	// their cell is loaded; they go when their cell's objects leave memory (and aren't saved)
	std::vector<int> spawned;
	// Placed creatures of cells whose objects left memory (and from a save): placed again when the cell is read
	struct PendingSpawn { std::string id, cell; float pos[3], yaw, health; bool dead; };
	std::vector<PendingSpawn> pendingSpawns;
	// Dropped items: references drawn with the item's own mesh (fp pieces) while their cell is in memory;
	// kept here (and in saves) while it isn't
	struct DroppedItem { InventoryItem item; std::string cell; float pos[3], yaw; };
	std::vector<DroppedItem> droppedWaiting;
	int dropItem(const InventoryItem& it, const float pos[3], float yaw, int cell = -1);
	// In front of a spot on the floor there (dist units along yaw)
	int dropItemAt(const InventoryItem& it, const float base[3], float yaw, float dist);
	int spawnActor(const std::string& id, const float pos[3], float yaw, int cell = -1);
	void despawn(int ri);             // a placed reference goes (a summons ends)
	// Day and night outdoors: the tint on baked lighting, the sky's and the fog colour for this hour
	void daylight(float land[3], float sky[3], float fog[3]) const;
	// Places `count` of an id `dist` in front of (dir 0), behind (1), left of (2) or right of (3) a spot
	void placeAt(const std::string& id, int count, float dist, int dir, const float base[3], float yaw);
	void snapToFloor(float pos[3]);     // down onto the floor below (from a little above), never up
	void attachSpawned(int ri);       // its mesh and collision in its (loaded) cell
	int refsInMemory() const;
	// Moves the objects of the cells used longest ago (not loaded, nothing pointing into them: inUse)
	// out of memory until at most `budget` remain; returns how many cells went
	int evictCells(int budget, const std::function<bool(int)>& inUse);
	void evictCell(int cell);
	// save.cpp: one object's save item, its hash (baseHash when unchanged), applying an item; a script
	// instance's item; a cell's changed objects and scripts as text, and applying stored text
	cJSON* refState(int i) const;
	u32 refHash(int i) const;
	void applyRefState(Ref& r, const cJSON* item);
	cJSON* scriptState(const ScriptInstance& s) const;
	std::string cellStateText(int cell) const;
	void applyCellState(int cell);
	void rebuildScriptIndex();
	bool sameSpace(int a, int b) const;                 // both in one interior, or both outdoors
	Cell* cellOf(int ref) { return cells[placeOf(ref)].live ? &cells[placeOf(ref)].live->cell : nullptr; }
	ActorSet* actorsOf(int ref) { return cells[placeOf(ref)].live ? &cells[placeOf(ref)].live->actors : nullptr; }
	Actor* actorOf(int ref);
	Scene scene();                                      // collision of everything loaded, for movement
	// Actors, swinging doors and usable objects of the loaded cells, rebuilt when cells load or unload
	// (per-frame loops use these instead of every loaded reference)
	std::vector<int> loadedActors, loadedDoors, loadedPickables;
	void rebuildLoadedLists();
	template <typename Fn> void forLoadedActors(Fn fn)
	{
		for (size_t k = 0; k < loadedActors.size(); k++)
			fn(loadedActors[k]);
	}
	// Calls fn(ref index) for every reference of the loaded cells
	template <typename Fn> void forLoadedRefs(Fn fn)
	{
		for (size_t k = 0; k < loaded.size(); k++)
		{
			const LevelCell& c = cells[loaded[k]->index];
			for (int i = c.refBase; i < c.refBase + c.refCount; i++)
				if (refs[i].at < 0)
					fn(i);
		}
		for (size_t k = 0; k < spawned.size(); k++)
			if (refs[spawned[k]].cell >= 0 && cells[placeOf(spawned[k])].live)
				fn(spawned[k]);
	}
	void freeAll();
	// Re-hides disabled / taken references of the current cell (after their state changed wholesale)
	void refreshHidden();
	void syncActor(int ref);
	// Save games (save.cpp): everything that changes while playing, as JSON
	bool saveGame(const char* path) const;
	static std::string savedCell(const char* path);     // "" when there is no usable save
	bool applySave(const char* path);                   // after load(dataDir, savedCell(path))
	int findRef(const std::string& id) const;            // by object id, case-insensitive; loaded cells first
	int findRefAnywhere(const std::string& id);          // also reads cells that hold it (scripts)
	bool ensureRefs(int cell);                           // reads a cell's objects if not yet
	int findActorRef(int actor) const;
	void setEnabled(int ref, bool enabled);
	void pickUp(int ref);
	int itemCount(const std::string& id) const;
	void addItem(const std::string& id, int count);
	void addStack(const InventoryItem& item);           // a whole item (a soul gem with its soul) joins the stack it fits
	int removeItem(const std::string& id, int count);   // how many were removed
	// The same for a container's or an actor's own contents (AddItem / RemoveItem / GetItemCount on them)
	int refItemCount(const Ref& r, const std::string& id) const;
	void refAddItem(Ref& r, const std::string& id, int count);
	void refAddStack(Ref& r, const InventoryItem& item);   // a whole item, its wear / soul / charge kept (OpenMW's stacks())
	void takeStack(const ContentItem& c, int count);       // count of a container's stack into the inventory, state and all
	int refRemoveItem(Ref& r, const std::string& id, int count);
	bool trapSoul(const std::string& creature, int soul);   // fills the smallest gem that holds it
	int soulValue(const InventoryItem& it) const;
	void setJournal(const std::string& quest, int index);        // the test harness: the stage, written, exactly
	// Journal quest index: writes the stage's entry (once), raises the index, finished / restart. True when the
	// player is told the journal changed
	bool journalAdd(const std::string& quest, int index);
	std::unordered_set<std::string> questFinished;               // lowercase quest ids (saved)
	int getJournal(const std::string& quest) const;
	void recomputeStats();
	void refreshStats();                          // recomputeStats keeping current health / magicka / fatigue
	const SpellDef* enchantmentOf(const InventoryItem& it) const;
	float chargeOf(const InventoryItem& it) const;   // what's left (full when never used)
	float effectTotal(int effect) const;                // sum of an active effect's magnitudes
	float actorEffect(int ref, int effect) const;       // an actor's: its abilities' constant effects + the spells on it
	void reapplyEffects();
	const RaceDef* race() const;
	const ClassDef* playerClass() const;
	const BirthDef* birthsign() const;
	float distanceToPlayer(int ref) const;
	// pathfind.cpp
	bool lineOfSight(const float a[3], const float b[3]);
	int nearestPathPoint(const float p[3]);
	bool findPath(const float from[3], const float to[3], std::vector<int>& out);   // path point indices
	void update(float dt);

	// Script events raised on objects (scripts see them through OnActivate / OnPCAdd / OnPCEquip)
	void startScriptFor(int ref);
	int startGlobalScript(const std::string& name, int target = -1);
	// Enable / Disable of a reference whose cell's objects aren't read yet (Startup disables ~150 of
	// them across the island): kept by id and applied when the cell is read (saved)
	std::unordered_map<std::string, bool> lazyEnabled;
	bool deferEnable(const std::string& id, bool on);   // true: remembered, nothing read
	void stopGlobalScript(const std::string& name);
	void setItemScriptLocal(const std::string& itemId, const char* local, float value);
};

// Nearest activatable reference along the view ray within reach, or -1.
int worldPick(const World& w, const float eye[3], const float dir[3], float reach);
// Whether the crosshair can find an actor where it stands: a look from 100 units off (on the player's side)
// at its middle (worked out from its position) meets its box; what stands in front of it doesn't count
bool actorAimable(const World& w, int ri);
