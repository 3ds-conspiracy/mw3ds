// Buying spells from an NPC
#include <cmath>
#include <cstdio>
#include <cstring>
#include "mwsound/soundmanagerimp.hpp"
#include "platform/linear.hpp"
#include "components/debug/debuglog.hpp"
#include "mwrender/renderingmanager.hpp"
#include "mwgui/windowmanagerimp.hpp"
#include "mwbase/session.hpp"

// A spell bought from a spell merchant: fSpellValueMult x its cost at their barter price, learned when the gold is there
bool Session::buySpell(int merchant, const std::string& id)
{
	auto it = w.game.spells.find(id);
	if (it == w.game.spells.end())
		return false;
	const SpellDef& sp = it->second;
	int price = barterPrice(merchant, std::max(1, (int)(sp.cost * w.game.gmstf("fspellvaluemult", 10.0f))), true);
	if (w.itemCount("gold_001") < price)
	{
		notify("You don't have enough gold.");
		return false;
	}
	w.removeItem("gold_001", price);
	w.refs[merchant].gold += price;          // (OpenMW: services pay into their gold)
	w.stats.spells.push_back(id);
	playSound(-1, "Item Gold Down");
	notify("You learned " + sp.name + ".");
	logf("spells: bought %s from %s for %d", id.c_str(), w.refs[merchant].id.c_str(), price);
	return true;
}

// Spell merchants: fSpellValueMult x spell cost at their barter price
void Session::drawSpells()
{
	const Ref& r = w.refs[barterRef];
	const ActorDef& def = w.game.actors[r.actor];
	int gold = w.itemCount("gold_001");
	header(w.game.gmst("sspells", "Spells") + " - " + def.name, "Gold: " + std::to_string(gold));
	std::vector<std::string> known = knownSpells(), ids, rows;
	std::vector<int> icons;
	for (auto& id : def.spells)
	{
		auto it = w.game.spells.find(id);
		if (it == w.game.spells.end() || it->second.type != 0)
			continue;
		bool have = false;
		for (auto& k : known)
			have |= k == id;
		if (have)
			continue;
		int price = barterPrice(barterRef, std::max(1, (int)(it->second.cost * w.game.gmstf("fspellvaluemult", 10.0f))), true);
		ids.push_back(id);
		rows.push_back(it->second.name + "   " + std::to_string(price) + "g");
		auto me = it->second.effects.empty() ? w.game.magicEffects.end() : w.game.magicEffects.find(it->second.effects[0].effect);
		icons.push_back(me != w.game.magicEffects.end() ? me->second.iconIx : -1);
	}
	if (rows.empty())
		uiTextCentered(160, 100, 0.5f, col::textDim, "Nothing you don't already know");
	uiList(list2, 4, 24, 312, 176, rows, 0.45f, true, nullptr, &icons);
	int sel = list2.selected < (int)ids.size() ? list2.selected : -1;
	// what the chosen spell does, on the top screen as in the Magic menu
	if (sel >= 0)
		spellTip(w.game.spells[ids[sel]]);
	int b = buttonRow({ "Buy", w.game.gmst("sdone", "Done") }, focus);
	if (b == 0 && sel >= 0)
		buySpell(barterRef, ids[sel]);
	else if (b == 1 || (uiIn().down & KEY_B))
	{
		barterRef = -1;
		screen = SCR_DIALOGUE;
	}
}
