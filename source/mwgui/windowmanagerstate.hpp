#pragma once

// Session's state for the screens: menus and lists, the journal, books, the map, message boxes, the HUD, the
// options (OpenMW's WindowManager)
// Session derives from it: the members are Session's (s.console), kept here beside the code that uses them
#include "mwbase/sessiontypes.hpp"

struct WindowManagerState
{
	DevConsole console;              // the in-game console (Options): its output, history and selected object
	Dialogue dlg;
	Screen screen = SCR_NONE;
	std::vector<MessageState> messages;
	std::vector<ScriptMenu> pendingMenus;
	bool wantName = false;            // main loop shows the system keyboard between frames
	enum { TEXT_NONE, TEXT_MAKENAME, TEXT_SEARCH, TEXT_CONSOLE, TEXT_CLASSNAME, TEXT_CLASSDESC };
	int wantText = TEXT_NONE;         // the same, for the name of the spell / item being made, the journal's search or the class being made
	int travelCell = -1;              // door to another cell: main loop shows a loading screen, then finishTravel()
	float travelPos[3] = {}, travelYaw = 0.0f;
	bool travelToSpawn = false;   // test GOTO into an exterior cell: land at the cell's own spawn spot
	int enemyRef = -1;                // last NPC hit: its health bar shows on the top screen
	bool dynamicView = true;          // Options > Video > Dynamic view: the view shortens when frames run long (dynview=)
	std::string hudCaption;           // the cell name the HUD wrote under the minimap last
	bool hidden = false;              // sneaking and no one nearby has noticed
	float previewSpin = 0.0f;         // the race menu's face turned this far (radians) by the C-stick
	// repairing: -1 = with a repair tool (inventory index repairTool), else the smith's ref
	int repairRef = -1, repairTool = -1;
	int quitSaving = 0;               // Save and quit: "Saving..." shown first (frames), then the save; -1 saved, closing
	bool wantReload = false;          // death / Saves screen: main loop restarts the session from a save
	std::string reloadPath;           // which save (empty = the autosave)
	std::vector<SaveInfo> saveList;   // Saves screen
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
	bool mapLocal = false;            // the map screen shows the local map (else the world map)
	bool mapLocalOutdoors = false;    // and which one it opens on outdoors (indoors: the local map)
	int mapDoorTip = -1;              // the door marker tapped on the local map: its destination shows
	int mapPlaceTip = -1;             // the place marker tapped on the world map (game.map.labels): its name shows
	std::vector<int> mapLabelCells;   // game.map.labels: how many exterior cells carry each name (its marker's size)
	// Interior fog of war (World::mapFog) as a texture over the local map, filled from it each time it is drawn
	C3D_Tex fogTex;
	bool fogTexReady = false;
	// The 2x-detail map's tiles: read only while the map screen shows the zoomed-in view (up to 2 stay
	// when out of view, 4 at most, 512 KB each), freed when it closes
	struct MapTileTex { C3D_Tex tex; bool loaded = false, failed = false; };
	MapTileTex mapTiles[4];
	// Local map: the interior rendered from above on arrival (renderer texture), again when the player
	// changes floor; outdoors (localMapCell -2) the cells round the player, while the map screen shows it
	int localMapCell = -1;
	// MAPCELL (tools/convert/maprender.py): an outdoor cell rendered from above into a file, once its neighbours load
	struct MapCellJob { int gx = 0, gy = 0, frames = 0; std::string path; bool on = false; } mapCellJob;
	float localMin[2] = {}, localSize = 1.0f, localTopZ = 0.0f;
	bool testNoMovies = false;    // tests: play as if the data had no converted movies

	// menu state
	UiList list, list2;
	UiGrid grid;                      // item icons (inventory, containers, barter)
	int invTab = 0;                   // inventory filter: all, weapon, apparel, magic, misc
	int magicTab = 0;                 // Magic menu filter: all, then the six schools
	int journalTab = 0;               // journal: 0 the entries, 1 the topics index
	std::string journalTopic;         // the topic open in the index
	std::string journalSearch;        // what the index and the entries are narrowed to ("": all)
	UiLinkText journalText;           // the entries / the open topic, its topics as links
	// The entries a page at a time (a long journal was slow to lay out and draw whole), narrowed to one questline
	int journalCat = 0;               // 0 every quest, else 1 + kQuestLines' index (mwgui/statswindow.cpp)
	int journalPage = 0;
	bool journalPicking = false;      // the questline list is open over the page
	UiList journalCatList;
	std::vector<int> journalShown;    // w.journal's entries in the questline and the search, newest first
	std::vector<int> journalCatCount; // entries per questline (0: all)
	long journalShownKey = -1;        // what journalShown was worked out for
	UiScroll scroll;
	int focus = 0;
	int bookRef = -1, containerRef = -1;
	bool pickpocketing = false;       // the container screen shows a living NPC's pockets (sneaking)
	std::string bookItem;
	// The book open: its text cut into pages (for the book read, bookPagesOf), the spread shown (pages 2n, 2n + 1)
	std::vector<UiPage> bookPages;
	std::string bookPagesOf;
	int bookSpread = 0;
	int quizIndex = 0, quizCounts[3] = {};
	std::string pickedClass;
	// Create Custom Class: the class being filled in (kept when Back leaves it, as OpenMW keeps the dialog), and the
	// field whose choice list is open over the form (-1: none; CF_ field ids, mwgui/statswindow.cpp)
	ClassDef classDraft = {};
	bool classDraftReady = false;
	int classPicking = -1, classPickCursor = 0;
	struct Note { std::string text; float until; };
	std::vector<Note> notes;
	UiScroll quizScroll;              // the class quiz's page, when a question and its answers are taller than the screen
	float quizBottom = 0.0f;          // tests: the bottom of the class quiz's focused answer (the screen is 240 high)
	int magicShown = 0, magicIcons = 0;   // tests: the Magic menu's rows after its school filter, and those with an icon
	int birthIcons = 0;               // tests: the birthsign menu's effect rows with an icon
	float notesRight = 0.0f;          // tests: the right edge of the notices drawn last (the top screen is 400 wide)
	bool messageSidebar = false;      // a message box with more text than fits above its buttons: buttons on the right
	int messageTop = 0, messageLeft = 0;   // tests: its first line shown, and the lines below what is shown
	UiScroll messageScroll;           // a long message box's text, scrolled
	std::string messageShown;         // the message box that scroll belongs to
	int hudEye = 0;                   // the HUD pass drawing now (0 left / mono, 1 right)
	std::string subtitle;
	float subtitleUntil = 0.0f;
	float fade = 0.0f;

	// dialogue layout
	std::vector<Word> words;
	std::vector<std::string> linkTopics;
	float wordsHeight = 0.0f, dlgScroll = 0.0f;
	int wordsRevision = -1;
	int restInterruptAt = -1;                    // sleeping outdoors: the hour (restDone) a creature comes
	std::string restInterruptList;
	float fadeTarget = 0.0f, fadeRate = 2.5f;     // script fades (FadeOut / FadeIn)

	// Stereo 3D (main.cpp draws each eye): the 3D slider alone turns it on and off (no Options toggle), whatever the frame
	// rate; only a second eye that wouldn't fit the frame's GPU command buffer waits
	// Combat rules (Options): vanilla, or Glancing Blows and Crits (GBAC 1.2.1 by ZullleMW, Nexus 49541):
	// misses become glancing blows, crits come with a high hit chance, the same damage on average
	enum { COMBAT_VANILLA = 0, COMBAT_GBAC = 1 };
	int combatMode = COMBAT_VANILLA;
	// Hit & Miss Indicators (MorleyDev, Nexus 55396; Options): floating text by the crosshair for the player's
	// blows: "Miss (NN%)" white, health damage red, fist (fatigue) damage green; rises and fades in 1 s
	bool hitIndicators = false;
	struct Indicator { std::string text; u32 rgb; float x, y, timer; };
	std::vector<Indicator> indicators;
	// Options (settings.txt): difficulty -100..100 (Morrowind's slider), effects / music volume 0..100
	int difficulty = 0, effectsVolume = 100, musicVolume = 100;
	float consoleBack = 0.0f;         // its output scrolled back by this many lines
	std::string consoleDraft;         // what its keyboard opens with (Edit last: the line typed last)
	// Tooltip: what a menu has selected, in full on the top screen (as Morrowind's hover box);
	// the screens fill it each frame, drawTop shows and clears it
	std::vector<std::pair<std::string, u32>> tip;
	// A bordered box in the middle of the top screen (character creation: the class picture, the birthsign's
	// constellation, health / magicka / fatigue): the screens fill it each frame, drawTop shows and clears it as it does the tip
	struct TopModal
	{
		enum { NONE, ART, VITALS } kind = NONE;
		ArtRef art;                       // ART: the picture
		std::string title, caption;
		int vitals[3][2] = {};            // VITALS: health, magicka, fatigue: now, most
		bool attributes = false;          // VITALS: and the eight attributes under them (the player's)
	} topModal;
	FILE* movieFile = nullptr;
	u32 movieFrames = 0, movieFps100 = 1000, movieW = 0, movieH = 0;
	int movieFrame = -1;
	float movieTime = 0.0f;
	C3D_Tex movieTex;
	bool movieTexOk = false;
	bool containerPutting = false;                    // the container screen shows the player's items (to put in)
	int promptRows = 1;                                    // the lines it took last (the attack hint under it moves down for two)
	bool deathAnySave = false;                    // any save to choose from (the game's starting points too)
	int barterCount = 0;                          // barter: how many of the picked stack (0: not choosing yet)
	bool deletingSave = false;                    // Saves screen: asking before deleting the selected save
	int haggle = 0;                               // barter: the player's offer against the price, in 5% steps
	std::string rechargeGem;                       // the soul gem Recharge was chosen on (its soul's creature)
	int enchanterRef = -1, enchantItem = -1, enchantGem = -1, enchantType = -1;
	std::vector<SpellEffect> makeEffects;        // spellmaking / enchanting: the effects being put together
	std::string makeName;                        // and the name it gets (OpenMW's name box)
	std::string makeNameItem;                    // enchanting: the item that name was taken from
	int makeSel = -1;
	std::vector<std::string> alchemySlots;        // up to four ingredients chosen
	// rest / wait, levelling, the calendar (mwgui/waitdialog.cpp)
	int restHours = 1;                // chosen on the rest screen
	bool restSleep = false;           // asleep (health and magicka come back) rather than waiting
	bool restBed = false;             // opened by a bed
	int restDone = -1, restTotal = 0; // hours passed so far while resting (-1: still choosing)
	int restFrames = 0;
	std::vector<int> levelPicks;      // attributes chosen on the level-up screen
	int levelCursor = 0;              // 0..7 attributes, 8 OK
	int mapCell = -1;                 // the cell whose name was last put on the map
};
