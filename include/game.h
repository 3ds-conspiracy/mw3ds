#pragma once

#include "uitheme.h"

#include <3ds/types.h>

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ---- Script syntax tree (built by tools/mwscript.py, see there for the JSON shapes)

enum NodeKind : unsigned char
{
	N_NUM, N_STR, N_LOCAL, N_GLOBAL, N_REMOTE, N_CALL, N_OP, N_NEG,
	N_SET, N_IF, N_WHILE, N_RET, N_BLOCK,
};

enum OpKind : unsigned char { OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE, OP_IDIV };

struct Node
{
	NodeKind kind = N_BLOCK;
	unsigned char op = 0;
	int func = 0;              // N_CALL: function id (script.cpp); N_LOCAL: slot or -1 = look up by name
	float num = 0.0f;
	std::string a, b;          // N_STR text; N_LOCAL/N_GLOBAL name; N_REMOTE ref + var; N_CALL ref + name
	std::vector<Node> kids;    // N_CALL args; N_OP/N_NEG operands; N_SET target, value;
	                           // N_IF cond, block, cond, block, ..., [else block]; N_WHILE cond, block
};

struct Script
{
	std::string name;
	std::vector<std::string> localNames;
	std::vector<char> localTypes;        // 's', 'l', 'f'
	Node body;
	bool usesOnActivate = false;         // activation is handed to the script instead of the default action
	bool scriptedHits = false;           // HitOnMe / SetHealth: the script runs the fight (the Heart of Lorkhan)
	int localIndex(const std::string& lowerName) const;
};

// ---- Records

// range: 0 self, 1 touch, 2 target
struct SpellEffect { int effect, skill, attribute, min, max, duration, range; int area = 0; };   // area: feet around where it lands

struct Object
{
	std::string id, type, name, script, icon, text, sound, openSound, closeSound;
	int iconIx = -1;                                   // item icon (ui/icons_*.t3x), -1 = none
	float weight = 0.0f;
	int value = 0, subtype = -1, armor = 0, flags = 0;
	bool scroll = false;
	bool respawn = false;                // containers: their contents come back (World::respawnCell)
	bool organic = false;                // containers: plants and the like (no chest sound when opened)
	float capacity = -1.0f;              // containers: the weight they hold (-1: data from before it was exported, no limit)
	int lightRadius = 0;                 // lights: how far they reach, and their colour (0..255)
	u8 lightColor[3] = { 0, 0, 0 };
	bool magic = false;                                // enchanted
	std::vector<std::pair<int, std::string>> items;    // container / NPC contents: count, id
	std::vector<SpellEffect> effects;                  // potions, ingredients (first effect)
	// weapons
	u8 chop[2] = {}, slash[2] = {}, thrust[2] = {};    // min, max damage
	float speed = 1.0f, reach = 1.0f;
	int health = 0;                                    // weapons / armor: condition when new (0 = doesn't wear)
	int uses = 0;                                      // repair tools
	float quality = 0.0f;
	int bookSkill = -1;                                // skill books: the skill the first read raises
	std::string ench;                                  // enchanted: its enchantment (a SpellDef id)
	std::string iconBase;                              // enchanted by the player: the item it was made from
	int enchant = 0;                                   // enchantment capacity
	struct IngredientEffect { int effect, skill, attribute; };
	std::vector<IngredientEffect> ingredient;          // ingredients: their (up to) four effects, for alchemy
};

// Weapon types (WPDT)
enum { WEAP_SHORT_BLADE = 0, WEAP_BOW = 9, WEAP_CROSSBOW = 10, WEAP_THROWN = 11, WEAP_ARROW = 12, WEAP_BOLT = 13 };
enum { ARMO_SHIELD = 8 };

struct Cond
{
	char kind;          // '1' function, '2' global, '3' local, '4' journal, '5' item, '6' dead, 'C' not local
	int func;           // function code for kind '1'
	std::string name;   // lowercase variable / journal / item name
	char op;            // '0' ==, '1' !=, '2' >, '3' >=, '4' <, '5' <=
	float value;
};

struct Info
{
	std::vector<int> actors;          // older data: the actors that may say it
	// Who may say it (lowercase; empty / -1 = anyone): speaker id, race, class, faction ("ffff": none),
	// cell name prefix, sex, least rank in the faction
	std::string whoId, whoRace, whoClass, whoFaction, whoCell;
	int whoSex = -1, whoRank = -1;
	std::string text, sound;
	std::vector<Cond> conds;
	int disposition = 0;
	Node script;
	int uid = -1;                     // its number in load order (ClearInfoActor remembers it)
};

enum TopicType { TOPIC_TOPIC = 0, TOPIC_VOICE = 1, TOPIC_GREETING = 2, TOPIC_PERSUASION = 3 };

struct Topic
{
	std::string name, lower;
	int type;
	std::vector<Info> infos;
};

struct JournalEntry { int index; std::string text; bool finished; bool restart = false; };
struct Journal { std::string name, title; std::vector<JournalEntry> entries; };

struct ClassDef
{
	std::string id, name, desc;
	int attributes[2], specialization, major[5], minor[5];
	bool playable;
};

struct RaceDef
{
	std::string id, name, desc;
	std::vector<std::pair<int, int>> skillBonus;   // skill, bonus
	int attributes[8][2];                         // [attribute][male, female]
	bool playable, beast;
	std::vector<std::string> spells;
};

struct BirthDef { std::string id, name, desc, texture; std::vector<std::string> spells; };
// type: 0 spell, 1 ability, 2 blight, 3 disease, 4 curse, 5 power
// type: 0 spell, 1 ability, 2 blight, 3 disease, 4 curse, 5 power; enchantments (game.json "enchantments")
// are 10 + their type: 10 cast once, 11 on strike, 12 when used, 13 constant; charge: theirs when full
enum { ENCH_ONCE = 10, ENCH_STRIKE = 11, ENCH_USE = 12, ENCH_CONSTANT = 13 };
extern u32 g_gameGeneration;           // bumped whenever game data is loaded or handed on: caches of pointers into it
struct SpellDef { std::string id, name; int type, cost = 0, flags = 0, charge = 0; std::vector<SpellEffect> effects; };
struct MagicEffectDef { int school; float cost; int flags = 0; std::string name; std::string cvfx, bvfx, hvfx, avfx; std::string snd[4]; u32 rgb = 0xFFFFFF; };   // snd: cast, bolt, hit, area (empty: the school's)   // flags: 0x4 no duration, 0x8 no magnitude, 0x10 harmful      // school: 0 alteration, 1 conjuration, 2 destruction,
                                                        // 3 illusion, 4 mysticism, 5 restoration
struct SkillDef { int attribute, specialization; std::string desc; float use[4]; };   // use: progress per kind of use
struct SoundDef { std::string file; float volume; int minRange, maxRange; };

// Merchant services (NPC AIDT flags): which item types they trade
enum
{
	SERVICE_WEAPON = 0x1, SERVICE_ARMOR = 0x2, SERVICE_CLOTHING = 0x4, SERVICE_BOOKS = 0x8,
	SERVICE_INGREDIENTS = 0x10, SERVICE_PICKS = 0x20, SERVICE_PROBES = 0x40, SERVICE_LIGHTS = 0x80,
	SERVICE_APPARATUS = 0x100, SERVICE_REPAIR_ITEMS = 0x200, SERVICE_MISC = 0x400, SERVICE_POTIONS = 0x2000,
	SERVICE_BARTER = 0x27FF,
};

struct TravelDest
{
	std::string cell;                    // display name
	bool hasGrid = false;                // outdoors: grid cell to load
	int grid[2] = {};
	float pos[3], rot[3];
};

struct ActorDef
{
	std::string id, name, race, cls, faction, script;
	int rank, female, level, disposition, reputation;
	int attributes[8] = { 40, 40, 40, 40, 40, 40, 40, 40 };
	int skills[27] = {};
	int health = 50, fatigue = 200, gold = 0, fight = 30;
	unsigned services = 0;
	std::string weapon;                  // lowercase WEAP id they fight with, "" = hand-to-hand
	float armor = 0.0f;                  // armor rating
	std::vector<TravelDest> travel;      // caravaner / boatman destinations inside the level
	int wander = 0;                      // AI_W distance: strolls this far from their spot
	// First AI package when it isn't wandering (AIPKG_*, -1 none): target id, destination, hours
	int aiPackage = -1;
	std::string lib;                     // mesh library (drawing it in a cell it wasn't placed in)
	int skel = -1;                       // shared skeleton index
	std::string aiTarget;
	float aiDest[3] = {}, aiDuration = 0.0f;
	bool creature = false;
	std::vector<std::string> spells;     // spell merchants: what they sell (lowercase ids)
	std::vector<std::pair<int, int>> attack;   // creatures: damage min, max of each attack
	int magicka = 0, flee = 0;           // AI flee rating (0..100)
	int alarm = 0;                       // AI alarm (0..100): how readily they report crimes
	int blood = 0;                                     // 0 red, 1 skeleton (white), 2 metal (gold sparks)
	u8 afloat = 0;                       // creatures: 0x10 swims, 0x20 flies (they don't fall to the floor)
	bool respawn = false, essential = false;   // come back after dying / their death severs the prophecy
	bool autocalc = false;               // their spells are the engine's pick (GameData::autoCalcSpells)
	int soul = 0;                        // creatures: soul size (Soultrap fills a gem that holds it)
	int resistNormal = 0;                // percent: blows from ordinary weapons do that much less (ghosts)
	int hello = 30;                      // AI Hello: greets within hello x iGreetDistanceMultiplier (0: never)
	std::string ammo, shield;            // lowercase ids: arrows / bolts for their bow, the shield they carry
	std::vector<std::string> combatSpells;   // lowercase: harmful spells they cast at the player, heals
	std::vector<std::string> diseases;       // lowercase: diseases their blows can pass on
	std::vector<std::pair<int, float>> constEffects;   // effect, magnitude: resistances, shields ... their abilities keep on them
};

// First-person view model parts (tools/firstperson.py): data/fp/<piece>.fpm files
struct FpItem
{
	std::unordered_map<int, std::pair<std::string, std::string>> slots;   // body part slot -> male, female piece
	std::string model;                   // the item's own mesh (arrows in flight)
};
// Third-person pieces (tools/firstperson.py third_person): [female] race -> slot -> piece, the heads and
// hairs to choose from (body id, piece), and item -> slot -> (male, female piece)
// The sun, the moons and the stars (level.py "sky_bodies"; Morrowind.ini [Moons])
struct MoonDef { float size = 50, fadeInStart = 14, fadeInFinish = 15, fadeOutStart = 7, fadeOutFinish = 10,
	axisOffset = 35, speed = 0.5f, dailyIncrement = 1; };
struct SkyBodiesDef
{
	std::string sun, glare, stars, masser[8], secunda[8];
	MoonDef moons[2];
	float sunrise = 6, sunset = 18;
};
// Pictures (tools/artassets.py): loading screens, the level-up art by class, book pictures by path
struct ArtRef { std::string file; int w = 0, h = 0; };
struct ArtDef
{
	std::vector<ArtRef> splash;
	std::unordered_map<std::string, ArtRef> levelup, bookart, hud;
};
struct ThirdPersonDef
{
	std::unordered_map<std::string, std::unordered_map<int, std::string>> races[2];
	std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> heads[2], hairs[2];
	std::unordered_map<std::string, std::unordered_map<int, std::pair<std::string, std::string>>> items;
};
struct FirstPersonDef
{
	ThirdPersonDef third;
	std::unordered_map<std::string, std::string> vfx;   // spell visual static id -> piece
	std::unordered_map<std::string, std::unordered_map<int, std::string>> races[2];   // [female] race -> slot -> piece
	std::unordered_map<std::string, FpItem> items;                                   // lowercase item id
};

struct GlobalDef { char type; float value; };
struct FactionDef
{
	std::string name;
	std::vector<std::string> ranks;
	int attrs[2] = { -1, -1 };               // favoured attributes
	int reqs[10][5] = {};                    // per rank: attribute 1, attribute 2, primary skill, favoured skill, reputation
	std::vector<int> skills;                 // favoured skills
	std::unordered_map<std::string, int> reactions;   // lowercase faction -> how this one feels about it
};
struct QuizQuestion { std::string question, answers[3], sound; };
struct MapLabel { std::string text; float x, y; };
// A piece of the 2x-detail map: its own area is (x, y, w, h) in the detail picture's pixels, found at
// (ox, oy) in its texture (a pixel of margin on the sides that touch another tile)
struct MapTile { std::string file; int x = 0, y = 0, w = 0, h = 0, ox = 0, oy = 0, texWidth = 0, texHeight = 0; };
struct MapDef
{
	std::string file;                    // "" = no map
	float originX = 0, originY = 0, unitsPerPixel = 128;
	int width = 0, height = 0, texWidth = 0, texHeight = 0;
	std::vector<MapLabel> labels;
	float detailUnitsPerPixel = 0;       // the tiles' scale (0 = no detail map)
	int detailWidth = 0, detailHeight = 0;
	std::vector<MapTile> tiles;
};

struct CellDef
{
	std::string name, file;
	bool interior;
	int gx = 0, gy = 0;
	float sea = 0.0f;
	bool noSleep = false;         // CELL flag: resting here is illegal (only waiting, or in a bed)
	std::string region;           // exterior: its region (lowercase id)
};

// A weather (Morrowind.ini [Weather X]): colours [sky, fog, ambient, sun][sunrise, day, sunset, night],
// fog depth by day / night (thicker = shorter view), its ambient loop sound
struct WeatherType { std::string name; float colors[4][4][3] = {}; float fog[2] = { 0.69f, 0.69f }; std::string sound;
	// the rest of what OpenMW reads for it (weather.cpp): how fast the change to it goes (per real second), wind,
	// sun glare when looked at, rain threshold, thunder, clouds maximum, rain / snow falling
	float delta = 0.015f, wind = 0.0f, glare = 1.0f, rainThreshold = 0.5f, thunderFreq = 0.0f, thunderThreshold = 0.0f,
		flashDecrement = 0.0f, cloudsMax = 1.0f;
	bool precip = false; };
// The day's boundaries (Morrowind.ini [Weather]) and, per colour channel [sky, fog, ambient, sun], the hours it
// leads / lags them: before sunrise, after sunrise, before sunset, after sunset
struct WeatherClock { float sunrise = 6.0f, sunset = 18.0f, sunriseDur = 2.0f, sunsetDur = 2.0f;
	float win[4][4] = { { 0.5f, 1.0f, 1.5f, 0.5f }, { 0.5f, 1.0f, 2.0f, 1.0f }, { 0.5f, 2.0f, 1.0f, 1.25f }, { 0.0f, 0.0f, 1.0f, 1.25f } }; };
enum { WEATHER_CLEAR, WEATHER_CLOUDY, WEATHER_FOGGY, WEATHER_OVERCAST, WEATHER_RAIN, WEATHER_THUNDER, WEATHER_ASH,
	WEATHER_BLIGHT, WEATHER_SNOW, WEATHER_BLIZZARD };
struct RegionDef { std::string name; int chances[10] = {}; std::string sleep;   // sleep: its sleep creatures' leveled list
	std::vector<std::pair<std::string, int>> ambient; };                        // sounds heard outdoors, chance %

// A creature / NPC scripts place at run time (PlaceAtPC / PlaceAtMe): its actor entry, mesh library,
// skeleton
struct SpawnDef { int actor; std::string lib, type; int skeleton; };

struct GameData
{
	// Morrowind's automatic spell choice (OpenMW's autocalcspell.cpp): for NPCs flagged autocalc, the
	// Autocalc spells their Intelligence can cast three times, their attributes / skills allow and they
	// cast at fAutoSpellChance, at most iAutoSpell<School>Max a school (the dearest kept); for a new
	// player the PC Start spells at fAutoPCSpellChance
	std::vector<std::string> autoCalcSpells(const int* attributes, const int* skills, bool player) const;
	std::unordered_map<std::string, SpawnDef> spawns;       // lowercase id
	std::vector<std::vector<float>> divineMarkers, templeMarkers;   // x, y, z, yaw: Intervention destinations
	// Jail: prison markers in towns; a released player is sent to the marker's destination
	struct PrisonMarker { float pos[3]; std::string cell; float dest[4]; };
	std::vector<PrisonMarker> prisonMarkers;
	// Weather colours (sky, fog, ambient, sun) at sunrise, day, sunset, night: day and night outdoors
	float weather[4][4][3] = {};
	bool hasWeather = false;
	std::vector<WeatherType> weatherTypes;       // in the regions' chance order
	float weatherHours = 20.0f;                   // game hours between weather rolls
	WeatherClock weatherClock;
	std::unordered_map<std::string, RegionDef> regions;   // lowercase id
	// Leveled creature lists: entries (level, creature or list), any entry up to the level (all) or only
	// the highest such, the chance (percent) of nothing
	struct LeveledList { bool all = false, each = false; int none = 0; std::vector<std::pair<int, std::string>> entries; };
	std::unordered_map<std::string, LeveledList> leveled;
	// A creature id for that player level ("" = none); seed: a pick that comes out the same each time
	std::string pickLeveled(const std::string& list, int level, unsigned* seed = nullptr) const;
	// The entries (positions in the list) a roll can land on once chance-none has passed (OpenMW's candidates)
	static void levCandidates(const LeveledList& l, int level, std::vector<int>& out);
	std::string cellName;                                  // where the level starts
	bool chargen = true;                                   // starts with character creation
	std::vector<CellDef> cells;                            // the level's cells, the starting one first
	std::unordered_map<std::string, std::string> levelUpText;   // Morrowind.ini [Level Up]: "level2".., "default"
	std::vector<ActorDef> actors;
	std::unordered_map<std::string, Object> objects;       // lowercase id
	std::unordered_map<std::string, Script> scripts;       // lowercase name
	std::vector<Topic> topics;
	std::unordered_set<std::string> speakers;   // lowercase ids some response names as its speaker
	std::unordered_map<std::string, Journal> journals;     // lowercase quest id
	std::unordered_map<std::string, GlobalDef> globals;
	std::unordered_map<std::string, std::string> gmstText;
	std::unordered_map<std::string, float> gmstNum;
	std::vector<ClassDef> classes;
	std::vector<RaceDef> races;
	std::vector<BirthDef> birthsigns;
	std::unordered_map<std::string, SpellDef> spells;
	std::unordered_map<int, MagicEffectDef> magicEffects;
	std::vector<SkillDef> skills;
	std::unordered_map<std::string, SoundDef> sounds;      // lowercase sound id
	std::unordered_map<std::string, std::string> voices;   // lowercase "vo\...wav" path -> file
	std::vector<std::string> music, battleMusic;
	std::unordered_map<std::string, FactionDef> factions;
	std::vector<QuizQuestion> quiz;                        // class generation questions
	std::vector<std::pair<int, std::string>> startItems;   // new game: the player's clothes (count, id)
	std::vector<std::pair<int, std::string>> skipChargenItems;  // starting past chargen: weapon, gold
	std::unordered_map<std::string, std::vector<float>> townSpawns;   // lowercase town -> feet x y z, yaw
	std::string waterSound;                                // looping shore ambience (sound id)
	MapDef map;
	UiThemeDef ui;
	std::unordered_map<std::string, std::vector<int>> refCells;   // object id scripts name -> cells holding it
	FirstPersonDef firstPerson;
	SkyBodiesDef skyBodies;
	ArtDef art;

	const Object* object(const std::string& id) const;
	u32 enchantGlow(const std::string& itemId) const;   // an enchanted item's glow (0xRRGGBB: its first effect's colour), 0 none
	const Script* script(const std::string& name) const;
	std::string gmst(const char* name, const char* fallback = "") const;
	float gmstf(const char* name, float fallback = 0.0f) const;
	const Topic* topic(const std::string& name) const;
	std::string factionName(const std::string& id) const;
	std::string rankName(const std::string& faction, int rank) const;
};

std::string lower(const std::string& s);
bool gameLoad(GameData& game, const char* path);
