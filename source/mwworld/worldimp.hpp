#pragma once

#include <algorithm>
#include <functional>
#include <string>
#include <unordered_map>
#include <map>
#include <unordered_set>
#include <deque>
#include <vector>

#include "mwrender/animation.hpp"
#include "mwrender/objects.hpp"
#include "mwworld/esmstore.hpp"
#include "mwworld/player.hpp"
#include "mwworld/containerstore.hpp"
#include "mwmechanics/aipackage.hpp"
#include "mwworld/livecellref.hpp"
#include "mwscript/locals.hpp"
#include "mwmechanics/activespells.hpp"
#include "mwdialogue/journalentry.hpp"
#include "mwmechanics/npcstats.hpp"
#include "mwworld/cellstore.hpp"
#include "mwworld/datetimemanager.hpp"
#include "mwmechanics/pathgrid.hpp"

// Menus the chargen scripts switch on one at a time
enum { MENU_STATS = 1, MENU_INVENTORY = 2, MENU_MAGIC = 4, MENU_MAP = 8, MENU_REST = 16 };

float angleDelta(float a, float b);    // a - b wrapped into -pi..pi
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

struct World
{
	GameData game;
	const char* dataDir = "";
	std::vector<LevelCell> cells;
	int current = -1;             // index into cells: where the player is
	std::vector<LoadedCell*> loaded;   // an interior alone, or the 3 x 3 exterior cells around the player
	TextureCache textures;
	struct StreamJob* streamJob = nullptr;   // an exterior cell loading on the streaming thread
	bool streamPaused = false;               // tests (STREAM:off): no neighbour streams in; the player's own cell still loads
	bool streamThreaded = false;             // a cell has streamed in on a thread (the console, the emulator; never the PC build)
	std::unordered_map<long long, int> gridIndex;   // exterior grid -> index into cells
	// every cell's references. A deque: it grows without moving them (a vector's doubling needed one
	// block twice the size, tens of MB, and ran the heap out after many cells were visited)
	std::deque<Ref> refs;
	std::vector<PathPoint> pathPoints;   // every cell's path grid (mwmechanics/pathfinding.cpp)
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
	std::vector<ActiveEffect> abilitiesShown;   // every effect of the abilities and worn constant effects (the HUD's icons)
	float corprusSince = -1.0f;   // gameHour the player caught Corprus (-1: hasn't); it worsens by the day
	int corprusLevel() const { return corprusSince < 0.0f ? 1 : std::min(20, 1 + (int)((gameHour - corprusSince) / 24.0f)); }
	int markCell = -1;            // Mark: where Recall goes
	float markPos[3] = {}, markYaw = 0.0f;
	float lastOutside[3] = {};    // the player's last spot outdoors (Intervention from inside)
	int bounty = 0;               // crime: gold the guards want
	// OpenMW's crime ids (Player::getNewCrimeId / recordCrimeId): the last crime's, and the last one paid off when the bounty
	// went to 0 (a fine, jail, PayFine, SetPCCrimeLevel 0). Saved
	int crimeIdNow = -1, crimeIdPaid = -1;
	void crimePaid() { crimeIdPaid = crimeIdNow; }
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
	// Interiors' fog of war: kMapFog x kMapFog squares over the local map, 0 seen .. 15 never seen, by lowercase cell
	// name (saved)
	static constexpr int kMapFog = 32;
	std::unordered_map<std::string, std::vector<u8>> mapFog;
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
	u32 frame = 0;                // World::update calls (actor posing's cadence)
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
	int loadCellsNear(const float pos[3], float reach);   // the unloaded neighbours within reach of pos, now (behind a loading screen)
	void retryMissingTextures(float dt);    // loaded cells' textures that failed, tried again every few seconds
	float retryTimer = 0.0f;
	int cellIndex(const std::string& name) const;       // by name or file; -1 when the level doesn't include it
	int gridCell(int gx, int gy) const;                 // exterior cell at that grid position, or -1
	std::string cellName() const { return cells[current].name; }
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
	float weatherFogScale() const;                // view distance factor: 1 (the weather moves the fog's start)
	float weatherFogStart() const;                // land fog start / end outdoors (OpenMW: 1 - fog depth); < -1.5 none
	float weatherColour(int kind, int c) const;   // [sky, fog, ambient, sun] channel now (0..1), blended while changing
	float weatherFogDepth() const;                // land fog depth now
	float underwaterFog() const;                  // how thick the water is to the eye now (fog starts 1 - this of the view)
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
	bool levitationDisabled = false;  // DisableLevitation (Tribunal's Mournhold): Levitate ends at once, saved (OpenMW's LEVT)
	// SetWaterLevel / ModWaterLevel: an interior's water moved by a script (lowercase cell name -> height), saved and
	// laid on the cell each time it is read (Tribunal's sewers)
	std::unordered_map<std::string, float> waterLevels;
	std::unordered_set<std::string> startScriptsDone;   // the data's start scripts this game has run (saved)
	// The player's own notes on the map (Tribunal's: sEditNote / sDeleteNote): an interior's (its lowercase name) or the
	// outdoors' (cell ""), at a world spot (saved)
	struct MapNote { std::string cell; float x = 0.0f, y = 0.0f; std::string text; };
	std::vector<MapNote> mapNotes;
	float scriptWaterLevel() const;                // GetWaterLevel: the player's cell (0 outdoors, very low with no water)
	void setScriptWaterLevel(float z);             // the player's interior's water to that height
	bool aiEnabled = true;            // ToggleAI (the console): off, actors stand still
	// (no longer written: ClearInfoActor only edits the Topics index) "actor id|info uid"
	std::unordered_set<std::string> clearedInfos;
	// Set by the session: whether an actor notices the player (dialogue "Detected"), and whether it attacks on sight
	std::function<bool(int)> awarenessHook, shouldAttackHook;
	// Set by the session: give heap back now (force: not rate limited, the player's own cell is waiting for it); a neighbour
	// whose load failed on memory waits until streamRetryAt (osGetTime) to be tried again
	std::function<void(bool)> memoryLowHook;
	u64 streamRetryAt = 0;
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
	// every cell it mentions, and evictCells can drop cells the player left far behind. Kept compressed, as
	// a save writes them: a save copies them as they are, not put together and compressed again each time
	struct StoredCell
	{
		std::vector<u8> z;          // {"refs": [...], "scripts": [...]}, zlib
		u32 raw = 0;                // its length unpacked
		bool movedRefs = false;     // holds references moved to another cell ("at"): read on loading
	};
	std::unordered_map<std::string, StoredCell> cellStates;   // cell file -> its changes
	void storeCellState(const std::string& file, const std::string& json);
	std::string cellStateJson(const StoredCell& s) const;     // "" when it won't unpack
	std::unordered_map<std::string, int> deadCounts;           // lowercase id -> killed (GetDeadCount)
	std::vector<std::pair<int, int>> freeRefs, freePaths;      // slot ranges evicted cells left (first, count)
	// findRef's index: (interned lower-case id, slot), sorted; rebuilt on the next lookup after references came, went or
	// changed id (refsChanged)
	mutable std::vector<std::pair<const char*, int>> refIndex;
	mutable bool refIndexStale = true;
	void refsChanged() { refIndexStale = true; }
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
	// The light from the side outdoors (shaders/cell.v.pica sideLight): rgb the sun's colour now on a wall facing it, w +1 when it
	// comes from the east, -1 from the west; zeros indoors
	void sunSide(float out[4]) const;
	// Places `count` of an id `dist` in front of (dir 0), behind (1), left of (2) or right of (3) a spot
	void placeAt(const std::string& id, int count, float dist, int dir, const float base[3], float yaw);
	// PlaceItem / PlaceItemCell: a new object of that id at that spot (cell -1: where the spot is); a person or
	// creature, an item lying there, or any other object with its script running. Its ref, or -1
	int placeObject(const std::string& id, int cell, const float pos[3], float yaw);
	void loseCollision(int ref);                    // its collision gone for good, the object still drawn (saved: colGone)
	void regainCollision(int ref);                  // back again (a Clockwork City door shut after it was opened)
	void snapToFloor(float pos[3]);     // down onto the floor below (from a little above), never up
	// How far above where it stands (the ground under its feet) a creature's model is drawn: its collision box rested on
	// the highest ground under all of it (0 without a box)
	float actorBoxLift(int ri);
	void attachSpawned(int ri);       // its mesh and collision in its (loaded) cell
	int refsInMemory() const;
	// Moves the objects of the cells used longest ago (not loaded, nothing pointing into them: inUse)
	// out of memory until at most `budget` remain; returns how many cells went
	// now: not sparing the cells a script asked for in the last seconds (a test's EVICT: the native runs' clock races)
	int evictCells(int budget, const std::function<bool(int)>& inUse, bool now = false);
	// Out of memory with nothing else to give (Session::relieveMemory): the farthest loaded neighbour's
	// geometry goes — its collision arrays are the large single blocks a cell load needs — and the cell
	// streams back in when there is room. False indoors or with nothing to drop
	bool dropFarthestLiveNeighbour();
	// Waits for the cell streaming in the background and takes it in (before anything that can't have it running: a clean-up)
	void waitForStream();
	void evictCell(int cell);
	// mwstate/statemanagerimp.cpp: one object's save item, its hash (baseHash when unchanged), applying an item; a script
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
	// Moved / turned objects' shapes as boxes where the level placed them (World::update moves them with the object;
	// worked out again for another object in that slot or a new scale), while their cell is loaded
	struct MoverShape { int cell = -1, mesh = -1; float scale = 0.0f; std::vector<CollisionBox> boxes; };
	std::unordered_map<int, MoverShape> moverShapes;
	// Whether feet at f rest on this object (GetStandingPC / GetStandingActor; OpenMW's getActorStandingOn)
	bool feetOnObject(int ref, const float* f) const;
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
	// Save games (mwstate/statemanagerimp.cpp): everything that changes while playing, as JSON; the cells out of memory follow it as
	// the compressed blocks they are kept in (version 2). legacy: version 1, every cell inside the JSON (tests: the
	// old saves still load)
	bool saveGame(const char* path, bool legacy = false) const;
	static std::string savedCell(const char* path);     // "" when there is no usable save
	bool applySave(const char* path);                   // after load(dataDir, savedCell(path))
	std::string saveMissingAddon(const char* path) const;   // the add-on the save needs that the data lacks, or ""
	int findRef(const std::string& id) const;            // by object id, case-insensitive; loaded cells first
	int findRefAnywhere(const std::string& id);          // also reads cells that hold it (scripts)
	bool ensureRefs(int cell);                           // reads a cell's objects if not yet
	// A game that starts past character creation: what its scripts leave behind is set on each object as its cell is read in
	// (the Census Office's inner door unlocked, the people's "state" at the end), without reading cells ahead of the player
	bool postChargen = false;
	void postChargenRef(Ref& r);
	int findActorRef(int actor) const;
	void setEnabled(int ref, bool enabled);
	void pickUp(int ref);
	int itemCount(const std::string& id) const;
	void addItem(const std::string& id, int count);
	void addStack(const InventoryItem& item);           // a whole item (a soul gem with its soul) joins the stack it fits
	int removeItem(const std::string& id, int count);   // how many were removed
	void itemLeft(const std::string& id);               // the last one carried gone (any way): its script stops
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
	// mwmechanics/pathfinding.cpp
	// (doors false: shut doors don't count: for walking, where a door is opened on the way, and for the test driver)
	bool lineOfSight(const float a[3], const float b[3], bool doors = true);
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
// A body lies on the floor: the crosshair meets a dead actor's box only up to this far over its foot (worldPick)
constexpr float kCorpsePickHeight = 40.0f;
// A vector turned by `angle` about a door's own swing axis (its local up, tilted with the door: a slave pod's lid)
void doorSwing(const Ref& r, float angle, const float in[3], float out[3]);
// Whether the crosshair can find an actor where it stands: a look from 100 units off (on the player's side)
// at its middle (worked out from its position) meets its box; what stands in front of it doesn't count
bool actorAimable(const World& w, int ri);
