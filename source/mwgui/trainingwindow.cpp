// Training a skill at an NPC
#include <cmath>
#include <cstdio>
#include <cstring>
#include "mwsound/soundmanagerimp.hpp"
#include "platform/linear.hpp"
#include "components/debug/debuglog.hpp"
#include "mwrender/renderingmanager.hpp"
#include "mwgui/windowmanagerimp.hpp"
#include "mwbase/session.hpp"

// The trainer's (barterRef) three best skills, best first
void Session::trainerSkills(int best[3])
{
	const ActorDef& def = w.game.actors[w.refs[barterRef].actor];
	best[0] = best[1] = best[2] = -1;
	for (int k = 0; k < 27; k++)
		for (int j = 0; j < 3; j++)
			if (best[j] < 0 || def.skills[k] > def.skills[best[j]])
			{
				for (int m = 2; m > j; m--)
					best[m] = best[m - 1];
				best[j] = k;
				break;
			}
}

int Session::trainPrice(int skill)
{
	return trainPriceFor(barterRef, skill);
}

// One point of a skill from the trainer (barterRef): Morrowind's refusals, else paid, raised, two hours
bool Session::trainSkill(int k)
{
	const ActorDef& def = w.game.actors[w.refs[barterRef].actor];
	const PlayerStats& s = w.stats;
	int price = trainPrice(k);
	std::string msg;
	if (w.itemCount("gold_001") < price)
		msg = "You don't have enough gold.";
	else if (baseSkill(k) >= def.skills[k])
		msg = "You are already as skilled as your trainer.";
	else if (k < (int)w.game.skills.size() && w.game.skills[k].attribute >= 0 && w.game.skills[k].attribute < 8
		&& baseSkill(k) >= s.attributes[w.game.skills[k].attribute])
		msg = w.game.gmst("snotifymessage17", "Your governing attribute is too low to be trained further.");
	if (!msg.empty())
	{
		notify(msg);
		logf("training: refused: %s", msg.c_str());
		return false;
	}
	w.removeItem("gold_001", price);
	w.refs[barterRef].gold += price;
	// Exactly one point, counted towards the next level like any increase
	raiseSkill(k);
	w.gameHour += 2.0f;
	rechargeItems(2.0f * 3600.0f / w.timescale());
	fade = 1.0f;
	fadeRate = 5.0f;                     // (OpenMW's TrainingWindow: fadeScreenIn(0.2))
	logf("training: skill %d for %d gold", k, price);
	return true;
}

// Trainers teach their three best skills, one point at a time: fTrainingMod x the current
// level, at their barter price; two hours pass
void Session::drawTraining()
{
	const Ref& r = w.refs[barterRef];
	const ActorDef& def = w.game.actors[r.actor];
	PlayerStats& s = w.stats;
	int gold = w.itemCount("gold_001");
	header(w.game.gmst("strain", "Training") + " - " + def.name, "Gold: " + std::to_string(gold));
	int best[3];
	trainerSkills(best);
	std::vector<std::string> rows;
	int prices[3];
	for (int j = 0; j < 3; j++)
	{
		prices[j] = trainPrice(best[j]);
		rows.push_back(skillName(w, best[j]) + " " + std::to_string(s.skills[best[j]]) + " -> "
			+ std::to_string(s.skills[best[j]] + 1) + "   " + std::to_string(prices[j]) + "g");
	}
	uiList(list2, 4, 24, 312, 176, rows, 0.5f, true);
	int sel = list2.selected < 3 ? list2.selected : -1;
	int b = buttonRow({ "Train", w.game.gmst("sdone", "Done") }, focus);
	if (b == 0 && sel >= 0)
		trainSkill(best[sel]);
	else if (b == 1 || (uiIn().down & KEY_B))
	{
		barterRef = -1;
		screen = SCR_DIALOGUE;
	}
}
