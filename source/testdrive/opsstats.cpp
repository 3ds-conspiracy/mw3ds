// Tokens: stats and services (skills, enchanting, spells, repairs, training, trade, levels)
#include <optional>
#include <3ds.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <queue>
#include <tuple>
#include "testdrive/testdrive.hpp"
#include "platform/gamma.hpp"
#include "mwbase/session.hpp"
#include "components/debug/debuglog.hpp"
#include "mwsound/soundmanagerimp.hpp"
#include "platform/console.hpp"

extern const char* g_playerBlock;
#include "mwgui/windowmanagerimp.hpp"
#include "mwphysics/physicssystem.hpp"
#include "platform/linear.hpp"
#include "platform/memstat.hpp"
#include "components/files/zfile.hpp"
#include <malloc.h>
#include "testdrive/testhelpers.hpp"
#include "testdrive/opshelpers.hpp"

std::optional<bool> opsStats(Session& s, const std::vector<std::string>& a)
{
	World& w = s.w;
	const std::string& verb = a[0];
	auto fail = [&](const std::string& why) { logf("drive: FAIL %s: %s", verb.c_str(), why.c_str()); return true; };
	auto service = [&](int ri, unsigned flag) {
		return ri >= 0 && w.refs[ri].actor >= 0 && (w.game.actors[w.refs[ri].actor].services & flag) != 0;
	};
	auto notTalking = [&](int ri) { return s.testLegit && !s.talkingTo(ri); };
	if (verb == "SNAP" && a.size() >= 2)
	{
		if (a[1] == "moved")
			memcpy(s_movedFrom, w.player.feet, sizeof(s_movedFrom));
		float v;
		std::string arg = a.size() >= 3 ? a[2] : "";
		if (!numberOf(s, a[1], arg, v))
			return fail("no such number " + a[1] + " " + arg);
		s_snap[a[1] + ":" + arg] = v;
		logf("test: snap %s %s = %g", a[1].c_str(), arg.c_str(), v);
		return true;
	}
	if (verb == "CLASS" && a.size() >= 2)
	{
		w.stats.cls = lower(spaced(a[1]));
		if (!w.playerClass())
			return fail("no class " + a[1]);
		w.recomputeStats();
		logf("test: class %s", w.stats.cls.c_str());
		return true;
	}
	if ((verb == "SETSKILL" || verb == "SETATTR") && a.size() >= 3)
	{
		bool skill = verb == "SETSKILL";
		int k = skill ? skillIndex(w, a[1]) : attrIndex(w, a[1]);
		if (k < 0)
			return fail("no such " + a[1]);
		int want = atoi(a[2].c_str());
		for (int pass = 0; pass < 2; pass++)
		{
			if (skill)
				w.stats.skillBonus[k] += want - w.stats.skills[k];
			else
				w.stats.attrBonus[k] += want - w.stats.attributes[k];
			w.recomputeStats();
		}
		logf("test: %s %s = %d", skill ? "skill" : "attribute", a[1].c_str(), skill ? w.stats.skills[k] : w.stats.attributes[k]);
		return true;
	}
	if (verb == "SKILLPROG" && a.size() >= 3)
	{
		int k = skillIndex(w, a[1]);
		if (k < 0)
			return fail("no such skill " + a[1]);
		// absolute (the need itself is under test: skillNeed isn't used to set it)
		w.stats.skillProgress[k] = (float)atof(a[2].c_str());
		logf("test: skill %d progress %.2f of %.2f", k, w.stats.skillProgress[k], s.skillNeed(k));
		return true;
	}
	if (verb == "LEVELPROG" && a.size() >= 2)
	{
		w.stats.levelProgress = atoi(a[1].c_str());
		return true;
	}
	if (verb == "ENCHANTAT" && a.size() >= 2)
	{
		int ri = findRef(s, a[1]);
		if (!service(ri, 0x10000))
			return fail(a[1] + " doesn't enchant");
		if (notTalking(ri))
			return fail("not talking to " + a[1] + " (LEGIT)");
		s.openEnchanting(ri);
		return true;
	}
	// BUYSPELL:<merchant>:<spell>: the Spells service's Buy (talking to them): one they sell that the player doesn't know
	if (verb == "BUYSPELL" && a.size() >= 3)
	{
		int ri = findRef(s, a[1]);
		if (ri < 0 || w.refs[ri].actor < 0)
			return fail(a[1] + " not loaded");
		if (notTalking(ri))
			return fail("not talking to " + a[1] + " (LEGIT)");
		const ActorDef& def = w.game.actors[w.refs[ri].actor];
		std::string want = lower(spaced(a[2])), id;
		for (auto& sp : def.spells)
			if (lower(sp) == want || lower(sp) == lower(a[2]))
				id = sp;
		if (id.empty())
			return fail(a[1] + " doesn't sell " + a[2]);
		for (auto& k : w.stats.spells)
			if (lower(k) == lower(id))
				return fail(a[2] + " is known already");
		if (!s.buySpell(ri, id))
			return fail("couldn't buy " + a[2]);
		return true;
	}
	// REPAIRAT:<smith>:<item>: the smith's Repair service on that carried item (talking to them), as its row does
	// REPAIRWITH:<tool>:<item>: one try with a carried repair tool (a hammer, prongs) on that item, as the tool's screen
	if ((verb == "REPAIRAT" || verb == "REPAIRWITH") && a.size() >= 3)
	{
		int item = carried(w, a[2]);
		if (item < 0)
			return fail(a[2] + " not carried");
		Screen before = s.screen;
		if (verb == "REPAIRAT")
		{
			int ri = findRef(s, a[1]);
			if (!service(ri, 0x20000))
				return fail(a[1] + " doesn't repair");
			if (notTalking(ri))
				return fail("not talking to " + a[1] + " (LEGIT)");
			s.openRepair(ri, -1);
		}
		else
		{
			int tool = carried(w, a[1]);
			const Object* o = tool >= 0 ? w.game.object(w.inventory[tool].id) : nullptr;
			if (!o || o->type != "REPA")
				return fail(a[1] + " isn't a carried repair tool");
			s.openRepair(-1, tool);
		}
		s.repairOne(item);
		s.screen = before;
		s.repairTool = -1;
		return true;
	}
	if (verb == "ENCHITEM" && a.size() >= 2)
	{
		std::vector<int> items, gems;
		s.enchantChoices(items, gems);
		int i = carried(w, a[1]);
		if (i < 0 || std::find(items.begin(), items.end(), i) == items.end())
			return fail(a[1] + " can't be enchanted (or isn't carried)");
		s.enchantItem = i;
		return true;
	}
	if (verb == "ENCHGEM" && a.size() >= 2)
	{
		s.enchantGem = -1;
		for (size_t k = 0; k < w.inventory.size(); k++)
			if (lower(w.inventory[k].id) == lower(a[1]) && !w.inventory[k].soul.empty())
				s.enchantGem = (int)k;
		if (s.enchantGem < 0)
			return fail("no " + a[1] + " holding a soul");
		return true;
	}
	if (verb == "ENCHTYPE" && a.size() >= 2)
	{
		s.enchantType = atoi(a[1].c_str());
		return true;
	}
	if (verb == "ADDEFFECT" && a.size() >= 6)
	{
		// ADDEFFECT:effect:min:max:duration:range[:attribute or skill or "-"[:area]]
		SpellEffect e = {};
		e.effect = atoi(a[1].c_str());
		e.min = atoi(a[2].c_str());
		e.max = atoi(a[3].c_str());
		e.duration = atoi(a[4].c_str());
		e.range = atoi(a[5].c_str());
		e.attribute = e.skill = -1;
		if (a.size() >= 8)
			e.area = atoi(a[7].c_str());
		if (a.size() >= 7 && a[6] != "-")
		{
			bool skillEffect = e.effect == 21 || e.effect == 26 || e.effect == 78 || e.effect == 83 || e.effect == 89;
			if (skillEffect)
				e.skill = skillIndex(w, a[6]);
			else
				e.attribute = attrIndex(w, a[6]);
		}
		s.makeEffects.push_back(e);
		return true;
	}
	if (verb == "CONFIRM")
	{
		bool done = s.screen == SCR_ENCHANT ? s.enchantConfirm() : s.screen == SCR_SPELLMAKE ? s.spellmakeConfirm() : false;
		logf("test: confirm on screen %d: %s", (int)s.screen, done ? "done" : "refused");
		return true;
	}
	if (verb == "SPELLMAKE" && a.size() >= 2)
	{
		int ri = findRef(s, a[1]);
		if (!service(ri, 0x8000))
			return fail(a[1] + " doesn't make spells");
		if (notTalking(ri))
			return fail("not talking to " + a[1] + " (LEGIT)");
		s.makeEffects.clear();
		s.makeSel = -1;
		s.makeName.clear();
		s.barterRef = ri;                  // (the spellmaker's barter price)
		s.screen = SCR_SPELLMAKE;
		return true;
	}
	if (verb == "TRAIN" && a.size() >= 3)
	{
		int ri = findRef(s, a[1]);
		int k = skillIndex(w, a[2]);
		if (!service(ri, 0x4000))
			return fail(a[1] + " doesn't train");
		if (notTalking(ri))
			return fail("not talking to " + a[1] + " (LEGIT)");
		s.barterRef = ri;
		int best[3];
		s.trainerSkills(best);
		if (k < 0 || (best[0] != k && best[1] != k && best[2] != k))
			return fail(a[1] + " doesn't teach " + a[2]);
		s.trainSkill(k);
		s.barterRef = -1;
		return true;
	}
	if ((verb == "BUY" || verb == "SELL") && a.size() >= 3)
	{
		int ri = findRef(s, a[1]);
		if (ri < 0 || !s.isMerchant(ri))
			return fail(a[1] + " doesn't trade");
		if (notTalking(ri) && !(s.screen == SCR_BARTER && s.barterRef == ri))
			return fail("not talking to " + a[1] + " (LEGIT)");
		// (the trade happens as the Barter screen's; then back to what was open: the dialogue, as its Done button does)
		Screen before = s.screen;
		s.openBarter(ri);
		s.screen = before;
		Ref& m = w.refs[ri];
		int index = -1;
		const Object* o = nullptr;
		if (verb == "SELL")
		{
			index = carried(w, a[2]);
			o = index >= 0 ? w.game.object(w.inventory[index].id) : nullptr;
		}
		int from = -1;
		bool found = verb == "SELL" && index >= 0;
		if (verb == "BUY")
			for (auto& g : s.merchantGoods(ri))
			{
				std::string id = s.merchantGood(g).second;
				if (lower(id) == lower(a[2]) || lower(id) == lower(spaced(a[2])))
				{
					from = g.first;
					index = g.second;
					o = w.game.object(id);
					found = true;
				}
			}
		if (!found || !o)
		{
			// (what they do have, to pick from)
			std::string have;
			if (verb == "BUY")
				for (auto& g : s.merchantGoods(ri))
					if (have.size() < 300)
						have += " " + s.merchantGood(g).second;
			return fail(a[2] + (verb == "SELL" ? " not carried" : " not among their goods (they have:" + have + ")"));
		}
		if (!s.merchantTrades(ri, o))
			return fail(a[1] + " doesn't trade in " + a[2]);
		int price = s.barterPrice(ri, o->value, verb == "BUY");
		// BUY / SELL:<npc>:<item>:<steps>: haggled first, as Y / X on the barter screen (each 5% of the price: up when
		// selling, down when buying); a refused offer is no trade
		int offer = price;
		if (a.size() >= 4)
		{
			int steps = atoi(a[3].c_str());
			offer = std::max(1, price + (int)roundf(price * 0.05f * (verb == "SELL" ? steps : -steps)));
			s.barterRef = ri;
			s.barterSell = verb == "SELL";
			if (offer != price && !s.haggleAccepted(price, offer))
			{
				logf("test: %s %s %s at %d of %d: offer refused", verb.c_str(), a[1].c_str(), a[2].c_str(), offer, price);
				s.barterRef = -1;
				return true;
			}
		}
		bool done = s.barterTrade(verb == "SELL", index, offer, from);
		logf("test: %s %s %s for %d: %s", verb.c_str(), a[1].c_str(), a[2].c_str(), offer, done ? "done" : "refused");
		s.barterRef = -1;
		return true;
	}
	if (verb == "LEVELUP" && a.size() >= 2)
	{
		if (s.screen != SCR_LEVELUP)
			return fail("the level up screen isn't open");
		s.levelPicks.clear();
		for (size_t k = 1; k < a.size(); k++)
			s.levelPicks.push_back(attrIndex(w, a[k]));
		s.applyLevelUp();
		s.closeScreen();
		return true;
	}
	if (verb == "READ" && a.size() >= 2)
	{
		int i = carried(w, a[1]);
		if (i < 0)
			return fail(a[1] + " not carried");
		// the inventory's Read (mwgui/inventorywindow.cpp): the book screen, which teaches its skill the first time
		const Object* o = w.game.object(w.inventory[i].id);
		if (!o || o->type != "BOOK")
			return fail(a[1] + " isn't a book");
		s.openScreen(SCR_BOOK);
		s.bookItem = w.inventory[i].id;
		logf("test: reading %s", a[1].c_str());
		return true;
	}
	return std::nullopt;
}
