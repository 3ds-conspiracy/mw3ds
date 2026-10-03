#include <algorithm>
#include <cmath>
#include <cstdio>

#include "log.h"
#include "screens.h"
#include "session.h"

// Morrowind's rest, wait and level-up rules (as OpenMW has them), skill increases and the calendar

static const char* kMonthKeys[12] = { "smonthmorningstar", "smonthsunsdawn", "smonthfirstseed", "smonthrainshand",
	"smonthsecondseed", "smonthmidyear", "smonthsunsheight", "smonthlastseed", "smonthheartfire", "smonthfrostfall",
	"smonthsunsdusk", "smontheveningstar" };
static const char* kMonthNames[12] = { "Morning Star", "Sun's Dawn", "First Seed", "Rain's Hand", "Second Seed",
	"Midyear", "Sun's Height", "Last Seed", "Hearthfire", "Frostfall", "Sun's Dusk", "Evening Star" };

// Hours of rest one frame passes while the rest screen counts them off
static const int kRestFramesPerHour = 4;

// "16 Last Seed, 9 a.m."
std::string Session::dateText(bool withHour)
{
	GameDate d = w.date();
	std::string t = std::to_string(d.day) + " " + w.game.gmst(kMonthKeys[d.month], kMonthNames[d.month]);
	if (withHour)
	{
		int h = (int)d.hour;
		t += ", " + std::to_string(h % 12 == 0 ? 12 : h % 12) + " "
			+ w.game.gmst(h < 12 ? "ssavemenuhelp04" : "ssavemenuhelp05", h < 12 ? "a.m." : "p.m.");
	}
	return t;
}

// ---- Skills and levels

int Session::levelUpTotal()
{
	return (int)w.game.gmstf("ileveluptotal", 10.0f);
}

// Progress one increase takes: (skill + 1) x fMajor / fMinor / fMiscSkillBonus, x fSpecialSkillBonus in
// the class's specialization
float Session::skillNeed(int skill)
{
	const ClassDef* c = w.playerClass();
	if (!c || skill < 0 || skill >= 27 || skill >= (int)w.game.skills.size())
		return 1e9f;
	float factor = w.game.gmstf("fmiscskillbonus", 1.25f);
	for (int i = 0; i < 5; i++)
	{
		if (c->minor[i] == skill)
			factor = w.game.gmstf("fminorskillbonus", 1.0f);
		if (c->major[i] == skill)
			factor = w.game.gmstf("fmajorskillbonus", 0.75f);
	}
	float need = (baseSkill(skill) + 1) * factor;         // OpenMW: 1 + the BASE skill (Fortify / Drain don't count)
	if (w.game.skills[skill].specialization == c->specialization)
		need *= w.game.gmstf("fspecialskillbonus", 0.8f);
	return need;
}

// One point more in a skill (use, training, a skill book): major and minor skills bring the next level
// closer, and every skill counts towards its governing attribute's multiplier at the level up
void Session::raiseSkill(int skill, bool fromBook)
{
	PlayerStats& s = w.stats;
	const ClassDef* c = w.playerClass();
	if (skill < 0 || skill >= 27 || skill >= (int)w.game.skills.size() || baseSkill(skill) >= 100 || !c)
		return;
	s.skillBonus[skill]++;
	s.skills[skill]++;
	bool major = false, minor = false;
	for (int i = 0; i < 5; i++)
	{
		major |= c->major[i] == skill;
		minor |= c->minor[i] == skill;
	}
	if (major)
		s.levelProgress += (int)w.game.gmstf("ilevelupmajormult", 1.0f);
	else if (minor)
		s.levelProgress += (int)w.game.gmstf("ilevelupminormult", 1.0f);
	int attr = w.game.skills[skill].attribute;
	if (attr >= 0 && attr < 8)
		s.attrSkillUps[attr] += (int)(major ? w.game.gmstf("ilevelupmajormultattribute", 1.0f)
			: minor ? w.game.gmstf("ilevelupminormultattribute", 1.0f) : w.game.gmstf("ilevelupmiscmultattriubte", 1.0f));

	playSound(-1, "skillraise");
	if (fromBook)
		notify(w.game.gmst("sbookskillmessage", "You have gained knowledge from this book."));
	// "Your %s skill increased to %d."
	std::string msg = w.game.gmst("snotifymessage39", "Your %s skill increased to %d.");
	size_t at = msg.find("%s");
	if (at != std::string::npos)
		msg.replace(at, 2, skillName(w, skill));
	at = msg.find("%d");
	if (at != std::string::npos)
		msg.replace(at, 2, std::to_string(s.skills[skill]));
	notify(msg);
	if ((major || minor) && s.levelProgress >= levelUpTotal())
		notify(w.game.gmst("slevelupmsg", "You should rest and meditate on what you've learned."));
	logf("skill %d -> %d (level progress %d)", skill, s.skills[skill], s.levelProgress);
}

// A skill book's first read teaches its skill
void Session::readBook(const Object* o)
{
	if (o && o->bookSkill >= 0 && w.stats.booksRead.insert(lower(o->id)).second)
		raiseSkill(o->bookSkill, true);
}

// The level up's bonus for an attribute: x1 with no skill increases under it, else iLevelUp01Mult ..
// iLevelUp10Mult (2, 2, 2, 2, 3, 3, 3, 4, 4, 5)
int Session::attrMultiplier(int attr)
{
	int n = w.stats.attrSkillUps[attr];
	if (n <= 0)
		return 1;
	n = n > 10 ? 10 : n;
	char key[24];
	snprintf(key, sizeof(key), "ilevelup%02dmult", n);
	return (int)w.game.gmstf(key, n >= 10 ? 5 : n >= 8 ? 4 : n >= 5 ? 3 : 2);
}

void Session::applyLevelUp()
{
	PlayerStats& s = w.stats;
	std::string raised;
	for (int a : levelPicks)
	{
		int m = attrMultiplier(a);
		s.attrBonus[a] += m;
		raised += " " + attrName(w, a) + " +" + std::to_string(m);
	}
	s.level++;
	s.levelProgress = std::max(0, s.levelProgress - levelUpTotal());
	for (int i = 0; i < 8; i++)
		s.attrSkillUps[i] = 0;
	float health = s.health, magicka = s.magicka, fatigue = s.fatigue;
	w.recomputeStats();
	// Health grows by a tenth of the endurance (as raised just now), at once and for good
	// (from the base Endurance: level-up raises count, Fortify and Drain do not)
	float gain = w.game.gmstf("fleveluphealthendmult", 0.1f) * std::min(100, s.attrCreation[ATTR_ENDURANCE] + s.attrBonus[ATTR_ENDURANCE]);
	s.healthBonus += gain;
	w.recomputeStats();
	s.health = fminf(s.healthMax, health + gain);
	s.magicka = fminf(s.magickaMax, magicka);
	s.fatigue = fminf(s.fatigueMax, fatigue);
	levelPicks.clear();
	logf("level up: %d,%s, health +%.1f", s.level, raised.c_str(), gain);
}

// Morrowind's level-up window: the new level's words (Morrowind.ini), then three attributes to raise,
// each by its multiplier
void Session::drawLevelUp()
{
	PlayerStats& s = w.stats;
	const UiInput& in = uiIn();
	int next = s.level + 1;
	uiCaption(w.game.gmst("slevelup", "Level Up"), w.game.gmst("slevel", "Level") + " " + std::to_string(next));
	auto words = w.game.levelUpText.find("level" + std::to_string(next));
	if (words == w.game.levelUpText.end())
		words = w.game.levelUpText.find("default");
	std::string text = w.game.gmst("slevelupmenu1", "You have ascended to Level ") + std::to_string(next) + ".";
	if (words != w.game.levelUpText.end())
		text += "\n" + words->second;
	uiPanel(4, 24, 312, 68);
	// Morrowind's picture for the class, when it has one, beside the words
	auto art = w.game.art.levelup.find(lower(s.cls));
	bool pic = art != w.game.art.levelup.end() && art->second.h > 0;
	if (pic)
	{
		float ph = 64.0f, pw = art->second.w * ph / art->second.h;
		uiArt(art->second.file, art->second.w, art->second.h, 314 - pw, 26, pw, ph);
		uiTextBox(scroll, 6, 26, 308 - pw - 4, 64, text, 0.4f);
	}
	else
		uiTextBox(scroll, 6, 26, 308, 64, text, 0.4f);

	int open = 0;
	for (int i = 0; i < 8; i++)
		open += s.attributes[i] < 100;
	size_t need = open < 3 ? open : 3;

	// D-pad: two columns of four, the OK button below them
	if (in.down & KEY_DOWN)
		levelCursor = levelCursor == 8 || levelCursor % 4 == 3 ? 8 : levelCursor + 1;
	if (in.down & KEY_UP)
		levelCursor = levelCursor == 8 ? 3 : levelCursor % 4 == 0 ? levelCursor : levelCursor - 1;
	if ((in.down & (KEY_LEFT | KEY_RIGHT)) && levelCursor < 8)
		levelCursor = (levelCursor + 4) % 8;
	int toggle = -1;
	if ((in.down & KEY_A) && levelCursor < 8)
		toggle = levelCursor;
	for (int i = 0; i < 8; i++)
	{
		float x = 6 + (i / 4) * 156, y = 98 + (i % 4) * 26;
		bool can = s.attributes[i] < 100;
		bool picked = std::find(levelPicks.begin(), levelPicks.end(), i) != levelPicks.end();
		uiRect(x, y, 152, 24, picked ? col::select : col::panelLight);
		uiFrame(x, y, 152, 24, "menu_button_frame", i == levelCursor ? col::header : col::border);
		if (i == levelCursor)
			uiFrame(x + 1, y + 1, 150, 22, "menu_thin_border", col::header);
		u32 c = !can ? col::textDim : picked ? col::textPressed : col::text;
		uiText(x + 6, y + 4, 0.45f, c, attrName(w, i));
		uiTextRight(x + 114, y + 4, 0.45f, c, std::to_string(s.attributes[i]));
		int m = attrMultiplier(i);
		if (m > 1 && can)
			uiTextRight(x + 146, y + 4, 0.45f, picked ? col::textPressed : col::header, "x" + std::to_string(m));
		if (can && uiHit(x, y, 152, 24))
		{
			toggle = i;
			levelCursor = i;
		}
	}
	if (toggle >= 0 && s.attributes[toggle] < 100)
	{
		// Picking a fourth replaces the last one picked (OpenMW's rule)
		auto it = std::find(levelPicks.begin(), levelPicks.end(), toggle);
		if (it != levelPicks.end())
			levelPicks.erase(it);
		else if (levelPicks.size() < need)
			levelPicks.push_back(toggle);
		else if (!levelPicks.empty())
			levelPicks.back() = toggle;
		playSound(-1, "Menu Click");
	}
	if (uiButton(110, 206, 100, 30, w.game.gmst("sok", "OK"), levelCursor == 8, levelPicks.size() == need))
	{
		applyLevelUp();
		closeScreen();
	}
}

// ---- Rest and wait

bool Session::enemiesNear()
{
	bool enemies = false;
	w.forLoadedRefs([&](int i) {
		const Ref& r = w.refs[i];
		enemies |= r.ai == AI_COMBAT && !r.dead && w.distanceToPlayer(i) < 3000.0f;
	});
	return enemies;
}

// Morrowind's refusals first (World::canRest): enemies about, in the air or in water. Where sleeping is
// illegal only waiting is offered, except in a bed.
void Session::openRest(bool bed)
{
	if (enemiesNear())
	{
		notify(w.game.gmst("snotifymessage2", "You can't rest here enemies are nearby."));
		return;
	}
	if ((!w.player.onGround && !w.player.flying) || w.player.swimming)
	{
		notify(w.game.gmst("snotifymessage1", "You can only rest on solid ground."));
		return;
	}
	restBed = bed;
	restSleep = bed || w.current < 0 || !w.cells[w.current].noSleep;
	restDone = -1;
	restHours = restHours < 1 ? 1 : restHours > 24 ? 24 : restHours;
	openScreen(SCR_REST);
	focus = restSleep ? 1 : 0;
}

// Hours of sleep until health and magicka are full (Actors::getHoursToRest)
static int hoursToHeal(World& w)
{
	const PlayerStats& s = w.stats;
	float hp = 0.1f * s.attributes[ATTR_ENDURANCE];
	float mp = w.game.gmstf("frestmagicmult", 0.15f) * s.attributes[ATTR_INTELLIGENCE];
	float hh = hp > 0.0f ? (s.healthMax - s.health) / hp : 1.0f;
	float mh = mp > 0.0f ? (s.magickaMax - s.magicka) / mp : 1.0f;
	return (int)ceilf(fmaxf(1.0f, fmaxf(hh, mh)));
}

// One hour passes. Asleep: a tenth of endurance in health, fRestMagicMult x intelligence in magicka.
// Awake or asleep: fatigue comes back, and spell effects run an hour of their course.
void Session::restHour()
{
	PlayerStats& s = w.stats;
	w.gameHour += 1.0f;
	w.syncTime();
	int endurance = s.attributes[ATTR_ENDURANCE];
	if (restSleep)
	{
		s.health = fminf(s.healthMax, s.health + 0.1f * endurance);
		if (w.effectTotal(136) <= 0.0f)          // Stunted Magicka: none back from rest
			s.magicka = fminf(s.magickaMax, s.magicka + w.game.gmstf("frestmagicmult", 0.15f) * s.attributes[ATTR_INTELLIGENCE]);
	}
	// OpenMW's restoreDynamicStats: (fFatigueReturnBase + fFatigueReturnMult x (1 - load)) x fEndFatigueMult x Endurance a
	// second, the load being the carried weight over capacity (at most 1); nothing when already at or over the maximum
	float perSecond = (w.game.gmstf("ffatiguereturnbase", 2.5f) + w.game.gmstf("ffatiguereturnmult", 0.02f) * (1.0f - playerLoad))
		* w.game.gmstf("fendfatiguemult", 0.04f) * endurance;
	if (s.fatigue < s.fatigueMax)
		s.fatigue = fminf(s.fatigueMax, s.fatigue + 3600.0f * perSecond);
	updateEffects(3600.0f / w.timescale());
	restDone++;
}

// The hours start passing (the rest screen's Rest / Wait / Until Healed)
void Session::startRest(int hours)
{
	restTotal = hours;
	restDone = 0;
	restFrames = 0;
	w.pcSleeping = restSleep;
	w.wakeUp = false;
	// Sleeping out in the wilds (not in a bed): the region's creatures may come (OpenMW's roll:
	// d(hours) < fSleepRandMod x hours, at fSleepRestMod x hours in)
	restInterruptAt = -1;
	restInterruptList.clear();
	const LevelCell* here = w.current >= 0 ? &w.cells[w.current] : nullptr;
	if (restSleep && !restBed && here && !here->interior && !here->region.empty())
	{
		auto rg = w.game.regions.find(here->region);
		if (rg != w.game.regions.end() && !rg->second.sleep.empty()
			&& 1 + rand() % hours < w.game.gmstf("fsleeprandmod", 0.25f) * hours)
		{
			restInterruptAt = std::max(1, (int)(w.game.gmstf("fsleeprestmod", 0.3f) * hours));
			restInterruptList = rg->second.sleep;
		}
	}
}

void Session::finishRest()
{
	bool slept = restSleep && restDone > 0;
	logf("rest: %d of %d hours %s, %s", restDone, restTotal, restSleep ? "asleep" : "waiting", dateText().c_str());
	w.pcSleeping = false;
	w.wakeUp = false;
	restDone = -1;
	closeScreen();
	fade = 1.0f;
	// Waking with ten major / minor increases behind you: the level up
	if (slept && w.stats.levelProgress >= levelUpTotal())
	{
		levelPicks.clear();
		levelCursor = 0;
		openScreen(SCR_LEVELUP);
		playSound(-1, "levelUP");
	}
}

void Session::drawRest()
{
	PlayerStats& s = w.stats;
	const UiInput& in = uiIn();
	uiCaption(w.game.gmst(restSleep ? "srest" : "swait", restSleep ? "Rest" : "Wait"), dateText());

	float cur[3] = { s.health, s.magicka, s.fatigue }, max[3] = { s.healthMax, s.magickaMax, s.fatigueMax };
	u32 colors[3] = { col::health, col::magicka, col::fatigue };
	for (int i = 0; i < 3; i++)
	{
		float y = 30 + i * 15;
		uiBar(40, y, 240, 12, max[i] > 0 ? cur[i] / max[i] : 0, colors[i]);
		char buf[32];
		snprintf(buf, sizeof(buf), "%d/%d", (int)cur[i], (int)max[i]);
		uiTextCentered(160, y + 6 - uiLineHeight(0.36f) / 2, 0.36f, col::textPressed, buf);
	}

	if (restDone >= 0)
	{
		// The hours pass a few frames each; a script's WakeUpPC or B ends it early
		char buf[64];
		snprintf(buf, sizeof(buf), "%s  %d / %d", restSleep ? "Resting" : "Waiting", restDone, restTotal);
		uiTextCentered(160, 96, 0.5f, col::text, buf);
		uiBar(40, 120, 240, 12, restTotal > 0 ? (float)restDone / restTotal : 1.0f, col::header);
		bool stop = uiButton(110, 196, 100, 32, w.game.gmst("scancel", "Cancel"), true) || (in.down & KEY_B);
		if (!stop && !w.wakeUp && ++restFrames >= kRestFramesPerHour)
		{
			restFrames = 0;
			restHour();
			if (restInterruptAt >= 0 && restDone >= restInterruptAt)
			{
				// Something comes out of the dark
				std::string id = w.game.pickLeveled(restInterruptList, w.stats.level);
				float fx = sinf(w.player.yaw), fy = cosf(w.player.yaw);
				float pos[3] = { w.player.feet[0] + fx * 300.0f, w.player.feet[1] + fy * 300.0f, w.player.feet[2] + 50.0f };
				int ri = id.empty() ? -1 : w.spawnActor(id, pos, w.player.yaw + 3.14159f);
				logf("rest: interrupted after %d hours by %s", restDone, id.c_str());
				restInterruptAt = -1;
				if (ri >= 0)
				{
					notify(w.game.gmst("ssleepinterrupt", "Your rest has been interrupted."));
					finishRest();
					return;
				}
			}
		}
		if (stop || w.wakeUp || restDone >= restTotal)
			finishRest();
		return;
	}

	std::string ask = restSleep ? w.game.gmst("srestmenu1", "How many hours?")
		: w.game.gmst("srestillegal", "Resting here is illegal. You'll need to find a bed.");
	std::vector<std::string> lines = uiWrap(ask, 300, 0.42f);
	for (size_t i = 0; i < lines.size() && i < 2; i++)
		uiTextCentered(160, 76 + i * uiLineHeight(0.42f), 0.42f, restSleep ? col::text : col::health, lines[i]);

	// Hours: D-pad up / down, the - and + buttons, or a tap on the bar
	if (in.down & KEY_UP) restHours++;
	if (in.down & KEY_DOWN) restHours--;
	if (uiButton(16, 104, 34, 28, "-")) restHours--;
	if (uiButton(270, 104, 34, 28, "+")) restHours++;
	if (in.touching && in.touchX >= 56 && in.touchX < 264 && in.touchY >= 100 && in.touchY < 136)
		restHours = 1 + (int)roundf((in.touchX - 60) / 200.0f * 23.0f);
	restHours = restHours < 1 ? 1 : restHours > 24 ? 24 : restHours;
	uiRect(60, 114, 200, 8, col::panel);
	uiFrame(60, 114, 200, 8);
	float knob = 60 + (restHours - 1) / 23.0f * 200.0f;
	uiRect(knob - 4, 108, 8, 20, col::header);
	uiTextCentered(160, 138, 0.5f, col::text, std::to_string(restHours) + " " + w.game.gmst("srestmenu2", "Hours"));
	if (restSleep && s.levelProgress >= levelUpTotal())
		uiTextCentered(160, 164, 0.42f, col::header, "You will level up when you wake.");

	std::vector<std::string> labels;
	if (restSleep)
		labels.push_back(w.game.gmst("suntilhealed", "Until Healed"));
	labels.push_back(w.game.gmst(restSleep ? "srest" : "swait", restSleep ? "Rest" : "Wait"));
	labels.push_back(w.game.gmst("scancel", "Cancel"));
	int b = buttonRow(labels, focus, 200, 32);
	int hours = 0;
	if (restSleep && b == 0)
		hours = hoursToHeal(w);
	else if (b == (int)labels.size() - 2)
		hours = restHours;
	if (hours > 0)
		startRest(hours);
	else if (b == (int)labels.size() - 1 || (in.down & KEY_B))
		closeScreen();
}
