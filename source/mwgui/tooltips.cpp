// Tooltips: item and spell details, the crosshair's name and prompt
#include <cmath>
#include <cstdio>
#include <cstring>

#include "mwgui/windowmanagerimp.hpp"
#include "mwbase/session.hpp"
#include "mwclass/classes.hpp"
#include <algorithm>
#include <array>
#include <functional>
#include <sys/stat.h>
#include <tex3ds.h>
#include <citro2d.h>
#include <cstdlib>
#include <malloc.h>

std::string Session::effectLine(const SpellEffect& e, bool showRange)
{
	std::string s = effectLabel(e.effect, e.skill, e.attribute);
	auto me = w.game.magicEffects.find(e.effect);
	int flags = me != w.game.magicEffects.end() ? me->second.flags : 0;
	char buf[64];
	// Fortify Maximum Magicka is a multiple of Intelligence (the birthsigns', 1.5x INT), not points (OpenMW's effect widget)
	if (e.effect == 84)
	{
		snprintf(buf, sizeof(buf), " %g", e.min / 10.0f);
		s += buf + w.game.gmstLabel("sxtimesint", "x INT");
	}
	else if (!(flags & 0x8) && (e.min || e.max))
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
	// its type's lines: kind, damage or armor rating, condition, quality (mwclass/)
	MWClass::toolTipStats(*this, *o, it);
	char buf[96];
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
	// its type's: a potion's effects, an ingredient's known ones, a book or a scroll (mwclass/)
	MWClass::toolTipContents(*this, *o);
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
	bool talks = t.recType == REC_CREA && t.actor >= 0 && ((w.game.actors[t.actor].services & 0x3FFFF) || w.game.speakers.count(t.idLower));
	std::string verb = t.dead ? "Search " : t.recType == REC_NPC_ || talks ? "Talk to " : t.recType == REC_CREA ? "" : t.recType == REC_DOOR ? "Open " : "Use ";
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
