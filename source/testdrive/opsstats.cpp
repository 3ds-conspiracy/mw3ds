// Tokens: stats and services (skills, enchanting, spells, repairs, training, trade, levels)
#include <optional>
#include <3ds.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <functional>
#include <set>
#include <map>
#include <queue>
#include <tuple>
#include "testdrive/testdrive.hpp"
#include "mwbase/session.hpp"
#include "components/debug/debuglog.hpp"

extern const char* g_playerBlock;
#include <malloc.h>
#include "testdrive/testhelpers.hpp"

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
	// DUMPSPELLS: every NPC's spells as the engine has them (the record's, plus its autocalc picks), a log line per
	// NPC and one per spell (a whole list could pass the log line's length), for tools/test/golden/golden_compare.py;
	// a creature's: the spells it knows, those it casts in a fight, the diseases it carries
	if (verb == "DUMPSPELLS")
	{
		int n = 0;
		for (auto& d : w.game.actors)
		{
			std::string id = lower(d.id);
			if (d.creature)
			{
				logf("golden: creature\t%s", id.c_str());
				for (auto& sp : d.spells)
					logf("golden: cspell\t%s\t%s", id.c_str(), lower(sp).c_str());
				for (auto& sp : d.combatSpells)
					logf("golden: ccast\t%s\t%s", id.c_str(), lower(sp).c_str());
				for (auto& sp : d.diseases)
					logf("golden: cdisease\t%s\t%s", id.c_str(), lower(sp).c_str());
				continue;
			}
			logf("golden: npc\t%s", id.c_str());
			for (auto& sp : d.spells)
				logf("golden: spell\t%s\t%s", id.c_str(), sp.c_str());
			if (const RaceDef* race = w.game.raceOf(d.race.c_str()))       // (known too: World::actorHasSpell)
				for (auto& sp : race->spells)
					logf("golden: spell\t%s\t%s", id.c_str(), lower(sp).c_str());
			n++;
		}
		logf("test: dumped the spells of %d NPCs", n);
		return true;
	}
	// DUMPCELLS: every cell's objects read, then per cell how many of each object stand in it and every door with where it
	// leads (cell, position), for golden_compare.py cells. Cells are named as OpenMW's: an interior by its name, an
	// exterior by its grid ("ext x,y")
	if (verb == "DUMPCELLS")
	{
		for (int i = 0; i < (int)w.cells.size(); i++)
			w.ensureRefs(i);
		auto cellKey = [&](int i) {
			char buf[64];
			if (!w.cells[i].interior)
			{
				snprintf(buf, sizeof(buf), "ext %d,%d", w.cells[i].gx, w.cells[i].gy);
				return std::string(buf);
			}
			return lower(w.cells[i].name);
		};
		std::map<std::pair<int, std::string>, int> counts;
		int doors = 0;
		for (size_t ri = 0; ri < w.refs.size(); ri++)
		{
			const Ref& r = w.refs[ri];
			if (r.cell < 0 || r.spawnedRef || r.dropped)
				continue;
			// (a leveled spot by its list, as OpenMW lists it, not by the creature rolled for it)
			counts[{ r.cell, r.levList.empty() ? std::string(r.idLower.c_str()) : lower(r.levList) }] += 1;
			if (r.hasDest)
			{
				logf("golden: door\t%s\t%s\t%.0f,%.0f,%.0f\t%s\t%.0f,%.0f,%.0f", cellKey(r.cell).c_str(), r.idLower.c_str(),
					r.pos[0], r.pos[1], r.pos[2], lower(r.destCell).c_str(), r.destPos[0], r.destPos[1], r.destPos[2]);
				doors++;
			}
		}
		for (auto& kv : counts)
			logf("golden: cellobj\t%s\t%s\t%d", cellKey(kv.first.first).c_str(), kv.first.second.c_str(), kv.second);
		logf("test: dumped %d cells, %d refs, %d doors", (int)w.cells.size(), (int)w.refs.size(), doors);
		return true;
	}
	// TOURSTART: the golden tour's clock stops and the player is OpenMW's when started past the menu (golden_behave.py): the
	// "player" record's Dark Elf Acrobat, every attribute 30
	if (verb == "TOURSTART")
	{
		w.globals["timescale"] = 0.0f;
		w.stats.race = "Dark Elf";
		w.stats.cls = "acrobat";
		w.recomputeStats();
		for (int k = 0; k < 8; k++)
			w.stats.attrBonus[k] += 30 - w.stats.attributes[k];
		w.recomputeStats();
		logf("golden: tplayer	%s	%s	%d", w.stats.race.c_str(), w.stats.cls.c_str(), w.stats.attributes[6]);
		return true;
	}
	if (verb == "COE" && a.size() >= 3)
	{
		s.centerOnExterior(atoi(a[1].c_str()), atoi(a[2].c_str()));
		return true;
	}
	// DUMPTOUR:<cell key>: what golden_tour_compare.py sets beside OpenMW's tour_*.lua, one line per object, script and
	// actor of the cell; with no key, the global variables
	if (verb == "DUMPTOUR")
	{
		if (a.size() < 2)
		{
			for (auto& kv : w.globals)
				logf("golden: tglobal	%s	%g", kv.first.c_str(), kv.second);
			return true;
		}
		std::string key = a[1];
		for (size_t k = 2; k < a.size(); k++)      // (a cell's name can hold a colon: "Vivec, Miun-Gei: Enchanter")
			key += ":" + a[k];
		for (auto& ch : key)
			ch = ch == '%' ? ' ' : ch;
		int gx, gy, cell = sscanf(key.c_str(), "ext %d,%d", &gx, &gy) == 2 ? w.gridCell(gx, gy) : w.cellIndex(key);
		if (cell < 0)
			return fail("no cell " + key);
		logf("golden: tcell	%s", key.c_str());
		for (size_t ri = 0; ri < w.refs.size(); ri++)
		{
			Ref& r = w.refs[ri];
			if (r.cell < 0 || (r.at >= 0 ? r.at : r.cell) != cell || r.spawnedRef || r.dropped || r.pickedUp)
				continue;
			bool lockable = r.recType == REC_DOOR || r.recType == REC_CONT;
			logf("golden: tobj	%s	%s	%d	%d	%.0f,%.0f,%.0f	%.3f	%d	%s	%s	%s	%s	%s	%s", key.c_str(),
				r.levList.empty() ? r.idLower.c_str() : lower(r.levList).c_str(), r.enabled ? 1 : 0,
				!r.levList.empty() ? 2 : r.actor >= 0 ? 1 : 0,      // (2: a leveled spot, by where the level placed it)
				r.levList.empty() ? r.pos[0] : r.start[0], r.levList.empty() ? r.pos[1] : r.start[1],
				r.levList.empty() ? r.pos[2] : r.start[2], r.scale, r.count,
				lockable ? std::to_string(abs(r.lockLevel)).c_str() : "", lockable ? (r.lockLevel > 0 ? "1" : "0") : "",
				r.disarmed ? "" : r.trap.c_str(), r.owner.c_str(), r.ownerFaction.c_str(),
				r.ownerFaction.empty() ? "" : std::to_string(r.ownerRank).c_str());
			if (r.script >= 0 && r.script < (int)w.scripts.size() && w.scripts[r.script].script)
			{
				const ScriptInstance& si = w.scripts[r.script];
				std::vector<std::string> vars;
				for (size_t k = 0; k < si.locals.size() && k < si.script->localNames.size(); k++)
				{
					char buf[96];
					snprintf(buf, sizeof(buf), "%s=%g", si.script->localNames[k].c_str(), si.locals[k]);
					vars.push_back(buf);
				}
				std::sort(vars.begin(), vars.end());
				std::string all;
				for (auto& v : vars)
					all += (all.empty() ? "" : ";") + v;
				logf("golden: tsv	%s	%s	%s	%d	%s", key.c_str(), r.idLower.c_str(), lower(si.script->name).c_str(),
					si.running ? 1 : 0, all.c_str());
			}
			if (r.actor < 0 || !r.enabled)
				continue;
			const ActorDef& d = w.game.actors[r.actor];
			static const char* kPackage[] = { "Wander", "Travel", "Escort", "Follow", "Activate", "" };
			std::string pkg = r.aiPackage >= 0 && r.aiPackage <= 5 ? kPackage[r.aiPackage] : (d.wander > 0 ? "Wander" : "");
			std::string foes;
			if (r.ai == AI_COMBAT)
				foes = "player";
			if (!r.duelWith.empty())
				foes += (foes.empty() ? "" : ",") + r.duelWith.str();
			for (auto& ch : foes)
				ch = ch == '|' ? ',' : ch;
			if (!foes.empty())
				pkg = "Combat";
			// (OpenMW's getWalkSpeed leaves out scale and race weight, which it applies to the movement)
			float pace = (r.scale > 0.0f ? r.scale : 1.0f) * (d.creature ? 1.0f : s.raceWeight(d.race.str(), d.female != 0));
			logf("golden: tactor	%s	%s	%s	%s	%s	%d,%d,%d,%d	%.0f,%.0f,%.0f	%.2f,%.2f	%d	%d", key.c_str(),
				r.idLower.c_str(), pkg.c_str(), foes.c_str(), d.creature ? "" : std::to_string(w.disposition(ri)).c_str(),
				r.fight >= 0 ? r.fight : d.fight, r.flee >= 0 ? r.flee : d.flee, r.alarm >= 0 ? r.alarm : d.alarm,
				r.hello >= 0 ? r.hello : d.hello, r.healthMax, r.magickaMax, r.fatigueMax, s.actorRunSpeed(ri) / pace,
				s.actorWalkSpeed(ri) / pace, r.gold, r.dead ? 1 : 0);
		}
		return true;
	}
	// DUMPLEVELED: for every leveled list, at each of golden_compare.py's player levels, every id a pick can give (nested
	// lists followed; "" when nothing can come): one line per list and level
	if (verb == "DUMPLEVELED")
	{
		static const int kLevels[] = { 1, 2, 3, 4, 5, 6, 8, 10, 12, 15, 18, 21, 25, 30 };
		std::vector<int> cand;
		std::function<void(const std::string&, int, int, std::set<std::string>&)> reach =
			[&](const std::string& list, int level, int depth, std::set<std::string>& out) {
				auto it = w.game.leveled.find(list);
				if (it == w.game.leveled.end() || depth > 8)
				{
					out.insert(lower(list));
					return;
				}
				if (it->second.none > 0)
					out.insert("");
				std::vector<int> ok;
				GameData::levCandidates(it->second, level, ok);
				if (ok.empty())
					out.insert("");
				for (int i : ok)
					reach(it->second.entries[i].second, level, depth + 1, out);
			};
		for (auto& kv : w.game.leveled)
			for (int level : kLevels)
			{
				std::set<std::string> out;
				reach(kv.first, level, 0, out);
				for (auto& s : out)
					logf("golden: levpick\t%s\t%d\t%s", lower(kv.first).c_str(), level, s.empty() ? "-" : s.c_str());
			}
		logf("test: dumped %d leveled lists", (int)w.game.leveled.size());
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
			if (!o || o->recType != REC_REPA)
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
	// MAKEPICK:<effect>: the spellmaking / enchanting screen's list of effects the player knows, tapped on that effect (refused
	// when it isn't in it); MAKETARGET:<skill or attribute>: the choice of which, tapped on that one
	if (verb == "MAKEPICK" && a.size() >= 2)
	{
		int want = atoi(a[1].c_str());
		for (auto& e : s.knownEffects())
			if (e.effect == want)
			{
				s.makeEffects.push_back(s.newEffect(e, 7));
				s.makeSel = (int)s.makeEffects.size() - 1;
				s.askEffectTarget();
				return true;
			}
		return fail("effect " + a[1] + " isn't in the list");
	}
	if (verb == "MAKETARGET" && a.size() >= 2)
	{
		if (!s.makePick || s.makeSel < 0 || s.makeSel >= (int)s.makeEffects.size())
			return fail("no skill or attribute being chosen");
		int k = s.makePick == 1 ? skillIndex(w, a[1]) : attrIndex(w, a[1]);
		if (k < 0)
			return fail(a[1] + " isn't a " + (s.makePick == 1 ? "skill" : "attribute"));
		(s.makePick == 1 ? s.makeEffects[s.makeSel].skill : s.makeEffects[s.makeSel].attribute) = k;
		s.makePick = 0;
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
		if (!o || o->recType != REC_BOOK)
			return fail(a[1] + " isn't a book");
		if (float* skip = w.itemScriptLocal(w.inventory[i].id, "pcskipequip"); skip && *skip == 1.0f)
		{
			w.setItemScriptLocal(w.inventory[i].id, "onpcequip", 1.0f);     // (its script acts instead: the inventory's rule)
			logf("test: reading %s: its script takes it (PCSkipEquip)", a[1].c_str());
			return true;
		}
		s.openScreen(SCR_BOOK);
		s.bookItem = w.inventory[i].id;
		logf("test: reading %s", a[1].c_str());
		return true;
	}
	return std::nullopt;
}
