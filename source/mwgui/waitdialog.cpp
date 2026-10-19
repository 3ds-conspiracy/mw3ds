#include <algorithm>
#include <cmath>
#include <cstdio>

#include "components/debug/debuglog.hpp"
#include "mwgui/windowmanagerimp.hpp"
#include "mwbase/session.hpp"

// Hours of rest one frame passes while the rest screen counts them off
static const int kRestFramesPerHour = 4;

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
	rechargeItems(3600.0f / w.timescale());
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
	fadeRate = 5.0f;                     // (back in a fifth of a second, as OpenMW's WaitDialog: fadeScreenIn(0.2))
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
