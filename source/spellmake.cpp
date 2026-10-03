// Spellmaking at a spellmaker: effects the player's spells have, put together into a new spell
// (Morrowind's cost as OpenMW has it). The effect editor is shared with enchanting.
#include <cmath>
#include <functional>

#include "log.h"
#include "screens.h"
#include "session.h"
#include "ui.h"

static const char* kRangeNames[3] = { "Self", "Touch", "Target" };

// Magic effect flags (MGEF MEDT)
enum { MEF_APPLIED_ONCE = 0x1000, MEF_NO_DURATION = 0x4, MEF_NO_MAGNITUDE = 0x8, MEF_SELF = 0x40, MEF_TOUCH = 0x80, MEF_TARGET = 0x100 };

int Session::effectFlags(int effect)
{
	auto it = w.game.magicEffects.find(effect);
	return it != w.game.magicEffects.end() ? it->second.flags : 0;
}

// One effect's cost: 0.5 x (min + max) x base cost / 10 x (1 + duration) x fEffectCostMult (the target x1.5 is applied
// to the running total: madeSpellCost)
float Session::effectCost(const SpellEffect& e)
{
	auto it = w.game.magicEffects.find(e.effect);
	float base = it != w.game.magicEffects.end() ? it->second.cost : 1.0f;
	int flags = effectFlags(e.effect);
	float mn = (flags & MEF_NO_MAGNITUDE) ? 1.0f : fmaxf(1.0f, (float)e.min);
	float mx = (flags & MEF_NO_MAGNITUDE) ? 1.0f : fmaxf(1.0f, (float)e.max);
	float dur = (flags & MEF_NO_DURATION) ? 1.0f : (float)e.duration;
	if (!(flags & MEF_APPLIED_ONCE))
		dur = fmaxf(1.0f, dur);
	// (OpenMW's calcEffectCost for a spell being made: a duration offset of 1, an area of at least 1)
	float x = 0.5f * (mn + mx) * 0.1f * base * (1.0f + dur) + 0.05f * fmaxf(1.0f, (float)e.area) * base;
	return x * w.game.gmstf("feffectcostmult", 0.5f);
}

// A made spell's magicka cost (OpenMW's spell creation window, as vanilla): each effect adds max(1, its cost), and a
// target effect multiplies the running total by 1.5 (not just its own cost); the spell costs int() of that
float Session::madeSpellCost(const std::vector<SpellEffect>& effects)
{
	float y = 0.0f;
	for (auto& e : effects)
	{
		y += fmaxf(1.0f, effectCost(e));
		if (e.range == 2)
			y *= 1.5f;
	}
	return y;
}

// The list of effects (tap one to change it) and the selected one's range / magnitude / duration.
// Returns true when the effects changed. ranges: which ranges are allowed (bit 0 self, 1 touch, 2 target)
bool Session::effectEditor(std::vector<SpellEffect>& fx, int& sel, int ranges, float y0, int maxRows)
{
	float cy = y0 + maxRows * 13 + 4;          // the selected one's controls below the rows
	bool changed = false;
	for (size_t k = 0; k < (size_t)maxRows; k++)
	{
		if (k >= fx.size())
			break;
		const SpellEffect& e = fx[k];
		int flags = effectFlags(e.effect);
		std::string row = effectLabel(e.effect, e.skill, e.attribute);
		if (!(flags & MEF_NO_MAGNITUDE))
			row += " " + std::to_string(e.min) + (e.max != e.min ? "-" + std::to_string(e.max) : "");
		if (!(flags & MEF_NO_DURATION))
			row += " " + std::to_string(e.duration) + "s";
		row += std::string(" ") + kRangeNames[e.range];
		if (uiButton(138, y0 + k * 13, 178, 12, row, (int)k == sel))
			sel = k;
	}
	if (sel < 0 || sel >= (int)fx.size())
		return false;
	SpellEffect& e = fx[sel];
	int flags = effectFlags(e.effect);
	int step = (uiIn().held & KEY_R) ? 10 : 1;
	if (uiButton(138, cy, 90, 18, std::string("Range: ") + kRangeNames[e.range]))
	{
		// the next range this effect and this kind of item allow
		for (int k = 1; k <= 3; k++)
		{
			int r = (e.range + k) % 3;
			int bit = r == 0 ? MEF_SELF : r == 1 ? MEF_TOUCH : MEF_TARGET;
			if ((flags & bit) && (ranges & (1 << r)))
			{
				e.range = r;
				break;
			}
		}
		changed = true;
	}
	if (uiButton(232, cy, 84, 18, "Remove"))
	{
		fx.erase(fx.begin() + sel);
		sel = fx.empty() ? -1 : 0;
		return true;
	}
	auto spin = [&](float x, float y, const char* label, int& v, int lo, int hi) {
		uiText(x, y + 2, 0.38f, col::textDim, label);
		if (uiButton(x + 22, y, 16, 16, "-") && v > lo)
		{
			v = std::max(lo, v - step);
			changed = true;
		}
		uiTextCentered(x + 49, y + 2, 0.4f, col::text, std::to_string(v));
		if (uiButton(x + 60, y, 16, 16, "+") && v < hi)
		{
			v = std::min(hi, v + step);
			changed = true;
		}
	};
	if (!(flags & MEF_NO_MAGNITUDE))
	{
		spin(138, cy + 22, "Min", e.min, 1, 100);
		spin(222, cy + 22, "Max", e.max, 1, 100);
		if (e.max < e.min)
			e.max = e.min;
	}
	if (!(flags & MEF_NO_DURATION))
		spin(138, cy + 42, "Dur", e.duration, 1, 300);
	return changed;
}

// Effects the player can put into a spell: every effect of the spells they know
std::vector<SpellEffect> Session::knownEffects()
{
	std::vector<SpellEffect> out;
	for (auto& id : knownSpells())
		for (auto& e : w.game.spells[id].effects)
		{
			bool have = false;
			for (auto& f : out)
				have |= f.effect == e.effect && f.skill == e.skill && f.attribute == e.attribute;
			if (!have)
				out.push_back(e);
		}
	return out;
}

// A new effect as it first appears: magnitude 1, a second, the first range it allows
SpellEffect Session::newEffect(const SpellEffect& from, int ranges)
{
	SpellEffect e = { from.effect, from.skill, from.attribute, 1, 1, 1, 0 };
	int flags = effectFlags(e.effect);
	for (int r = 0; r < 3; r++)
		if ((flags & (r == 0 ? MEF_SELF : r == 1 ? MEF_TOUCH : MEF_TARGET)) && (ranges & (1 << r)))
		{
			e.range = r;
			break;
		}
	return e;
}

void Session::drawSpellmaking()
{
	int gold = w.itemCount("gold_001");
	header(w.game.gmst("sspellmakingmenutitle", "Spellmaking"), "Gold " + std::to_string(gold));
	std::vector<SpellEffect> known = knownEffects();
	std::vector<std::string> rows;
	for (auto& e : known)
		rows.push_back(effectLabel(e.effect, e.skill, e.attribute));
	int pick = uiList(list2, 4, 24, 130, 176, rows, 0.4f, true);
	if (pick >= 0 && pick < (int)known.size() && makeEffects.size() < 8)
	{
		makeEffects.push_back(newEffect(known[pick], 7));
		makeSel = makeEffects.size() - 1;
		playSound(-1, "Menu Click");
	}
	effectEditor(makeEffects, makeSel, 7);
	float cost = 0.0f;
	int price = spellmakePrice(&cost);
	uiText(232, 176, 0.38f, col::textDim, "Cost " + std::to_string((int)roundf(cost)));
	int noFocus = -1;
	int b = buttonRow({ "Buy (" + std::to_string(price) + ")", w.game.gmst("sclose", "Close") }, noFocus);
	if (b == 0)
		spellmakeConfirm();
	else if (b == 1 || (uiIn().down & KEY_B))
	{
		playSound(-1, "Menu Click");
		makeEffects.clear();
		makeSel = -1;
		screen = SCR_DIALOGUE;
	}
}

// The spell's cost (magicka) and price: fSpellMakingValueMult x the cost
int Session::spellmakePrice(float* costOut)
{
	float cost = madeSpellCost(makeEffects);
	if (costOut)
		*costOut = cost;
	// max(1, int(cost x fSpellMakingValueMult)), then the spellmaker's barter price
	int price = std::max(1, (int)(cost * w.game.gmstf("fspellmakingvaluemult", 7.0f)));
	return barterRef >= 0 && w.refs[barterRef].actor >= 0 ? barterPrice(barterRef, price, true) : price;
}

// The Buy button: the spell is the player's for the price (false: no effects / not enough gold)
bool Session::spellmakeConfirm()
{
	float cost = 0.0f;
	int price = spellmakePrice(&cost);
	if (makeEffects.empty())
		return false;
	{
		if (price > w.itemCount("gold_001"))
		{
			notify(w.game.gmst("snotifymessage18", "You don't have enough gold."));
			logf("spellmaking: refused: not enough gold for %d", price);
			return false;
		}
		else
		{
			SpellDef sp;
			sp.type = 0;
			sp.cost = std::max(1, (int)cost);
			sp.effects = makeEffects;
			const SpellEffect& f = makeEffects[0];
			sp.name = effectLabel(f.effect, f.skill, f.attribute);
			std::string key;
			for (auto& e : makeEffects)
				key += std::to_string(e.effect) + "_" + std::to_string(e.skill) + "_" + std::to_string(e.attribute) + "_"
					+ std::to_string(e.min) + "_" + std::to_string(e.max) + "_" + std::to_string(e.duration) + "_"
					+ std::to_string(e.range) + "_";
			sp.id = "mw3ds_spell_" + std::to_string(std::hash<std::string>()(key) % 1000000);
			if (!w.game.spells.count(sp.id))
			{
				w.game.spells[sp.id] = sp;
				w.madeSpells.push_back(sp.id);
			}
			w.stats.spells.push_back(sp.id);
			w.removeItem("gold_001", price);
			if (barterRef >= 0)
				w.refs[barterRef].gold += price;
			playSound(-1, "Item Gold Down");
			notify("You made the spell " + sp.name + ".");
			logf("spellmaking: %s (%s, %zu effects, cost %d, price %d)", sp.id.c_str(), sp.name.c_str(),
				sp.effects.size(), sp.cost, price);
			makeEffects.clear();
			makeSel = -1;
		}
	}
	return true;
}
