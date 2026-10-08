#include <cmath>
#include <cstdio>
#include <cstring>

#include "audio.h"
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
	return { o ? o->iconIx : -1, count, flags, o ? o->name : id };
}

// Morrowind's inventory filters: All, Weapon, Apparel, Magic, Misc
const char* kTabs[5] = { "All", "Weapon", "Apparel", "Magic", "Misc" };

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
		std::string label = targetLabel(target);
		float tw = uiTextWidth(label, 0.55f);
		uiRect(200 + ax - tw / 2 - 6, 132, tw + 12, 20, col::border);
		uiRect(200 + ax - tw / 2 - 5, 133, tw + 10, 18, col::panel);
		uiTextCentered(200 + ax, 134, 0.55f, w.ownedByOther(target) ? col::health : col::text, label);
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
	drawPrecipitation();
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
		uiTextCentered(200, 18, 0.4f, breath > 0.0f ? col::text : col::health, breath > 0.0f ? "Breath" : "Drowning!");
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
	case SCR_OPTIONS: drawOptions(); break;
	case SCR_CONTROLS: drawControls(); break;
	case SCR_REPAIR: drawRepair(); break;
	case SCR_SAVES: drawSaves(); break;
	case SCR_GAMEMENU: drawGameMenu(); break;
	default: break;
	}
}

// The four quick keys as small slots above the buttons (Up, Right, Down, Left): the item's icon or the spell's
// first letters; a tap uses the slot as its D-pad press does
void Session::drawQuickStrip()
{
	static const char* dirs[4] = { "U", "R", "D", "L" };
	for (int k = 0; k < 4; k++)
	{
		float x = 190 + k * 19, y = 192;
		const std::string& q = w.stats.quickKeys[k];
		bool isSpell = q.compare(0, 6, "magic:") == 0 && q.compare(0, 11, "magic:item:") != 0;
		std::string id = q.compare(0, 4, "inv:") == 0 ? q.substr(4) : q.compare(0, 11, "magic:item:") == 0 ? q.substr(11)
			: q.compare(0, 6, "magic:") == 0 ? q.substr(6) : q;
		const Object* o = q.empty() || isSpell ? nullptr : w.game.object(id);
		uiRect(x, y, 16, 16, col::panel);
		if (!q.empty() && !(o && uiIcon(o->iconIx, x, y, 16.0f)))
		{
			std::string name = isSpell ? (w.game.spells.count(id) ? w.game.spells[id].name : id) : o ? o->name : id;
			uiTextCentered(x + 8, y + 8 - uiLineHeight(0.34f) / 2, 0.34f, isSpell ? col::link : col::textDim, name.substr(0, 3));
		}
		uiFrame(x, y, 16, 16);
		uiText(x + 1, y - 1, 0.3f, col::textOver, dirs[k]);
		if (uiHit(x, y, 16, 16) && w.controlsEnabled && !playerDead)
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
		uiTextCentered(24, by + 18 - uiLineHeight(0.36f) / 2, 0.36f, col::textDim, wo ? wo->name.substr(0, 3) : "Fist");
	uiFrame(6, by, 36, 36);
	uiText(48, by + 1, 0.42f, col::text, wo ? wo->name : "Hand-to-hand");
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
	uiTextCentered(250, 156, 0.38f, col::textDim, w.cellName().size() > 26 ? w.cellName().substr(0, 25) + "..." : w.cellName());

	if (target >= 0)
	{
		const Ref& t = w.refs[target];
		// (a creature alive: "Talk to" one with words of its own, else only its name, as OpenMW shows it)
		bool talks = t.type == "CREA" && t.actor >= 0 && ((w.game.actors[t.actor].services & 0x3FFFF) || w.game.speakers.count(t.idLower));
		std::string verb = t.dead ? "Search " : t.type == "NPC_" || talks ? "Talk to " : t.type == "CREA" ? "" : t.type == "DOOR" ? "Open " : "Use ";
		// short of the date under the map (from x 190): a long name ("Ajira's Mushroom Report") ran into it
		std::string use = "A: " + verb + targetLabel(target);
		while (use.size() > 3 && uiTextWidth(use, 0.45f) > 180)
			use = use.substr(0, use.size() - 4) + "...";
		uiText(6, 172, 0.45f, col::link, use);
	}
	if (w.fightingEnabled)
	{
		bool xbox = g_controlLayout == CONTROLS_XBOX;
		std::string hint = w.stats.selectedSpell.empty() ? (xbox ? "R: attack (hold to wind up)" : "X: attack (hold to wind up)")
			: std::string(xbox ? "R: attack   L: cast " : "X: attack   Y: cast ") + w.game.spells[w.stats.selectedSpell].name;
		// Leave the quick key strip (from x 190) clear
		while (hint.size() > 3 && uiTextWidth(hint, 0.38f) > 180)
			hint = hint.substr(0, hint.size() - 4) + "...";
		uiText(6, 190, 0.38f, col::textDim, hint);
	}
	drawQuickStrip();
	uiTextCentered(250, 168, 0.38f, col::textDim, dateText());
	if (w.bounty > 0)
		uiTextRight(314, 181, 0.4f, col::health, "Bounty " + std::to_string(w.bounty));
	char perf[64];
	snprintf(perf, sizeof(perf), "%.0f fps", fps);
	uiTextRight(314, 194, 0.34f, col::textDim, perf);

	bool inv = w.menusEnabled & MENU_INVENTORY, stats = w.menusEnabled & MENU_STATS;
	if (uiButton(2, 208, 51, 28, "Items", false, inv))
		openScreen(SCR_INVENTORY);
	if (uiButton(55, 208, 51, 28, "Magic", false, (w.menusEnabled & MENU_MAGIC) || !w.game.chargen))
		openScreen(SCR_MAGIC);
	if (uiButton(108, 208, 51, 28, "Journal", false, !w.journal.empty()))
		openScreen(SCR_JOURNAL);
	if (uiButton(161, 208, 51, 28, "Map", false, !w.game.map.file.empty() && ((w.menusEnabled & MENU_MAP) || !w.game.chargen)))
		openScreen(SCR_MAP);
	if (uiButton(214, 208, 51, 28, "Stats", false, stats))
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
	header("Menu", w.cellName());
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
	{
		save();
		wantQuit = true;
	}
}

// Options: 3D, difficulty, effects and music volume (D-pad up / down picks, left / right or a tap on the
// bar sets); kept in settings.txt
void Session::drawOptions()
{
	header(w.game.gmst("soptions", "Options"));
	const UiInput& in = uiIn();
	// focus: 0..2 the bars, 3..5 the toggles (3D, combat rules, hit numbers), 6..8 the buttons along the bottom
	if (in.down & KEY_DOWN) focus = focus >= 6 ? 0 : focus >= 3 ? 6 : focus + 1;
	else if (in.down & KEY_UP) focus = focus == 0 ? 6 : focus >= 6 ? 3 : focus >= 3 ? 2 : focus - 1;
	else if (focus >= 3 && focus <= 5 && (in.down & KEY_RIGHT)) focus = focus == 5 ? 3 : focus + 1;
	else if (focus >= 3 && focus <= 5 && (in.down & KEY_LEFT)) focus = focus == 3 ? 5 : focus - 1;
	bool changed = false;
	const char* names[3] = { "Difficulty", "Effects volume", "Music volume" };
	int* vals[3] = { &difficulty, &effectsVolume, &musicVolume };
	const int lo[3] = { -100, 0, 0 }, hi[3] = { 100, 100, 100 }, step[3] = { 10, 10, 10 };
	for (int i = 0; i < 3; i++)
	{
		float y = 34 + i * 44;
		uiText(20, y, 0.45f, i == focus ? col::header : col::text, names[i] + std::string(": ") + std::to_string(*vals[i]));
		float bx = 20, bw = 280, by = y + 18;
		uiRect(bx, by, bw, 8, col::panel);
		uiFrame(bx, by, bw, 8);
		float f = (float)(*vals[i] - lo[i]) / (hi[i] - lo[i]);
		uiRect(bx + f * bw - 4, by - 5, 8, 18, col::header);
		int v = *vals[i];
		if (i == focus && (in.down & KEY_LEFT)) v -= step[i];
		if (i == focus && (in.down & KEY_RIGHT)) v += step[i];
		if (in.touching && in.touchY >= by - 8 && in.touchY <= by + 16 && in.touchX >= bx - 6 && in.touchX <= bx + bw + 6)
		{
			focus = i;
			v = lo[i] + (int)roundf(fmaxf(0.0f, fminf(1.0f, (in.touchX - bx) / bw)) * (hi[i] - lo[i]) / step[i]) * step[i];
		}
		v = v < lo[i] ? lo[i] : v > hi[i] ? hi[i] : v;
		if (v != *vals[i])
		{
			*vals[i] = v;
			changed = true;
		}
	}
	const char* stereo = stereoMode == STEREO_OFF ? "3D: Off" : "3D: On";
	if (uiButton(4, 170, 100, 26, stereo, focus == 3) || (focus == 3 && (in.down & KEY_A)))
	{
		stereoMode = stereoMode == STEREO_OFF ? STEREO_ON : STEREO_OFF;
		changed = true;
	}
	// Combat: vanilla, or Glancing Blows and Crits (misses glance, crits; the same average damage)
	const char* rules = combatMode == COMBAT_GBAC ? "Glance+Crit" : "Vanilla hits";
	if (uiButton(110, 170, 100, 26, rules, focus == 4) || (focus == 4 && (in.down & KEY_A)))
	{
		combatMode = combatMode == COMBAT_GBAC ? COMBAT_VANILLA : COMBAT_GBAC;
		notify(combatMode == COMBAT_GBAC ? "Glancing blows and crits: misses glance for half damage, strong hits can crit."
			: "Vanilla combat.");
		changed = true;
	}
	// Hit & Miss Indicators: damage numbers and misses by the crosshair
	if (uiButton(216, 170, 100, 26, hitIndicators ? "Numbers: On" : "Numbers: Off", focus == 5)
		|| (focus == 5 && (in.down & KEY_A)))
	{
		hitIndicators = !hitIndicators;
		notify(hitIndicators ? "Hit and miss numbers on." : "Hit and miss numbers off.");
		changed = true;
	}
	if (changed)
	{
		audioSetScales(effectsVolume / 100.0f, musicVolume / 100.0f);
		saveSettings();
	}
	// Controls, Credits, Close (focus 4..6: D-pad left / right between them)
	int b = -1;
	const char* row[3] = { "Controls", "Credits", "Close" };
	for (int i = 0; i < 3; i++)
		if (uiButton(4 + i * 106, 208, 100, 28, i == 2 ? w.game.gmst("sclose", "Close") : row[i], focus == 6 + i)
			|| (focus == 6 + i && (in.down & KEY_A)))
			b = i;
	if (focus >= 6 && (in.down & KEY_RIGHT)) focus = focus == 8 ? 6 : focus + 1;
	else if (focus >= 6 && (in.down & KEY_LEFT)) focus = focus == 6 ? 8 : focus - 1;
	if (b == 0)
	{
		closeScreen();
		openScreen(SCR_CONTROLS);
	}
	else if (b == 1)
	{
		// Morrowind's credits are its movie (Video\mw_credits.bik): data converted without it keeps Options open and
		// says so, rather than dropping back into the game as if the button did nothing
		if (playMovie("mw_credits"))
			closeScreen();
		else
			notify("The credits movie is missing: Video\\mw_credits.bik was not converted with the game data.");
	}
	else if (b == 2 || (in.down & KEY_B))
		closeScreen();
}

// What each button does
void Session::drawControls()
{
	bool xbox = g_controlLayout == CONTROLS_XBOX;
	header(xbox ? "Controls (Xbox layout)" : "Controls (classic layout)");
	static const char* kXbox[][2] = {
		{ "Circle Pad / C-Stick", "Move / look" }, { "A", "Activate: talk, open, take" },
		{ "R (hold, release)", "Wind up, strike" }, { "X", "Ready / put away the weapon" }, { "L", "Cast the selected spell" },
		{ "Y", "Jump (swim up in water)" }, { "B (hold)", "Run / walk" }, { "ZL (hold)", "Sneak" },
		{ "D-Pad", "Quick keys" }, { "SELECT", "First / third person" }, { "START", "Menu: save, rest, options" },
		{ "Touch screen", "Inventory, magic, map, stats" } };
	static const char* kClassic[][2] = {
		{ "Circle Pad", "Move" }, { "C-Stick / D-Pad", "Look" }, { "A", "Activate: talk, open, take" },
		{ "X (hold, release)", "Ready weapon, wind up, strike" }, { "ZL + X", "Put the weapon away" }, { "Y", "Cast the selected spell" },
		{ "B", "Jump (swim up in water)" }, { "ZR (hold)", "Run / walk" }, { "L (hold)", "Sneak" },
		{ "ZL + D-Pad", "Quick keys" }, { "SELECT", "First / third person" }, { "START", "Menu: save, rest, options" },
		{ "Touch screen", "Inventory, magic, map, stats" } };
	float y = 30;
	int n = xbox ? sizeof(kXbox) / sizeof(kXbox[0]) : sizeof(kClassic) / sizeof(kClassic[0]);
	for (int i = 0; i < n; i++)
	{
		const char* const* l = xbox ? kXbox[i] : kClassic[i];
		uiText(14, y, 0.42f, col::header, l[0]);
		uiText(140, y, 0.42f, col::text, l[1]);
		y += 13.5f;
	}
	// Y: the other layout; A / B: back to Options
	if (uiButton(40, 208, 130, 28, xbox ? "Y: Use classic" : "Y: Use Xbox") || (uiIn().down & KEY_Y))
	{
		g_controlLayout = xbox ? CONTROLS_CLASSIC : CONTROLS_XBOX;
		saveSettings();
	}
	else if (uiButton(180, 208, 100, 28, w.game.gmst("sclose", "Close"), true) || (uiIn().down & (KEY_A | KEY_B)))
	{
		closeScreen();
		openScreen(SCR_OPTIONS);
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
	bool canSave = pendingMenus.empty() && !wantName;
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
			uiPanel(20, 70, 280, 100);
			uiTextBox(scroll, 26, 76, 268, 50, "Delete " + saveList[list.selected].title + "?", 0.5f, false);
			int yn = buttonRow({ "Yes", "No" }, focus, 130.0f);
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
	int b = buttonRow({ canSave ? "New save" : "(finish chargen)", "Load", "Delete", "Back" }, focus);
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
		closeScreen();
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
		const char* unit = e.max == 1 ? " pt" : " pts";
		if (e.min == e.max)
			snprintf(buf, sizeof(buf), " %d%s", e.min, unit);
		else
			snprintf(buf, sizeof(buf), " %d to %d%s", e.min, e.max, unit);
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
	int school = spellSchool(sp);
	if (sp.type == 0 && school >= 0 && school < 6)
		tip.emplace_back(std::string("School: ") + kSchool[school], col::textDim);
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
		tip.emplace_back("Soul: " + soul, col::text);
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
		int t = sp.type - ENCH_ONCE;
		tip.emplace_back(t >= 0 && t < 4 ? kHow[t] : "Enchanted", col::header);
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
