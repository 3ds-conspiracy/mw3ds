#include <cmath>
#include <cstdio>
#include <cstring>

#include "components/debug/debuglog.hpp"
#include "mwgui/windowmanagerimp.hpp"
#include "mwbase/session.hpp"
#include <algorithm>
#include <array>
#include <functional>
#include <sys/stat.h>
#include <tex3ds.h>
#include <citro2d.h>
#include <cstdlib>
#include <malloc.h>

// ---- names

static const char* kAttrKeys[8] = { "sattributestrength", "sattributeintelligence", "sattributewillpower",
	"sattributeagility", "sattributespeed", "sattributeendurance", "sattributepersonality", "sattributeluck" };

static const char* kAttrFallback[8] = { "Strength", "Intelligence", "Willpower", "Agility", "Speed",
	"Endurance", "Personality", "Luck" };

static const char* kSkillKeys[27] = { "sskillblock", "sskillarmorer", "sskillmediumarmor", "sskillheavyarmor",
	"sskillbluntweapon", "sskilllongblade", "sskillaxe", "sskillspear", "sskillathletics", "sskillenchant",
	"sskilldestruction", "sskillalteration", "sskillillusion", "sskillconjuration", "sskillmysticism",
	"sskillrestoration", "sskillalchemy", "sskillunarmored", "sskillsecurity", "sskillsneak", "sskillacrobatics",
	"sskilllightarmor", "sskillshortblade", "sskillmarksman", "sskillmercantile", "sskillspeechcraft",
	"sskillhandtohand" };

static const char* kSpecKeys[3] = { "sspecializationcombat", "sspecializationmagic", "sspecializationstealth" };

std::string attrName(const World& w, int i)
{
	return i >= 0 && i < 8 ? w.game.gmst(kAttrKeys[i], kAttrFallback[i]) : "?";
}

std::string skillName(const World& w, int i)
{
	return i >= 0 && i < 27 ? w.game.gmst(kSkillKeys[i], kSkillKeys[i] + 6) : "?";
}

std::string specName(const World& w, int i)
{
	return i >= 0 && i < 3 ? w.game.gmst(kSpecKeys[i]) : "?";
}

void header(const std::string& title, const std::string& note)
{
	uiCaption(title, note);
}

int buttonRow(const std::vector<std::string>& labels, int& focus, float y, float h)
{
	const UiInput& in = uiIn();
	int n = labels.size();
	if (n == 0)
		return -1;
	if (in.down & KEY_RIGHT) focus = (focus + 1) % n;
	if (in.down & KEY_LEFT) focus = (focus + n - 1) % n;
	if (focus >= n) focus = n - 1;
	float bw = (312.0f - (n - 1) * 4.0f) / n;
	int hit = -1;
	for (int i = 0; i < n; i++)
		if (uiButton(4 + i * (bw + 4), y, bw, h, labels[i], i == focus))
			hit = i;
	return hit;
}

// ---- item grids (Morrowind's inventory look)

UiGridItem gridItem(const Object* o, const std::string& id, int count, bool equipped)
{
	u32 flags = (equipped ? UIGRID_EQUIPPED : 0) | (o && o->magic ? UIGRID_MAGIC : 0);
	return { o ? o->iconIx : -1, count, flags, o ? o->name.str() : id };
}

// Morrowind's inventory filters: All, Weapon, Apparel, Magic, Misc
const char* kTabs[5] = { "All", "Weapon", "Apparel", "Magic", "Misc" };
const char* kTabKeys[5] = { "salltab", "sweapontab", "sappareltab", "smagictab", "smisctab" };

bool inTab(const Object* o, int tab)
{
	if (tab == 0 || !o)
		return true;
	bool weapon = o->recType == REC_WEAP, apparel = o->recType == REC_ARMO || o->recType == REC_CLOT;
	bool magic = o->magic || o->recType == REC_ALCH || o->scroll;
	switch (tab)
	{
	case 1: return weapon;
	case 2: return apparel;
	case 3: return magic;
	default: return !weapon && !apparel && !magic;
	}
}

bool schoolButton(const GameData& g, float x, float y, float w, float h, int tab, bool lit)
{
	static const char* kShort[7] = { "All", "Alt", "Con", "Des", "Ill", "Mys", "Res" };
	int icon = tab > 0 && tab <= 6 ? g.schoolIcons[tab - 1] : -1;
	bool hit = uiButton(x, y, w, h, icon >= 0 ? "" : kShort[tab], lit);
	if (icon >= 0)
		uiIcon(icon, roundf(x + (w - 16) / 2), roundf(y + (h - 16) / 2), 16.0f);
	return hit;
}

// The selected item's name, and its weight and value on the right
void itemInfo(const Object* o, float y, const std::string& extra)
{
	if (!o)
		return;
	char buf[64];
	snprintf(buf, sizeof(buf), "Wt %.1f  Val %d", o->weight, o->value);
	std::string right = extra.empty() ? std::string(buf) : extra;
	float rw = uiTextWidth(right, 0.42f);
	std::string name = o->name;
	while (name.size() > 3 && uiTextWidth(name, 0.5f) > 300 - rw)
		name = name.substr(0, name.size() - 4) + "...";
	uiText(6, y, 0.5f, col::header, name);
	uiTextRight(314, y + 2, 0.42f, col::textDim, right);
}

// ---- bottom screen

void Session::drawBottom()
{
	// (the target is cleared to this colour already: main)
	if (!messages.empty())
	{
		drawMessage();
		return;
	}
	switch (screen)
	{
	case SCR_NONE: drawHud(); break;
	case SCR_DIALOGUE: drawDialogue(); break;
	case SCR_BOOK: drawBook(); break;
	case SCR_CONTAINER: drawContainer(); break;
	case SCR_INVENTORY: drawInventory(); break;
	case SCR_JOURNAL: drawJournal(); break;
	case SCR_STATS: drawStats(); break;
	case SCR_RACE: drawRace(); break;
	case SCR_CLASS_METHOD: drawClassMethod(); break;
	case SCR_CLASS_LIST: drawClassList(); break;
	case SCR_CLASS_QUIZ: drawClassQuiz(); break;
	case SCR_CLASS_RESULT: drawClassResult(); break;
	case SCR_CLASS_CREATE: drawClassCreate(); break;
	case SCR_BIRTH: drawBirth(); break;
	case SCR_REVIEW: drawReview(); break;
	case SCR_REST: drawRest(); break;
	case SCR_LEVELUP: drawLevelUp(); break;
	case SCR_END: drawEnd(); break;
	case SCR_BARTER: drawBarter(); break;
	case SCR_DEATH: drawDeath(); break;
	case SCR_TRAVEL: drawTravel(); break;
	case SCR_MAP: drawMap(); break;
	case SCR_MAGIC: drawMagic(); break;
	case SCR_SPELLS: drawSpells(); break;
	case SCR_ARREST: drawArrest(); break;
	case SCR_TRAINING: drawTraining(); break;
	case SCR_PERSUADE: drawPersuade(); break;
	case SCR_ALCHEMY: drawAlchemy(); break;
	case SCR_SPELLMAKE: drawSpellmaking(); break;
	case SCR_ENCHANT: drawEnchanting(); break;
	case SCR_RECHARGE: drawRecharge(); break;
	case SCR_OPTIONS: case SCR_AUDIO: case SCR_DISPLAY: case SCR_LOOK: drawOptions(); break;
	case SCR_CONTROLS: drawControls(); break;
	case SCR_CONSOLE: drawConsole(); break;
	case SCR_REPAIR: drawRepair(); break;
	case SCR_SAVES: drawSaves(); break;
	case SCR_GAMEMENU: drawGameMenu(); break;
	default: break;
	}
}

void Session::openScreen(Screen s)
{
	screen = s;
	list = UiList();
	list2 = UiList();
	grid = UiGrid();
	scroll = UiScroll();
	focus = 0;
	containerPutting = false;
	bookFromItems = false;
	playSound(-1, "Menu Click");
}

void Session::closeScreen()
{
	if (screen == SCR_DIALOGUE || screen == SCR_BARTER || screen == SCR_TRAVEL || screen == SCR_SPELLS
		|| screen == SCR_TRAINING || screen == SCR_PERSUADE || screen == SCR_SPELLMAKE
		|| (screen == SCR_REPAIR && repairRef >= 0)
		|| (screen == SCR_ENCHANT && enchanterRef >= 0))
		dialogueClose(dlg, w);
	barterRef = -1;
	// Walking away from someone's pockets: one last chance they notice
	if (screen == SCR_CONTAINER && pickpocketing && containerRef >= 0 && pickpocketCaught(0.0f))
	{
		logf("pickpocket: caught leaving %s", w.refs[containerRef].id.c_str());
		say(containerRef, "", w.game.gmst("scaughtstealingmessage", "Hey he's stealing my stuff!"));
		crimeSeen(CRIME_PICKPOCKET, 0, containerRef), reportCrime(containerRef, (int)w.game.gmstf("icrimepickpocket", 25));
	}
	pickpocketing = false;
	companionSharing = companionWarning = false;
	screen = SCR_NONE;
	bookRef = containerRef = -1;
	bookItem.clear();
	bookPagesOf.clear();
	bookSpread = 0;
	uiIconsRelease();
}

void Session::updateWindows()
{
	revealLocalMap();
	// A named place outdoors, once visited, is on the map
	if (w.current >= 0 && w.current != mapCell)
	{
		mapCell = w.current;
		if (!w.cells[mapCell].interior)
		{
			w.mapKnown.insert(lower(w.cells[mapCell].name));
			w.mapCells.insert(World::mapCellKey(w.cells[mapCell].gx, w.cells[mapCell].gy));
		}
	}
	// The journal opens on its newest page
	if (screen != SCR_JOURNAL)
	{
		journalPage = 0;
		journalPicking = false;
	}
	// The map screen starts centred on the player and gives its detail tiles back when it closes
	if (screen != SCR_MAP)
	{
		mapViewSet = false;
		freeMapTiles();
		mapDoorTip = -1;
		mapLocal = (w.current >= 0 && w.cells[w.current].interior) || mapLocalOutdoors;
	}
}

void Session::openPendingMenu()
{
	if (!menuOpen() && !wantName && !pendingMenus.empty())
	{
		ScriptMenu m = pendingMenus.front();
		pendingMenus.erase(pendingMenus.begin());
		switch (m)
		{
		case SMENU_NAME: wantName = true; break;
		case SMENU_RACE: openScreen(SCR_RACE); break;
		case SMENU_CLASS: openScreen(SCR_CLASS_METHOD); break;
		case SMENU_BIRTH: openScreen(SCR_BIRTH); break;
		case SMENU_REVIEW: openScreen(SCR_REVIEW); break;
		case SMENU_REST: openRest(false); break;
		case SMENU_REST_BED: openRest(true); break;
		}
	}
}
