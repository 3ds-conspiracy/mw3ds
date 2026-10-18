#include <cmath>
#include <cstdio>
#include <cstring>

#include "audio.h"
#include "gamma.h"
#include "linear.h"
#include "log.h"
#include "renderer.h"
#include "screens.h"
#include "session.h"

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
	bool weapon = o->type == "WEAP", apparel = o->type == "ARMO" || o->type == "CLOT";
	bool magic = o->magic || o->type == "ALCH" || o->scroll;
	switch (tab)
	{
	case 1: return weapon;
	case 2: return apparel;
	case 3: return magic;
	default: return !weapon && !apparel && !magic;
	}
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

// ---- top screen overlay

void Session::drawTop()
{
	const UiInput& in = uiIn();
	(void)in;
	// (aimShift: in 3D, the crosshair and the label sit at the depth of what's aimed at)
	float ax = aimShift;
	if (!indicators.empty())
	{
		// both eyes draw them; the time moves on once a frame (the left eye's pass)
		static u64 last = 0;
		u64 now = osGetTime();
		float dt = last && hudEye == 0 ? fminf(0.1f, (now - last) / 1000.0f) : 0.0f;
		if (hudEye == 0)
			last = now;
		drawIndicators(dt);
	}
	if (!menuOpen())
	{
		uiRect(199 + ax, 117, 3, 7, C2D_Color32(255, 255, 255, 150));
		uiRect(197 + ax, 119, 7, 3, C2D_Color32(255, 255, 255, 150));
	}
	if (target >= 0 && !menuOpen())
	{
		// (a long name, "Hut Door - Ald-ruhn, Tiras Sadus: General Merchandise", wraps over lines inside the screen)
		std::vector<std::string> lines = targetPopupLines(target);
		float lh = uiLineHeight(0.55f), tw = 0.0f;
		for (auto& l : lines)
			tw = fmaxf(tw, uiTextWidth(l, 0.55f));
		float h = lines.size() * lh + 2;
		uiRect(200 + ax - tw / 2 - 6, 132, tw + 12, h + 2, col::border);
		uiRect(200 + ax - tw / 2 - 5, 133, tw + 10, h, col::panel);
		for (size_t i = 0; i < lines.size(); i++)
			uiTextCentered(200 + ax, 134 + i * lh, 0.55f, w.ownedByOther(target) ? col::health : col::text, lines[i]);
	}
	// Notices, each wrapped to the screen's width ("Release Identification has been removed from your inventory."
	// ran off it)
	float y = 4;
	notesRight = 0.0f;
	// (while a menu pauses the world, short of the "Paused" label in the top right corner)
	float notesW = worldPaused() ? 384 - uiTextWidth("Paused", 0.5f) - 16 : 384;
	for (auto& n : notes)
		for (auto& line : uiWrap(n.text, notesW, 0.5f))
		{
			float tw = uiTextWidth(line, 0.5f);
			uiRect(4, y, tw + 8, 16, C2D_Color32(0, 0, 0, 150));
			uiText(8, y, 0.5f, col::header, line);
			notesRight = fmaxf(notesRight, 8 + tw);
			y += 18;
		}
	if (w.time < subtitleUntil && !subtitle.empty())
	{
		std::vector<std::string> lines = uiWrap(subtitle, 380, 0.5f);
		float lh = uiLineHeight(0.5f);
		float top = 236 - lines.size() * lh;
		uiRect(4, top - 2, 392, lines.size() * lh + 4, C2D_Color32(0, 0, 0, 220));   // the world barely shows through
		for (size_t i = 0; i < lines.size(); i++)
			uiTextCentered(200, top + i * lh, 0.5f, col::header, lines[i]);
	}
	// Sneaking and nobody has noticed: Morrowind's sneak eye by the crosshair
	if (hidden && !menuOpen())
	{
		auto eye = w.game.art.hud.find("sneak");
		if (eye == w.game.art.hud.end() || !uiArt(eye->second.file, eye->second.w, eye->second.h, 170, 158, 20, 20))
			uiTextCentered(200, 158, 0.45f, col::header, "(hidden)");
	}
	// Wind-up of a blow under the crosshair
	if (attackCharge >= 0.0f && !menuOpen())
	{
		uiRect(170, 150, 60, 5, C2D_Color32(0, 0, 0, 160));
		uiRect(170, 150, 60 * attackCharge, 5, col::header);
	}
	// Health of whoever was hit last
	if (enemyRef >= 0 && w.time < enemyUntil && w.active(enemyRef))
	{
		const Ref& e = w.refs[enemyRef];
		uiRect(150, 222, 100, 10, C2D_Color32(0, 0, 0, 170));
		uiRect(151, 223, 98 * fmaxf(0.0f, e.health) / fmaxf(1.0f, e.healthMax), 8, col::health);
	}
	if (hurtFlash > 0.0f)
		uiRect(0, 0, 400, 240, C2D_Color32(200, 0, 0, (u8)(hurtFlash * 90)));
	// The selected item's or spell's tooltip
	if (menuOpen() && !tip.empty())
	{
		const float scale = 0.45f, lh = uiLineHeight(scale);
		float tw = 0.0f;
		for (auto& l : tip)
			tw = fmaxf(tw, uiTextWidth(l.first, l.second == col::header ? 0.52f : scale));
		tw = fminf(tw, 380.0f);
		float th = tip.size() * lh + 8, x = 200 - tw / 2 - 8, y = 120 - th / 2;
		uiRect(x - 1, y - 1, tw + 18, th + 2, col::border);
		uiRect(x, y, tw + 16, th, C2D_Color32(0, 0, 0, 215));
		for (size_t i = 0; i < tip.size(); i++)
			uiTextCentered(200, y + 4 + i * lh, tip[i].second == col::header ? 0.52f : scale, tip[i].second, tip[i].first);
	}
	// set by the bottom screen each frame; kept until both eyes have drawn it (clearing it after the first
	// pass left the tooltip in one eye only)
	if (hudEye == hudEyes - 1)
		tip.clear();
	// A menu has stopped the world (both eyes draw it: it comes from the screen, not from a timer)
	if (worldPaused())
	{
		float pw = uiTextWidth("Paused", 0.5f);
		uiRect(396 - pw - 12, 4, pw + 10, 16, C2D_Color32(0, 0, 0, 150));
		uiTextRight(392, 4, 0.5f, col::header, "Paused");
	}
	// Breath under water: a bar that empties, then drowning
	if (breath >= 0.0f)
	{
		float full = w.game.gmstf("fholdbreathtime", 20.0f);
		uiRect(150, 8, 100, 8, C2D_Color32(0, 0, 0, 150));
		uiRect(151, 9, 98 * fmaxf(0.0f, breath) / fmaxf(1.0f, full), 6, C2D_Color32(120, 170, 230, 220));
		uiTextCentered(200, 18, 0.4f, breath > 0.0f ? col::text : col::health, breath > 0.0f ? w.game.gmstLabel("sbreath", "Breath") : "Drowning!");
	}
	if (fade > 0.0f)
		uiRect(0, 0, 400, 240, C2D_Color32(0, 0, 0, (u8)(fminf(fade, 1.0f) * 255)));
	if (wantName)
		uiTextCentered(200, 110, 0.6f, col::header, "Enter your name");
}

// ---- bottom screen

void Session::drawBottom()
{
	uiRect(0, 0, 320, 240, C2D_Color32(10, 8, 6, 255));
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

// The four quick keys as slots under the minimap (Up, Right, Down, Left), big enough to tap: the item's icon or the
// spell's first letters; a tap uses the slot as its D-pad press does (GoldenEye's D-pad moves: the taps are its quick keys)
void Session::drawQuickStrip()
{
	static const char* dirs[4] = { "U", "R", "D", "L" };
	const float kSlot = 26.0f;
	for (int k = 0; k < 4; k++)
	{
		float x = 190 + k * 30, y = 179;
		const std::string& q = w.stats.quickKeys[k];
		bool isSpell = q.compare(0, 6, "magic:") == 0 && q.compare(0, 11, "magic:item:") != 0;
		std::string id = q.compare(0, 4, "inv:") == 0 ? q.substr(4) : q.compare(0, 11, "magic:item:") == 0 ? q.substr(11)
			: q.compare(0, 6, "magic:") == 0 ? q.substr(6) : q;
		const Object* o = q.empty() || isSpell ? nullptr : w.game.object(id);
		uiRect(x, y, kSlot, kSlot, col::panel);
		if (!q.empty() && !(o && uiIcon(o->iconIx, x + 1, y + 1, kSlot - 2.0f)))
		{
			std::string name = isSpell ? (w.game.spells.count(id) ? w.game.spells[id].name : id) : o ? o->name.str() : id;
			uiTextCentered(x + kSlot / 2, y + kSlot / 2 - uiLineHeight(0.38f) / 2, 0.38f, isSpell ? col::link : col::textDim, name.substr(0, 3));
		}
		uiFrame(x, y, kSlot, kSlot);
		uiText(x + 2, y, 0.3f, col::textOver, dirs[k]);
		if (uiHit(x, y, kSlot, kSlot) && w.controlsEnabled && !playerDead)
			useQuickKey(k);
	}
}

void Session::drawHud()
{
	const PlayerStats& s = w.stats;
	std::string sub;
	for (auto& r : w.game.races)
		if (lower(r.id) == lower(s.race))
			sub = r.name;
	if (const ClassDef* c = w.playerClass())
		sub += " " + c->name;
	header(s.name, sub);

	// Health, magicka, fatigue (Morrowind's colours), the numbers inside the bars
	float cur[3] = { s.health, s.magicka, s.fatigue }, max[3] = { s.healthMax, s.magickaMax, s.fatigueMax };
	u32 colors[3] = { col::health, col::magicka, col::fatigue };
	for (int i = 0; i < 3; i++)
	{
		float y = 27 + i * 15;
		uiBar(6, y, 174, 12, max[i] > 0 ? cur[i] / max[i] : 0, colors[i]);
		char buf[32];
		snprintf(buf, sizeof(buf), "%d/%d", (int)cur[i], (int)max[i]);
		uiTextCentered(93, y + 6 - uiLineHeight(0.36f) / 2, 0.36f, col::textPressed, buf);
	}

	// Weapon and spell, as the boxes by Morrowind's bars
	const InventoryItem* weapon = nullptr;
	for (auto& it : w.inventory)
		if (it.equipped)
			if (const Object* o = w.game.object(it.id))
				if (o->type == "WEAP")
					weapon = &it;
	const Object* wo = weapon ? w.game.object(weapon->id) : nullptr;
	float by = 75;
	uiRect(6, by, 36, 36, col::panel);
	if (!(wo && uiIcon(wo->iconIx, 8, by + 2)))
		uiTextCentered(24, by + 18 - uiLineHeight(0.36f) / 2, 0.36f, col::textDim, wo ? wo->name.str().substr(0, 3) : "Fist");
	uiFrame(6, by, 36, 36);
	uiText(48, by + 1, 0.42f, col::text, wo ? wo->name.str() : w.game.gmstLabel("sskillhandtohand", "Hand-to-hand"));
	std::string spell = s.selectedSpell.empty() ? "" : w.game.spells[s.selectedSpell].name;
	uiText(48, by + 18, 0.42f, spell.empty() ? col::textDim : col::link, spell.empty() ? "No spell" : spell);

	// Messages
	float y = 116;
	for (auto& n : notes)
	{
		for (auto& line : uiWrap(n.text, 174, 0.42f))
		{
			if (y > 164)
				break;
			uiText(6, y, 0.42f, col::text, line);
			y += uiLineHeight(0.42f);
		}
	}

	// Minimap: tap it for the map
	bool mapOk = !w.game.map.file.empty() && ((w.menusEnabled & MENU_MAP) || !w.game.chargen);
	drawMapView(186, 26, 128, 128, 1.0f, false, w.current >= 0 && w.cells[w.current].interior);
	if (mapOk && uiHit(186, 26, 128, 128))
		openScreen(SCR_MAP);
	std::string here = w.game.cellLabel(w.cellName());
	hudCaption = here.size() > 26 ? here.substr(0, 25) + "..." : here;
	uiTextCentered(250, 155, 0.38f, col::textDim, hudCaption);

	if (target >= 0)
	{
		// two lines for a long name, a little smaller and higher, and the attack hint below them moves down
		float s = 0.45f;
		std::vector<std::string> use = targetPromptLines(target, &s);
		promptRows = (int)use.size();
		float lh = uiLineHeight(s);
		for (size_t i = 0; i < use.size(); i++)
			uiText(6, (use.size() > 1 ? 166 : 172) + i * lh, s, col::link, use[i]);
	}
	else
		promptRows = 1;
	if (w.fightingEnabled)
	{
		bool xbox = g_controlLayout == CONTROLS_XBOX || g_controlLayout == CONTROLS_GOLDENEYE;   // (GoldenEye: the Xbox buttons)
		std::string hint = w.stats.selectedSpell.empty() ? (xbox ? "R: attack (hold to wind up)" : "X: attack (hold to wind up)")
			: std::string(xbox ? "R: attack   L: cast " : "X: attack   Y: cast ") + w.game.spells[w.stats.selectedSpell].name;
		// Leave the quick key strip (from x 190) clear, and GoldenEye's 1st / 3rd button (from x 132)
		float room = g_controlLayout == CONTROLS_GOLDENEYE ? 122.0f : 180.0f;
		while (hint.size() > 3 && uiTextWidth(hint, 0.38f) > room)
			hint = hint.substr(0, hint.size() - 4) + "...";
		uiText(6, promptRows > 1 ? 192 : 190, 0.38f, col::textDim, hint);
	}
	drawQuickStrip();
	uiTextCentered(250, 166, 0.36f, col::textDim, dateText());
	// the bounty on the minimap's foot, the frame rate in its top corner (the strip took their place)
	if (w.bounty > 0)
	{
		std::string b = w.game.gmstLabel("sbounty", "Bounty") + " " + std::to_string(w.bounty);
		uiRect(312 - uiTextWidth(b, 0.4f) - 4, 140, uiTextWidth(b, 0.4f) + 4, 13, C2D_Color32(0, 0, 0, 170));
		uiTextRight(311, 140, 0.4f, col::health, b);
	}
	char perf[64];
	snprintf(perf, sizeof(perf), "%.0f fps", fps);
	uiTextRight(312, 27, 0.34f, col::textDim, perf);
	bool inv = w.menusEnabled & MENU_INVENTORY, stats = w.menusEnabled & MENU_STATS;
	if (uiButton(2, 208, 51, 28, "Items", false, inv))
		openScreen(SCR_INVENTORY);
	if (uiButton(55, 208, 51, 28, w.game.gmstLabel("smagicmenu", "Magic"), false, (w.menusEnabled & MENU_MAGIC) || !w.game.chargen))
		openScreen(SCR_MAGIC);
	if (uiButton(108, 208, 51, 28, w.game.gmstLabel("sjournal", "Journal"), false, !w.journal.empty()))
		openScreen(SCR_JOURNAL);
	if (uiButton(161, 208, 51, 28, w.game.gmstLabel("smap", "Map"), false, !w.game.map.file.empty() && ((w.menusEnabled & MENU_MAP) || !w.game.chargen)))
		openScreen(SCR_MAP);
	if (uiButton(214, 208, 51, 28, w.game.gmstLabel("sstats", "Stats"), false, stats))
		openScreen(SCR_STATS);
	if (uiButton(267, 208, 51, 28, "Saves", false, !wantName))
	{
		saveList = savesList(dataDir);
		openScreen(SCR_SAVES);
	}
}

// START: the game menu (Morrowind's Esc menu)
void Session::toggleGameMenu()
{
	if (screen == SCR_GAMEMENU)
		closeScreen();
	else if (screen == SCR_NONE && messages.empty() && !dlg.open && !wantName)
	{
		openScreen(SCR_GAMEMENU);
		playSound(-1, "Menu Click");
	}
}

void Session::drawGameMenu()
{
	header("Menu", w.game.cellLabel(w.cellName()));
	// Save and quit: "Saving..." on screen first, then the save (a long campaign's takes seconds: it looked frozen),
	// then "Closing..." for the quit itself: a screen that stays says which of the two it is stuck in. A save that
	// failed doesn't quit (its notice shows, the menu stays)
	if (quitSaving < 0)
	{
		uiTextCentered(160, 110, 0.6f, col::text, "Game saved. Closing...");
		wantQuit = true;
		return;
	}
	if (quitSaving > 0)
	{
		uiTextCentered(160, 110, 0.6f, col::text, w.game.gmstLabel("snotifymessage4", "Saving..."));
		if (++quitSaving > 2)
			quitSaving = save() ? -1 : 0;
		return;
	}
	const UiInput& in = uiIn();
	// OpenMW's getRestEnabled: EnableRest has run, or character creation has finished (CharGenState -1)
	auto cg = w.globals.find("chargenstate");
	bool rest = (w.menusEnabled & MENU_REST) || !w.game.chargen || (cg != w.globals.end() && cg->second == -1.0f);
	std::vector<std::string> labels = { "Return to game", "Save / Load", w.game.gmst("srestmenuxbox", "Rest / Wait"),
		w.game.gmst("soptions", "Options"), "Save and quit" };
	bool enabled[5] = { true, !wantName, rest, true, pendingMenus.empty() };
	int n = labels.size();
	if (in.down & KEY_DOWN) focus = (focus + 1) % n;
	if (in.down & KEY_UP) focus = (focus + n - 1) % n;
	int hit = -1;
	for (int i = 0; i < n; i++)
		if (uiButton(50, 32 + i * 40, 220, 34, labels[i], i == focus, enabled[i]))
			hit = i;
	if (hit == 0 || (in.down & KEY_B))
		closeScreen();
	else if (hit == 1)
	{
		saveList = savesList(dataDir);
		closeScreen();
		openScreen(SCR_SAVES);
	}
	else if (hit == 2)
	{
		closeScreen();
		openRest(false);
	}
	else if (hit == 3)
	{
		closeScreen();
		focus = 0;
		openScreen(SCR_OPTIONS);
	}
	else if (hit == 4)
		quitSaving = 1;
}

// Options, in Morrowind's tabs: Prefs, Audio, Video and Controls (L / R, or a tap on a tab). Each tab is a column of
// settings, a bar (D-pad left / right, or a tap on it, sets it) or a value (A, a tap or D-pad right steps to the next,
// left to the one before), over its buttons; D-pad up / down moves between them. Kept in settings.txt. Each tab is its
// own screen (options, audio, display, look), so B and Close leave from any of them. 3D has no setting: the 3D slider
// alone turns it on and off
static const Screen kOptionTabs[4] = { SCR_OPTIONS, SCR_AUDIO, SCR_DISPLAY, SCR_LOOK };
static const int kPrefsConsoleFocus = 4;      // Prefs' Console button, where Console's Close comes back to
static const int kControlsButtonsFocus = 6;   // Controls' Layout button, where the button list's Close comes back to

void Session::drawOptions()
{
	int tab = 0;
	while (tab < 3 && kOptionTabs[tab] != screen)
		tab++;
	header(w.game.gmst("soptions", "Options"));
	const UiInput& in = uiIn();
	static const char* kTabKeys[4][2] = { { "sprefs", "Prefs" }, { "saudio", "Audio" }, { "svideo", "Video" },
		{ "scontrols", "Controls" } };
	int to = -1;
	for (int i = 0; i < 4; i++)
		if (uiButton(4 + i * 79, 24, 75, 20, w.game.gmstLabel(kTabKeys[i][0], kTabKeys[i][1]), i == tab) && i != tab)
			to = i;
	if (in.down & KEY_L) to = (tab + 3) % 4;
	if (in.down & KEY_R) to = (tab + 1) % 4;
	if (to >= 0)
	{
		openScreen(kOptionTabs[to]);
		return;
	}

	// The focus walks lines: a line holds one setting, or buttons side by side (left / right between them)
	struct Line { int first, count; };
	std::vector<Line> lines;
	auto line = [&](int count) {
		int first = lines.empty() ? 0 : lines.back().first + lines.back().count;
		lines.push_back({ first, count });
		return first;
	};
	bool changed = false;
	// a setting's name at the left, shrunk to fit beside its control (x 118 on)
	auto name = [&](float y, const std::string& text, u32 color) {
		float s = 0.45f;
		while (s > 0.34f && uiTextWidth(text, s) > 104)
			s -= 0.02f;
		uiText(10, y, s, color, text);
	};
	// a bar: its name and value on a line, the bar across the screen under them (a line 34 high)
	auto bar = [&](float y, const std::string& label, const std::string& shown, int& value, int lo, int hi, int step) {
		bool f = focus == line(1);
		uiText(10, y, 0.45f, f ? col::header : col::text, label);
		uiTextRight(310, y, 0.45f, f ? col::header : col::text, shown);
		float bx = 14, bw = 292, by = y + 21;
		uiRect(bx, by, bw, 7, col::panel);
		uiFrame(bx, by, bw, 7);
		uiRect(bx + (float)(value - lo) / (hi - lo) * bw - 4, by - 5, 8, 17, col::header);
		int v = value;
		if (f && (in.down & KEY_LEFT)) v -= step;
		if (f && (in.down & KEY_RIGHT)) v += step;
		if (in.touching && in.touchY >= y + 10 && in.touchY < y + 34 && in.touchX >= bx - 8 && in.touchX <= bx + bw + 8)
		{
			focus = lines.back().first;
			v = lo + (int)roundf(fmaxf(0.0f, fminf(1.0f, (in.touchX - bx) / bw)) * (hi - lo) / step) * step;
		}
		v = v < lo ? lo : v > hi ? hi : v;
		if (v == value)
			return;
		value = v;
		changed = true;
	};
	// a value: its name at the left, the value on a button at the right. Returns the step asked for (-1, 0, 1)
	auto choice = [&](float y, const std::string& label, const std::string& shown, bool enabled = true) {
		bool f = focus == line(1);
		name(y + 4, label, !enabled ? col::textDim : f ? col::header : col::text);
		int d = uiButton(118, y, 198, 22, shown, f, enabled) ? 1 : 0;
		if (enabled && f && (in.down & KEY_RIGHT)) d = 1;
		if (enabled && f && (in.down & KEY_LEFT)) d = -1;
		if (d)
			changed = true;
		return d;
	};
	// buttons side by side across the screen: the one pressed, or -1
	auto buttons = [&](float y, float h, const std::vector<std::string>& labels) {
		int n = labels.size(), first = line(n), hit = -1;
		float bw = (312.0f - (n - 1) * 4.0f) / n;
		for (int i = 0; i < n; i++)
			if (uiButton(4 + i * (bw + 4), y, bw, h, labels[i], focus == first + i))
				hit = i;
		return hit;
	};
	auto onOff = [](bool on) { return std::string(on ? "On" : "Off"); };
	char text[64];
	int b = -1;
	bool closing = false;
	if (screen == SCR_OPTIONS)
	{
		snprintf(text, sizeof(text), "%d", difficulty);
		bar(50, w.game.gmstLabel("sdifficulty", "Difficulty"), text, difficulty, -100, 100, 10);
		// Combat: vanilla, or Glancing Blows and Crits (misses glance, crits; the same average damage)
		if (choice(86, "Combat hits", combatMode == COMBAT_GBAC ? "Glancing blows + crits" : "Vanilla"))
		{
			combatMode = combatMode == COMBAT_GBAC ? COMBAT_VANILLA : COMBAT_GBAC;
			notify(combatMode == COMBAT_GBAC ? "Glancing blows and crits: misses glance for half damage, strong hits can crit."
				: "Vanilla combat.");
		}
		// Hit & Miss Indicators: damage numbers and misses by the crosshair
		if (choice(112, "Hit numbers", onOff(hitIndicators)))
		{
			hitIndicators = !hitIndicators;
			notify(hitIndicators ? "Hit and miss numbers on." : "Hit and miss numbers off.");
		}
		// Subtitles: spoken lines written on the top screen
		if (choice(138, w.game.gmstLabel("ssubtitles", "Subtitles"), onOff(g_subtitles)))
		{
			g_subtitles = !g_subtitles;
			notify(g_subtitles ? "Subtitles on." : "Subtitles off: spoken lines are only heard.");
		}
		// Console: OpenMW's, one script line at a time, on the object the crosshair was on; Credits: Morrowind's movie
		int extra = buttons(170, 26, { "Console", "Credits" });
		b = buttons(208, 28, { "Default", w.game.gmstLabel("sclose", "Close") });
		closing = b == 1;
		if (extra == 0)
		{
			// the object under the crosshair when the menu was opened is the one the lines act on (OpenMW's click on it)
			if (target >= 0)
				console.selected = target;
			closeScreen();
			consoleBack = 0.0f;
			openScreen(SCR_CONSOLE);
			return;
		}
		// Morrowind's credits are its movie (Video\mw_credits.bik): data converted without it keeps Options open and
		// says so, rather than dropping back into the game as if the button did nothing
		if (extra == 1)
		{
			if (playMovie("mw_credits"))
				closeScreen();
			else
				notify("The credits movie is missing: Video\\mw_credits.bik was not converted with the game data.");
			return;
		}
		if (b == 0)
		{
			difficulty = 0;
			combatMode = COMBAT_VANILLA;
			hitIndicators = false;
			g_subtitles = true;
			changed = true;
		}
	}
	else if (screen == SCR_AUDIO)
	{
		snprintf(text, sizeof(text), "%d", effectsVolume);
		bar(50, w.game.gmstLabel("seffects", "Effects"), text, effectsVolume, 0, 100, 10);
		snprintf(text, sizeof(text), "%d", musicVolume);
		bar(86, w.game.gmstLabel("smusic", "Music"), text, musicVolume, 0, 100, 10);
		b = buttons(208, 28, { "Default", w.game.gmstLabel("sclose", "Close") });
		closing = b == 1;
		if (b == 0)
		{
			effectsVolume = musicVolume = 100;
			changed = true;
		}
	}
	else if (screen == SCR_DISPLAY)
	{
		// Gamma (0.80 to 1.80), and a ramp dark to light through the curve in use (what the bar does to the dim end);
		// the world shows on the top screen behind it, so the change is seen as it is made
		snprintf(text, sizeof(text), "%.2f", g_gammaPct / 100.0f);
		bar(50, "Gamma", text, g_gammaPct, kGammaMinPct, kGammaMaxPct, kGammaStepPct);
		float g = gammaValue();
		for (int i = 0; i < 10; i++)
		{
			int c = (int)(powf(i / 9.0f, 1.0f / g) * 255.0f + 0.5f);
			uiRect(14 + i * 29.2f, 88, 29.2f, 12, C2D_Color32(c, c, c, 255));
		}
		uiFrame(14, 88, 292, 12);
		// Auto gamma: what this console reads the light with
		GammaSensor sensor = gammaSensor();
		if (choice(108, "Auto gamma", sensor == SENSOR_NONE ? "No light sensor" : onOff(gammaAutoOn()), sensor != SENSOR_NONE))
		{
			g_autoGamma = gammaAutoOn() ? 0 : 1;
			notify(!gammaAutoOn() ? "Auto gamma off." : sensor == SENSOR_QTM
				? "Auto gamma on: the picture lifts in bright light."
				: "Auto gamma on: the inner camera checks the light at each loading screen.");
		}
		const char* how = sensor == SENSOR_QTM ? "Auto gamma checks the light every few seconds."
			: sensor == SENSOR_CAMERA ? "Auto gamma uses the inner camera at loads."
			: "No light sensor: Auto gamma stays off.";
		// Dynamic view: on, the outdoor view (OpenMW's, 7168 at the most) shortens while frames run long, the fog moving
		// in with it; off, always the whole view
		if (choice(134, "Dynamic view", onOff(dynamicView)))
		{
			dynamicView = !dynamicView;
			changed = true;
			notify(dynamicView ? "Dynamic view on: the fog closes in when the game slows down."
				: "Dynamic view off: always the full view, however slow.");
		}
		uiText(10, 162, 0.4f, col::textDim, how);
		uiText(10, 176, 0.4f, col::textDim, "Gamma 1.00 is Morrowind's own; higher helps in sun.");
		if (gammaAutoOn())
		{
			snprintf(text, sizeof(text), "Light now: x%.2f, gamma in use %.2f", gammaLift(), g);
			uiText(10, 190, 0.4f, col::textDim, text);
		}
		b = buttons(208, 28, { "Default", w.game.gmstLabel("sclose", "Close") });
		closing = b == 1;
		if (b == 0)
		{
			g_gammaPct = 100;
			g_autoGamma = -1;
			dynamicView = true;
			changed = true;
		}
	}
	else
	{
		// Look speed (the C-stick, the D-pad and the GoldenEye pad while aiming: one bar, they were two) and the GoldenEye
		// pad's turn while walking (0.50x to 2.00x), the sticks' dead zone (10% to 35% of a full push: a stick that rests
		// a little off centre kept walking or turning the view), Invert and Gyro aim
		snprintf(text, sizeof(text), "%.2fx", g_lookSpeed / 100.0f);
		bar(50, "Look speed (sticks, D-pad, aiming)", text, g_lookSpeed, kLookMinPct, kLookMaxPct, kLookStepPct);
		snprintf(text, sizeof(text), "%.2fx", g_geTurnSpeed / 100.0f);
		bar(84, "GoldenEye turn (walking)", text, g_geTurnSpeed, kLookMinPct, kLookMaxPct, kLookStepPct);
		snprintf(text, sizeof(text), "%d%%", g_stickZonePct);
		bar(118, "Stick dead zone", text, g_stickZonePct, kStickZoneMinPct, kStickZoneMaxPct, kStickZoneStepPct);
		if (choice(154, "Invert look", onOff(g_invertY)))
		{
			g_invertY = !g_invertY;
			notify(g_invertY ? "Up on the stick now looks down." : "Up on the stick looks up.");
		}
		// Gyro aim: turning the console turns the view (off, half, as far, twice as far as it turned)
		static const int kGyroSteps[4] = { 0, 50, 100, 200 };
		snprintf(text, sizeof(text), g_gyroAim > 0 ? "%.1fx" : "Off", g_gyroAim / 100.0f);
		if (int d = choice(180, "Gyro aim", text))
		{
			int k = 0;
			while (k < 3 && kGyroSteps[k] != g_gyroAim)
				k++;
			g_gyroAim = kGyroSteps[(k + d + 4) % 4];
			// (set to a speed, the gyroscope's offsets are measured first, the console laid down: GyroRest::calibrate.
			// Choosing a speed again calibrates again)
			extern bool g_gyroCalibrate;
			g_gyroCalibrate = g_gyroAim > 0;
			notify(g_gyroAim > 0 ? "Gyro aim on: lay the 3DS down for 2 seconds to calibrate, then turn it to aim." : "Gyro aim off.");
		}
		// Layout: the button list, what each button does in the layout in use (Y there switches Xbox, classic, GoldenEye)
		b = buttons(208, 28, { "Default", "Layout", w.game.gmstLabel("sclose", "Close") });
		closing = b == 2;
		if (b == 0)
		{
			g_lookSpeed = kLookDefaultPct;
			g_geTurnSpeed = 100;
			g_stickZonePct = kStickZoneDefaultPct;
			g_invertY = false;
			g_gyroAim = 0;
			changed = true;
		}
		else if (b == 1)
		{
			openScreen(SCR_CONTROLS);
			return;
		}
	}
	if (changed)
	{
		audioSetScales(effectsVolume / 100.0f, musicVolume / 100.0f);
		saveSettings();
	}
	if (closing || (in.down & KEY_B))
	{
		closeScreen();
		return;
	}
	// D-pad up / down: the line above / below (keeping the column, as near as the line has); left / right between
	// buttons side by side (a setting's own line takes them itself)
	int li = 0;
	while (li + 1 < (int)lines.size() && focus >= lines[li + 1].first)
		li++;
	int column = focus - lines[li].first, n = lines.size();
	if (in.down & (KEY_DOWN | KEY_UP))
	{
		li = (li + ((in.down & KEY_DOWN) ? 1 : n - 1)) % n;
		focus = lines[li].first + std::min(column, lines[li].count - 1);
	}
	else if (lines[li].count > 1 && (in.down & (KEY_LEFT | KEY_RIGHT)))
		focus = lines[li].first + (column + ((in.down & KEY_RIGHT) ? 1 : lines[li].count - 1)) % lines[li].count;
}

// What each button does
void Session::drawControls()
{
	bool xbox = g_controlLayout == CONTROLS_XBOX, goldenEye = g_controlLayout == CONTROLS_GOLDENEYE;
	header(xbox ? "Controls (Xbox layout)" : goldenEye ? "Controls (GoldenEye layout)" : "Controls (classic layout)");
	static const char* kXbox[][2] = {
		{ "Circle Pad / C-Stick", "Move (pushed far: run) / look" }, { "A", "Activate: talk, open, take" },
		{ "R (hold, release)", "Wind up, strike" }, { "X", "Ready / put away the weapon" }, { "L", "Cast the selected spell" },
		{ "Y", "Jump (swim up in water)" }, { "ZL (hold)", "Sneak" },
		{ "D-Pad", "Quick keys" }, { "SELECT", "First / third person" }, { "START", "Menu: save, rest, options" },
		{ "Touch screen", "Inventory, magic, map, stats" } };
	static const char* kClassic[][2] = {
		{ "Circle Pad", "Move (pushed far: run)" }, { "C-Stick / D-Pad", "Look" }, { "A", "Activate: talk, open, take" },
		{ "X (hold, release)", "Ready weapon, wind up, strike" }, { "ZL + X", "Put the weapon away" }, { "Y", "Cast the selected spell" },
		{ "B", "Jump (swim up in water)" }, { "L (hold)", "Sneak" },
		{ "ZL + D-Pad", "Quick keys" }, { "SELECT", "First / third person" }, { "START", "Menu: save, rest, options" },
		{ "Touch screen", "Inventory, magic, map, stats" } };
	// One stick (an Old 3DS without the C-stick) on the Xbox buttons: the pad walks and turns; an aim button held
	// stands still and the pad aims
	static const char* kGoldenEye[][2] = {
		{ "Circle Pad", "Walk (far: run) up, down; turn" }, { "A (hold) / ZL / ZR", "Aim: stand still, the pad aims" },
		{ "A (tap)", "Activate: talk, open, take" }, { "R (hold, release)", "Wind up, strike" },
		{ "L", "Cast the selected spell" }, { "X", "Ready / put away the weapon" },
		{ "Y", "Jump (swim up in water)" }, { "B", "Sneak on / off" }, { "SELECT", "First / third person" },
		{ "D-Pad", "Left / right: strafe; up / down: look" }, { "START", "Menu: save, rest, options" },
		{ "Touch screen", "Menus, quick keys" } };
	float y = 30;
	int n = xbox ? sizeof(kXbox) / sizeof(kXbox[0]) : goldenEye ? sizeof(kGoldenEye) / sizeof(kGoldenEye[0])
		: sizeof(kClassic) / sizeof(kClassic[0]);
	for (int i = 0; i < n; i++)
	{
		const char* const* l = xbox ? kXbox[i] : goldenEye ? kGoldenEye[i] : kClassic[i];
		uiText(14, y, 0.42f, col::header, l[0]);
		uiText(140, y, 0.42f, col::text, l[1]);
		y += 13.5f;
	}
	// Y: the next layout (Xbox, classic, GoldenEye); A / B: back to the Controls tab, on its Layout button
	if (uiButton(4, 208, 154, 28, xbox ? "Y: Use classic" : goldenEye ? "Y: Use Xbox" : "Y: Use GoldenEye") || (uiIn().down & KEY_Y))
	{
		g_controlLayout = xbox ? CONTROLS_CLASSIC : goldenEye ? CONTROLS_XBOX : CONTROLS_GOLDENEYE;
		saveSettings();
	}
	else if (uiButton(162, 208, 154, 28, w.game.gmst("sclose", "Close"), true) || (uiIn().down & (KEY_A | KEY_B)))
	{
		openScreen(SCR_LOOK);
		focus = kControlsButtonsFocus;
	}
}

// Options > Console (OpenMW's console, devconsole.cpp): the object the lines act on, what they printed, and the system
// keyboard for the next line. D-pad / C-stick up and down scroll the output; Edit last opens the keyboard on the line
// typed last
void Session::drawConsole()
{
	header("Console");
	const UiInput& in = uiIn();
	if (console.selected >= (int)w.refs.size())
		console.selected = -1;
	std::string sel = console.selectedName(w);
	uiText(8, 24, 0.4f, col::textDim, sel.empty() ? "Object: none (write Player->... for yourself)" : "Object: " + sel);
	// the output, wrapped, newest at the bottom
	const float kTop = 40.0f, kHeight = 162.0f, kScale = 0.4f;
	uiRect(4, kTop, 312, kHeight, col::panel);
	uiFrame(4, kTop, 312, kHeight);
	struct Row { std::string text; bool error; };
	std::vector<Row> rows;
	for (auto& l : console.lines)
		for (auto& part : uiWrap(l.text.empty() ? " " : l.text, 300.0f, kScale))
			rows.push_back({ part, l.error });
	float lh = uiLineHeight(kScale);
	int fit = (int)((kHeight - 6.0f) / lh);
	int maxBack = std::max(0, (int)rows.size() - fit);
	if (in.down & KEY_UP) consoleBack += 3.0f;
	if (in.down & KEY_DOWN) consoleBack -= 3.0f;
	if (in.touching)
		consoleBack += in.dragDY / lh;
	consoleBack -= uiStickScroll() / lh;
	consoleBack = fmaxf(0.0f, fminf((float)maxBack, consoleBack));
	int last = (int)rows.size() - (int)consoleBack;
	int first = std::max(0, last - fit);
	for (int i = first; i < last; i++)
		uiText(8, kTop + 3 + (i - first) * lh, kScale, rows[i].error ? col::health : col::text, rows[i].text);
	if (rows.empty())
	{
		uiText(8, kTop + 3, kScale, col::textDim, "Tap Type, write a line: OK on the keyboard runs it.");
		uiText(8, kTop + 3 + lh, kScale, col::textDim, "Player->AddItem gold_001 100, coc Balmora, tgm, sv");
	}
	// Type, Edit last, Clear, Close (focus 0..3: D-pad left / right)
	if (in.down & KEY_RIGHT) focus = focus >= 3 ? 0 : focus + 1;
	else if (in.down & KEY_LEFT) focus = focus <= 0 ? 3 : focus - 1;
	int b = -1;
	const std::string row[4] = { "Type", "Edit last", "Clear", w.game.gmst("sclose", "Close") };
	for (int i = 0; i < 4; i++)
		if (uiButton(4 + i * 79, 208, 75, 28, row[i], focus == i, i != 1 || !console.history.empty())
			|| (focus == i && (in.down & KEY_A)))
			b = i;
	if (b == 0 || (b == 1 && !console.history.empty()))
	{
		consoleDraft = b == 1 ? console.history.back() : "";
		wantText = TEXT_CONSOLE;
	}
	else if (b == 2)
	{
		console.lines.clear();
		console.runStart = 0;
		consoleBack = 0.0f;
	}
	else if (b == 3 || (in.down & KEY_B))
	{
		openScreen(SCR_OPTIONS);
		focus = kPrefsConsoleFocus;
	}
}

// Save a new game or load any save (autosave, your saves, bundled starting points)
void Session::drawSaves()
{
	header("Saves");
	int pick = uiSaveList(list, saveList, 4, 24, 312, 176, !deletingSave);
	if (saveList.empty())
		uiTextCentered(160, 100, 0.5f, col::textDim, "No saves yet");
	// Saving mid-chargen would lose the menus the scripts queued, so wait until they're done
	bool canSave = pendingMenus.empty() && !wantName && !playerDead;     // (dead: load only)
	bool canLoad = list.selected >= 0 && list.selected < (int)saveList.size();
	// (the starting points that come with the game stay)
	bool canDelete = canLoad && !saveList[list.selected].bundled;
	if (deletingSave)
	{
		// "Delete <name>?" Yes / No
		if (!canDelete)
			deletingSave = false;
		else
		{
			// (a solid box over the list, and its two buttons inside it, not across the whole screen as a button row)
			uiRect(20, 70, 280, 100, C2D_Color32(0, 0, 0, 255));
			uiPanel(20, 70, 280, 100);
			uiTextBox(scroll, 26, 76, 268, 50, w.game.gmstLabel("sdelete", "Delete") + " " + saveList[list.selected].title + "?", 0.5f, false);
			if (uiIn().down & (KEY_LEFT | KEY_RIGHT))
				focus = 1 - focus;
			int yn = -1;
			const std::string kAnswer[2] = { w.game.gmstLabel("syes", "Yes"), w.game.gmstLabel("sno", "No") };
			for (int i = 0; i < 2; i++)
				if (uiButton(34 + i * 132, 132, 120, 28, kAnswer[i], i == focus))
					yn = i;
			if (yn == 0)
			{
				remove(saveList[list.selected].path.c_str());
				remove((saveList[list.selected].path + ".meta").c_str());
				logf("saves: deleted %s", saveList[list.selected].path.c_str());
				saveList = savesList(dataDir);
				if (list.selected >= (int)saveList.size())
					list.selected = (int)saveList.size() - 1;
				notify("Save deleted.");
			}
			if (yn >= 0 || (uiIn().down & KEY_B))
			{
				deletingSave = false;
				focus = 0;
			}
		}
		return;
	}
	int b = buttonRow({ canSave ? "New save" : playerDead ? "(dead)" : "(finish chargen)", "Load", w.game.gmstLabel("sdelete", "Delete"),
		w.game.gmstLabel("sback", "Back") }, focus);
	if (b == 2 && canDelete)
	{
		deletingSave = true;
		focus = 1;               // (No)
	}
	else if (b == 0 && canSave)
	{
		std::string path = saveNewPath();
		if (w.saveGame(path.c_str()))
			notify("Game saved.");
		saveList = savesList(dataDir);
	}
	else if ((b == 1 || (pick >= 0 && (uiIn().down & KEY_A))) && canLoad)
	{
		reloadPath = saveList[list.selected].path;
		wantReload = true;
		closeScreen();
	}
	else if (b == 3 || (uiIn().down & KEY_B))
	{
		if (playerDead)
			openScreen(SCR_DEATH);     // back to the death screen: no way back into the game but a load
		else
			closeScreen();
	}
}

void Session::drawMessage()
{
	MessageState& m = messages.front();
	uiPanel(8, 8, 304, 224);
	const UiInput& in = uiIn();
	int n = m.buttons.size();
	if (m.text != messageShown)
	{
		messageShown = m.text;
		messageScroll = UiScroll();
	}
	// The text above a row of buttons when it fits there; else the buttons go to a column on the right and the
	// text gets the whole height on the left, scrolling a line at a time (drag it, or the circle pad) with a bar
	// that shows how much more there is
	float lh = uiLineHeight(0.5f);
	std::vector<std::string> lines = uiWrap(m.text, 284, 0.5f);
	float bh = 26.0f;
	messageSidebar = lines.size() * lh > 224 - n * (bh + 4) - 4 - 12;
	float tx = 14, tw = 292, ty = 12, th = 224 - n * (bh + 4) - 4 - 12;
	// (a column button's label wraps onto a second line rather than being cut short)
	const float bx = 188, bw = 118, bs = 0.42f;
	std::vector<std::vector<std::string>> labels(n);
	std::vector<float> by(n), bhs(n);
	if (messageSidebar)
	{
		tw = 166;
		th = 216;
		lines = uiWrap(m.text, tw - 8, 0.5f);
		float y = 16, blh = uiLineHeight(bs);
		for (int i = 0; i < n; i++)
		{
			labels[i] = uiWrap(m.buttons[i], bw - 10, bs);
			if (labels[i].size() > 2)
				labels[i].resize(2);
			by[i] = y;
			bhs[i] = fmaxf(24.0f, labels[i].size() * blh + 8);
			y += bhs[i] + 4;
		}
	}
	int rows = std::max(1, (int)(th / lh)), total = (int)lines.size();
	int maxTop = std::max(0, total - rows);
	if (in.touching && in.touchX >= tx && in.touchX < tx + tw && in.touchY >= ty && in.touchY < ty + th)
		messageScroll.scroll -= in.dragDY;
	messageScroll.scroll += uiStickScroll();
	messageScroll.scroll = fmaxf(0.0f, fminf(messageScroll.scroll, maxTop * lh));
	int first = std::min(maxTop, (int)(messageScroll.scroll / lh + 0.5f));
	for (int i = 0; i < rows && first + i < total; i++)
		uiText(tx + 4, ty + i * lh, 0.5f, col::text, lines[first + i]);
	if (maxTop > 0)
	{
		float bx = tx + tw + 2, trackH = rows * lh;
		uiRect(bx, ty, 3, trackH, col::panelLight);
		uiRect(bx, ty + trackH * first / total, 3, fmaxf(6.0f, trackH * rows / total), col::header);
	}
	messageTop = first;
	messageLeft = std::max(0, total - first - rows);
	if (in.down & (KEY_DOWN | KEY_RIGHT)) focus = (focus + 1) % n;
	if (in.down & (KEY_UP | KEY_LEFT)) focus = (focus + n - 1) % n;
	if (focus >= n) focus = 0;
	int hit = -1;
	for (int i = 0; i < n; i++)
	{
		bool pick;
		if (messageSidebar)
		{
			pick = uiButton(bx, by[i], bw, bhs[i], "", i == focus);
			float blh = uiLineHeight(bs), top = by[i] + (bhs[i] - labels[i].size() * blh) / 2;
			for (size_t k = 0; k < labels[i].size(); k++)
				uiTextCentered(bx + bw / 2, top + k * blh, bs, i == focus ? col::textOver : col::text, labels[i][k]);
		}
		else
			pick = uiButton(20, 224 - n * (bh + 4) + i * (bh + 4), 280, bh, m.buttons[i], i == focus);
		if (pick)
			hit = i;
	}
	if (hit >= 0)
	{
		playSound(-1, "Menu Click");
		if (m.fromScript)
			buttonPressed = hit;
		messages.erase(messages.begin());
		focus = 0;
	}
}

// ---- dialogue

// ---- tooltips

std::string Session::effectLine(const SpellEffect& e, bool showRange)
{
	std::string s = effectLabel(e.effect, e.skill, e.attribute);
	auto me = w.game.magicEffects.find(e.effect);
	int flags = me != w.game.magicEffects.end() ? me->second.flags : 0;
	char buf[64];
	if (!(flags & 0x8) && (e.min || e.max))
	{
		std::string unit = " " + (e.max == 1 ? w.game.gmstLabel("spoint", "pt") : w.game.gmstLabel("spoints", "pts"));
		if (e.min == e.max)
			snprintf(buf, sizeof(buf), " %d%s", e.min, unit.c_str());
		else
			snprintf(buf, sizeof(buf), " %d %s %d%s", e.min, w.game.gmstLabel("sto", "to").c_str(), e.max, unit.c_str());
		s += buf;
	}
	if (!(flags & 0x4) && e.duration > 1)
	{
		snprintf(buf, sizeof(buf), " for %d secs", e.duration);
		s += buf;
	}
	if (e.area > 0)
	{
		snprintf(buf, sizeof(buf), " in %d ft", e.area);
		s += buf;
	}
	if (showRange)
		s += e.range == 0 ? " on Self" : e.range == 1 ? " on Touch" : " on Target";
	return s;
}

void Session::spellTip(const SpellDef& sp)
{
	tip.clear();
	tip.emplace_back(sp.name, col::header);
	static const char* kSchool[6] = { "Alteration", "Conjuration", "Destruction", "Illusion", "Mysticism", "Restoration" };
	static const char* kSchoolKey[6] = { "sschoolalteration", "sschoolconjuration", "sschooldestruction", "sschoolillusion",
		"sschoolmysticism", "sschoolrestoration" };
	int school = spellSchool(sp);
	if (sp.type == 0 && school >= 0 && school < 6)
		tip.emplace_back(w.game.gmstLabel("sschool", "School") + ": " + w.game.gmstLabel(kSchoolKey[school], kSchool[school]),
			col::textDim);
	for (auto& e : sp.effects)
		tip.emplace_back(effectLine(e, true), col::text);
}

void Session::itemTip(const Object* o, const InventoryItem* it)
{
	tip.clear();
	if (!o)
		return;
	tip.emplace_back(o->name, col::header);
	char buf[96];
	std::string typeLabel = w.game.gmst("stype", "Type");
	if (typeLabel.empty() || typeLabel.back() != ':')
		typeLabel += ":";
	// Its kind, as OpenMW's tooltips name it: a weapon's skill, one or two handed for the melee ones ("Type: Long
	// Blade, Two Handed"); armor's weight class (Light / Medium / Heavy Armor)
	if (o->type == "WEAP" && o->subtype >= 0 && o->subtype <= WEAP_BOLT)
	{
		static const int kSkill[14] = { 22, 5, 5, 4, 4, 4, 7, 6, 6, 23, 23, 23, 23, 23 };
		std::string kind = typeLabel + " " + skillName(w, kSkill[o->subtype]);
		if (o->subtype < WEAP_BOW)
			kind += ", " + (o->subtype == 0 || o->subtype == 1 || o->subtype == 3 || o->subtype == 7
				? w.game.gmst("sonehanded", "One Handed") : w.game.gmst("stwohanded", "Two Handed"));
		tip.emplace_back(kind, col::textDim);
	}
	else if (o->type == "ARMO" && o->subtype >= 0 && o->subtype < 11)
	{
		tip.emplace_back(typeLabel + " " + skillName(w, armorSkill(o)), col::textDim);    // Light / Medium / Heavy Armor
	}
	if (o->type == "WEAP")
	{
		bool ranged = o->subtype >= WEAP_BOW && o->subtype <= WEAP_BOLT;
		if (ranged)
			snprintf(buf, sizeof(buf), "Attack: %d - %d", o->chop[0], o->chop[1]), tip.emplace_back(buf, col::text);
		else
		{
			snprintf(buf, sizeof(buf), "Chop: %d - %d", o->chop[0], o->chop[1]), tip.emplace_back(buf, col::text);
			snprintf(buf, sizeof(buf), "Slash: %d - %d", o->slash[0], o->slash[1]), tip.emplace_back(buf, col::text);
			snprintf(buf, sizeof(buf), "Thrust: %d - %d", o->thrust[0], o->thrust[1]), tip.emplace_back(buf, col::text);
		}
	}
	else if (o->type == "ARMO")
	{
		snprintf(buf, sizeof(buf), "Armor Rating: %d", o->armor);
		tip.emplace_back(buf, col::text);
	}
	if ((o->type == "WEAP" || o->type == "ARMO") && o->health > 0)
	{
		int cond = it && it->condition >= 0 ? it->condition : o->health;
		snprintf(buf, sizeof(buf), "Condition: %d/%d", cond, o->health);
		tip.emplace_back(buf, col::text);
	}
	if (o->type == "LOCK" || o->type == "PROB" || o->type == "REPA")
	{
		int uses = it && it->condition >= 0 ? it->condition : o->uses;
		snprintf(buf, sizeof(buf), "Quality: %.2f   Uses: %d", o->quality, uses);
		tip.emplace_back(buf, col::text);
	}
	if (o->type == "APPA")
	{
		snprintf(buf, sizeof(buf), "Quality: %.2f", o->quality);
		tip.emplace_back(buf, col::text);
	}
	snprintf(buf, sizeof(buf), "Weight: %.1f   Value: %d", o->weight, o->value);
	tip.emplace_back(buf, col::text);
	if (it && !it->soul.empty())
	{
		std::string soul = it->soul;
		for (auto& a : w.game.actors)
			if (lower(a.id) == it->soul)
				soul = a.name;
		tip.emplace_back(w.game.gmstLabel("ssoulgem", "Soul") + ": " + soul, col::text);
	}
	// potions: their effects; ingredients: as many as Alchemy reveals (fWortChanceValue each)
	if (o->type == "ALCH")
		for (auto& e : o->effects)
			tip.emplace_back(effectLine(e, false), col::text);
	if (o->type == "INGR")
	{
		int known = (int)(w.stats.skills[16] / w.game.gmstf("fwortchancevalue", 15.0f));
		for (size_t k = 0; k < o->ingredient.size(); k++)
			tip.emplace_back((int)k < known ? effectLabel(o->ingredient[k].effect, o->ingredient[k].skill, o->ingredient[k].attribute)
				: std::string("?"), (int)k < known ? col::text : col::textDim);
	}
	if (o->type == "BOOK" && o->bookSkill >= 0)
		tip.emplace_back(o->scroll ? "Scroll" : "Book", col::textDim);
	// enchantment: how it's cast, its charge and effects
	auto en = o->ench.empty() ? w.game.spells.end() : w.game.spells.find(o->ench);
	if (en != w.game.spells.end())
	{
		const SpellDef& sp = en->second;
		static const char* kHow[4] = { "Cast Once", "Cast When Strikes", "Cast When Used", "Constant Effect" };
		static const char* kHowKey[4] = { "sitemcastonce", "sitemcastwhenstrikes", "sitemcastwhenused", "sitemcastconstant" };
		int t = sp.type - ENCH_ONCE;
		tip.emplace_back(t >= 0 && t < 4 ? w.game.gmstLabel(kHowKey[t], kHow[t]) : "Enchanted", col::header);
		for (auto& e : sp.effects)
			tip.emplace_back(effectLine(e, t != 3), col::text);
		if (t == 1 || t == 2)
		{
			float charge = it ? w.chargeOf(*it) : (float)sp.charge;
			snprintf(buf, sizeof(buf), "Charge: %d/%d", (int)charge, sp.charge);
			tip.emplace_back(buf, col::text);
		}
	}
}

// The crosshair name for the top screen: wrapped within 340 pixels, so "Hut Door - Ald-ruhn, Tiras Sadus: General
// Merchandise" shows whole (it ran off both edges of the 400-pixel screen)
std::vector<std::string> Session::targetPopupLines(int ref)
{
	return uiWrap(targetLabel(ref), 340.0f, 0.55f);
}

// What A does to what is aimed at, for the bottom screen: short of the date under the map (from x 190), so within 180
// pixels, over two lines at most, the second cut with ".." when even that is not enough
std::vector<std::string> Session::targetPromptLines(int ref, float* scale)
{
	const Ref& t = w.refs[ref];
	// (a creature alive: "Talk to" one with words of its own, else only its name, as OpenMW shows it)
	bool talks = t.type == "CREA" && t.actor >= 0 && ((w.game.actors[t.actor].services & 0x3FFFF) || w.game.speakers.count(t.idLower));
	std::string verb = t.dead ? "Search " : t.type == "NPC_" || talks ? "Talk to " : t.type == "CREA" ? "" : t.type == "DOOR" ? "Open " : "Use ";
	std::string use = "A: " + verb + targetLabel(ref);
	// one line at the usual size; else two, a little smaller, and smaller still before anything is cut
	float s = 0.45f;
	std::vector<std::string> lines = uiWrap(use, 180.0f, s);
	for (float step : { 0.42f, 0.36f })
		if (lines.size() > 1)
		{
			s = step;
			lines = uiWrap(use, 180.0f, s);
			if (lines.size() <= 2)
				break;
		}
	if (scale)
		*scale = s;
	if (lines.size() > 2)
	{
		std::string rest;
		for (size_t i = 1; i < lines.size(); i++)
			rest += (i > 1 ? " " : "") + lines[i];
		lines.resize(2);
		lines[1] = rest;
	}
	while (lines.size() == 2 && lines[1].size() > 3 && uiTextWidth(lines[1], s) > 180.0f)
		lines[1] = lines[1].substr(0, lines[1].size() - 4) + "...";
	return lines;
}
