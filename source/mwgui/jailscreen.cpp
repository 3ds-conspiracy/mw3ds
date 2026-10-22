// Arrest: a guard's crime greeting, and the time in jail
#include <cmath>
#include <cstdio>
#include <cstring>
#include "components/debug/debuglog.hpp"
#include "mwgui/windowmanagerimp.hpp"
#include "mwbase/session.hpp"

void Session::drawArrest()
{
	header("Guard");
	int gold = w.itemCount("gold_001");
	uiPanel(4, 24, 312, 120);
	uiTextBox(scroll, 10, 30, 300, 110, "Stop right there, criminal scum! You violated the law. Pay the court a fine of "
		+ std::to_string(w.bounty) + " gold or serve your sentence.", 0.5f, false);
	std::vector<std::string> labels;
	if (gold >= w.bounty)
		labels.push_back("Pay " + std::to_string(w.bounty) + "g");
	labels.push_back("Go to jail");
	labels.push_back("Resist");
	int b = buttonRow(labels, focus, 170, 30);
	if (b < 0)
		return;
	auto calm = [&]() {
		w.forLoadedRefs([&](int i) {
			Ref& r = w.refs[i];
			if (r.ai == AI_COMBAT && !r.aggressor)
			{
				r.ai = AI_IDLE;
				if (Actor* a = w.actorOf(i))
					a->showWeapon = false;
			}
		});
	};
	if (labels[b].rfind("Pay", 0) == 0)
	{
		w.removeItem("gold_001", w.bounty);
		w.bounty = 0;
		w.crimePaid();
		calm();
		playSound(-1, "Item Gold Down");
		closeScreen();
	}
	else if (labels[b] == "Go to jail")
	{
		calm();
		closeScreen();
		goToJail();
	}
	else
	{
		w.arrestDeclined = w.bounty;
		if (arrestingGuard >= 0)
			makeHostile(arrestingGuard);
		closeScreen();
	}
	w.globals["pccrimelevel"] = (float)w.bounty;
}

// The land map (tools/convert/worldmap.py), centred on the player, with town names and an arrow for them
// ---- maps: the world map outdoors, the local map (a top-down render) indoors

// Jail: a day per hundred gold of bounty passes, stolen goods are taken, a skill a day changes (Security
// and Sneak may rise, the others fall), and the player is let out at the nearest prison's release spot
void Session::goToJail()
{
	int days = w.bounty / 100 > 1 ? w.bounty / 100 : 1;
	w.gameHour += 24.0f * days;
	rechargeItems(24.0f * days * 3600.0f / w.timescale());
	w.bounty = 0;
	w.crimePaid();
	w.globals["pccrimelevel"] = 0.0f;
	w.confiscateStolen();
	for (int d = 0; d < days; d++)
	{
		int k = rand() % 27;
		bool rises = (k == 18 || k == 19) && rand() % 2;       // Security, Sneak: learned inside
		if (rises)
			w.stats.skillBonus[k]++;
		else if (w.stats.skills[k] > 0)
			w.stats.skillBonus[k]--;
	}
	w.recomputeStats();
	fade = 1.0f;
	char msg[160];
	snprintf(msg, sizeof(msg), w.game.gmst(days == 1 ? "snotifymessage42" : "snotifymessage43",
		days == 1 ? "You have been released after %d day." : "You have been released after %d days.").c_str(), days);
	notify(msg);
	// Nearest prison (from the last spot outdoors when inside), let out at the marker itself, outdoors
	// (OpenMW teleportToClosestMarker). Its door destination only says where the stolen goods go: Ebonheart's
	// lies outside the garrison's walls, under every floor.
	const float* at = w.cells[w.current].interior ? w.lastOutside : w.player.feet;
	const GameData::PrisonMarker* best = nullptr;
	int bestCell = -1;
	float bestD = 1e30f;
	for (auto& m : w.game.prisonMarkers)
	{
		float dx = m.pos[0] - at[0], dy = m.pos[1] - at[1], d = dx * dx + dy * dy;
		int c = w.gridCell((int)floorf(m.pos[0] / 8192.0f), (int)floorf(m.pos[1] / 8192.0f));
		if (d < bestD && c >= 0)
		{
			bestD = d;
			best = &m;
			bestCell = c;
		}
	}
	if (best)
		teleportPlayer(bestCell, best->pos, 0.0f);
	logf("jail: %d days, released outside %s", days, best ? best->cell.c_str() : "nowhere (stays in place)");
}
