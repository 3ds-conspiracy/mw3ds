#pragma once

#include "components/misc/atom.hpp"
#include "components/misc/cardtext.hpp"
#include "mwgui/uitheme.hpp"

#include <3ds/types.h>

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "components/esm3/loadscpt.hpp"
#include "components/esm3/effectlist.hpp"
#include "components/esm3/objectrecord.hpp"
#include "components/esm3/loadinfo.hpp"
#include "components/esm3/loaddial.hpp"
#include "components/esm3/loadclas.hpp"
#include "components/esm3/loadrace.hpp"
#include "components/esm3/loadbsgn.hpp"
#include "components/esm3/loadspel.hpp"
#include "components/esm3/loadmgef.hpp"
#include "components/esm3/loadskil.hpp"
#include "components/esm3/loadsoun.hpp"
#include "components/esm3/loadnpc.hpp"
#include "components/esm3/loadglob.hpp"
#include "components/esm3/loadfact.hpp"
#include "components/esm3/loadcell.hpp"
#include "components/esm3/loadregn.hpp"
#include "components/fallback/fallback.hpp"

extern u32 g_gameGeneration;           // bumped whenever game data is loaded or handed on: caches of pointers into it

// First-person view model parts (tools/convert/firstperson.py): data/fp/<piece>.fpm files
struct FpItem
{
	std::unordered_map<int, std::pair<std::string, std::string>> slots;   // body part slot -> male, female piece
	std::string model;                   // the item's own mesh (arrows in flight)
	std::string ammo;                    // arrows and bolts: the piece that sits in the bow or crossbow
	std::vector<float> arrow;            // bows and crossbows: their ArrowBone (3x4 rows, kVfxFps a second of the weapon's time)
};
// Pictures (tools/convert/artassets.py): loading screens, the level-up art by class, book pictures by path
struct ArtRef { std::string file; int w = 0, h = 0; };
struct ArtDef
{
	std::vector<ArtRef> splash;
	std::unordered_map<std::string, ArtRef> levelup, bookart, hud;
	std::unordered_map<std::string, ArtRef> birthsign;   // the birthsign menu's constellations, by sign id
	std::unordered_map<std::string, ArtRef> book;   // the reading screen: "book", "scroll", "<button>_<idle|over|pressed>"
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
	std::unordered_map<int, std::string> ptex;          // magic effect -> its particle texture (a piece's fx shapes take it)
	std::unordered_map<std::string, float> vfxLength;  // spell visual piece -> seconds it plays (its longest controller)
	std::unordered_map<std::string, std::string> cpfx;  // creature id -> its sprays' sprite piece (particles.py)
	// World objects drawn as a visual where they are placed (particles.py world_entry): a steaming pool over
	// and over, an activator held at rest until a script plays one of its groups (start, stop on its timeline)
	struct WorldFxDef { std::string piece; bool loop = false; std::unordered_map<std::string, std::pair<float, float>> groups; };
	std::unordered_map<std::string, WorldFxDef> worldFx;
	std::unordered_map<std::string, std::unordered_map<int, std::string>> races[2];   // [female] race -> slot -> piece
	std::unordered_map<std::string, FpItem> items;                                   // lowercase item id
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
	int schoolIcons[6] = { -1, -1, -1, -1, -1, -1 };   // the magic schools' skill icons (item icon pages), alteration .. restoration
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
	float waterFps = 12.0f;                                // the water surface's frames a second (Morrowind.ini [Water] SurfaceFPS)
	MapDef map;
	UiThemeDef ui;
	std::unordered_map<std::string, std::vector<int>> refCells;   // object id scripts name -> cells holding it
	FirstPersonDef firstPerson;
	SkyBodiesDef skyBodies;
	ArtDef art;
	// A translation's own files beside its Morrowind.esm (OpenMW's Translation::Storage), keys as the files spell them:
	// .cel shown cell names, .top topic forms an "@...#" link may use, .mrk the words that link a topic in text
	std::unordered_map<std::string, std::string> cellNames, phraseForms, topicKeywords;

	const Object* object(const std::string& id) const;
	u32 enchantGlow(const std::string& itemId) const;   // an enchanted item's glow (0xRRGGBB: its first effect's colour), 0 none
	const Script* script(const std::string& name) const;
	std::string gmst(const char* name, const char* fallback = "") const;
	std::string gmstLabel(const char* name, const char* fallback) const;   // gmst without its edge spaces ("Map ")
	float gmstf(const char* name, float fallback = 0.0f) const;
	const Topic* topic(const std::string& name) const;
	std::string factionName(const std::string& id) const;
	std::string rankName(const std::string& faction, int rank) const;
	std::string cellLabel(const std::string& cell) const;        // a cell's name as shown (.cel), else its ID
	std::string topicStandardForm(const std::string& phrase) const;   // .top: the topic a phrase form links to
	std::string topicKeyword(const std::string& topic) const;   // .mrk: what names the topic in text
};

std::string lower(const std::string& s);
bool gameLoad(GameData& game, const char* path);
bool gameLoadUiTheme(const char* path, UiThemeDef& t);   // only game.json's "ui" (the startup, before the game loads)
