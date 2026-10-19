// The player's race, class, birthsign and derived stats
#include "mwworld/worldimp.hpp"

#include <3ds.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" u32 __ctru_heap_size;
#include <malloc.h>

const RaceDef* World::race() const
{
	for (auto& r : game.races)
		if (strcasecmp(r.id.c_str(), stats.race.c_str()) == 0)
			return &r;
	return nullptr;
}

const ClassDef* World::playerClass() const
{
	if (!stats.customClass.id.empty() && strcasecmp(stats.customClass.id.c_str(), stats.cls.c_str()) == 0)
		return &stats.customClass;
	for (auto& c : game.classes)
		if (strcasecmp(c.id.c_str(), stats.cls.c_str()) == 0)    // (Athletics asks every frame a run lasts: no copies)
			return &c;
	return nullptr;
}

const BirthDef* World::birthsign() const
{
	for (auto& b : game.birthsigns)
		if (strcasecmp(b.id.c_str(), stats.birthsign.c_str()) == 0)
			return &b;
	return nullptr;
}

// Character creation rules: race base attributes, +10 for the class's favored ones;
// skills 5, minors +10, majors +25, race bonuses, +5 for the class specialization;
// constant abilities from race and birthsign on top.
void World::recomputeStats()
{
	const RaceDef* r = race();
	const ClassDef* c = playerClass();
	const BirthDef* b = birthsign();
	std::fill(std::begin(stats.attrBelow), std::end(stats.attrBelow), 0);
	std::fill(std::begin(stats.skillBelow), std::end(stats.skillBelow), 0);
	for (int i = 0; i < 8; i++)
	{
		stats.attributes[i] = r ? r->attributes[i][stats.female ? 1 : 0] : 40;
		if (c && (c->attributes[0] == i || c->attributes[1] == i))
			stats.attributes[i] += 10;
		stats.attrCreation[i] = stats.attributes[i];
	}
	for (int s = 0; s < 27; s++)
	{
		stats.skills[s] = 5;
		if (c)
		{
			for (int k = 0; k < 5; k++)
			{
				if (c->major[k] == s) stats.skills[s] += 25;
				if (c->minor[k] == s) stats.skills[s] += 10;
			}
			if (s < (int)game.skills.size() && game.skills[s].specialization == c->specialization)
				stats.skills[s] += 5;
		}
	}
	if (r)
		for (auto& bonus : r->skillBonus)
			if (bonus.first >= 0 && bonus.first < 27)
				stats.skills[bonus.first] += bonus.second;
	for (int s = 0; s < 27; s++)
		stats.skillCreation[s] = stats.skills[s];

	float magickaMult = game.gmstf("fpcbasemagickamult", 1.0f);
	std::vector<std::string> abilities;
	if (r) abilities.insert(abilities.end(), r->spells.begin(), r->spells.end());
	// Worn constant-effect enchantments count as abilities while worn
	for (auto& it : inventory)
		if (it.equipped)
			if (const SpellDef* en = enchantmentOf(it))
				if (en->type == ENCH_CONSTANT)
					abilities.push_back(en->id);
	if (b) abilities.insert(abilities.end(), b->spells.begin(), b->spells.end());
	// The player's own abilities and diseases (AddSpell, caught from creatures) work the same way
	for (auto& id : stats.spells)
	{
		auto sp = game.spells.find(lower(id));
		if (sp != game.spells.end() && sp->second.type >= 1 && sp->second.type <= 3)
			abilities.push_back(id);
	}
	this->abilities.clear();
	// Abilities' and worn constant enchantments' fortifies and drains are modifiers on top (OpenMW's modified value):
	// only the base is held to 100, so the Boots of Blinding Speed take Speed past it
	int attrMod[8] = {}, skillMod[27] = {};
	for (auto& id : abilities)
	{
		auto it = game.spells.find(lower(id));
		if (it == game.spells.end() || ((it->second.type < 1 || it->second.type > 3) && it->second.type != ENCH_CONSTANT))
			continue;                                  // abilities, diseases, worn constant enchantments
		// Corprus: its drains and fortifies grow a point a day since it was caught
		int scale = 1;
		for (auto& e : it->second.effects)
			if (e.effect == 132)
				scale = corprusLevel();
		for (const SpellEffect& e0 : it->second.effects)
		{
			SpellEffect e = e0;
			if (e.effect == 132 && corprusSince < -1.5f)
				continue;                                  // cured: the Corprus effect is gone
			if (e.effect == 17 || e.effect == 79)
				e.min *= scale;
			if (e.effect == 79 && e.attribute >= 0 && e.attribute < 8) attrMod[e.attribute] += e.min;
			else if (e.effect == 83 && e.skill >= 0 && e.skill < 27) skillMod[e.skill] += e.min;
			else if (e.effect == 84) magickaMult += e.min / 10.0f;
			else if (e.effect == 17 && e.attribute >= 0 && e.attribute < 8) attrMod[e.attribute] -= e.min;
			else if (e.effect == 21 && e.skill >= 0 && e.skill < 27) skillMod[e.skill] -= e.min;
			else          // resistances, weaknesses, night eye ...: read while they last (for good)
				this->abilities.push_back({ e.effect, e.attribute, e.skill, (float)e.min, 1e9f, it->second.name });
		}
	}
	for (int i = 0; i < 8; i++)
	{
		stats.attributes[i] = (stats.attributes[i] + stats.attrBonus[i] > 100 ? 100 : stats.attributes[i] + stats.attrBonus[i]) + attrMod[i];
		// OpenMW cuts (base - damage) to a whole number, so part of a point of damage takes a whole point
		stats.attributes[i] = std::max(0, stats.attributes[i] - (int)ceilf(stats.attrDamage[i] - 0.001f));
	}
	for (int k = 0; k < 27; k++)
	{
		stats.skills[k] = (stats.skills[k] + stats.skillBonus[k] > 100 ? 100 : stats.skills[k] + stats.skillBonus[k]) + skillMod[k];
		stats.skills[k] = std::max(0, stats.skills[k] - (int)ceilf(stats.skillDamage[k] - 0.001f));
	}
	const int* a = stats.attributes;
	// OpenMW: base health is fixed at creation (floor of half Strength + Endurance) and grows only by the level-up
	// gains; raised, fortified or drained attributes do not move it (spec/derived.md)
	stats.healthMax = floorf(0.5f * (stats.attrCreation[ATTR_STRENGTH] + stats.attrCreation[ATTR_ENDURANCE])) + stats.healthBonus;
	stats.magickaMax = a[ATTR_INTELLIGENCE] * magickaMult;
	stats.fatigueMax = a[ATTR_STRENGTH] + a[ATTR_WILLPOWER] + a[ATTR_AGILITY] + a[ATTR_ENDURANCE];
	stats.health = stats.healthMax;
	stats.magicka = stats.magickaMax;
	stats.fatigue = stats.fatigueMax;
	reapplyEffects();
}
