#pragma once

// The player's stats: attributes, skills, level, health / magicka / fatigue, spells
#include "mwworld/esmstore.hpp"

enum Attribute { ATTR_STRENGTH, ATTR_INTELLIGENCE, ATTR_WILLPOWER, ATTR_AGILITY, ATTR_SPEED, ATTR_ENDURANCE, ATTR_PERSONALITY, ATTR_LUCK };

// The id a class made at character creation goes by (no record in Morrowind.esm has a $)
static const char* const kCustomClassId = "$custom";

struct PlayerStats
{
	std::string name = "Stranger";
	std::string race = "imperial", cls, birthsign;
	// The class made at character creation (Create Custom Class): cls is kCustomClassId while it is the player's.
	// Kept with the save, as OpenMW keeps the created class record
	ClassDef customClass = {};
	std::string head, hair;           // body part ids chosen at character creation ("" = the race's first)
	bool female = false;
	int attributes[8] = {};
	int skills[27] = {};
	int level = 1;
	float health = 50, healthMax = 50, magicka = 50, magickaMax = 50, fatigue = 100, fatigueMax = 100;
	// What maximum magicka is made of (refitMagicka): magicka per point of Intelligence (fPCbaseMagickaMult and the
	// race's and sign's Fortify Maximum Magicka abilities), the active spells' Fortify Maximum Magicka and Fortify
	// Magicka. Rebuilt by recomputeStats, not saved
	float magickaMult = 1.0f, magickaFortify = 0.0f, magickaFlat = 0.0f;
	// Growth since character creation (recomputeStats adds it on top of race / class / sign)
	float skillProgress[27] = {};
	int skillBonus[27] = {};
	int attrBonus[8] = {};
	int attrSkillUps[8] = {};          // major / minor skill increases per governing attribute this level
	float attrDamage[8] = {}, skillDamage[27] = {};   // Damage Attribute / Skill until restored
	// What lasting effects took below 0 (OpenMW keeps the modifier whole and shows max(0, base + modifier)):
	// owed back before the stat rises again, so an effect's end restores exactly what it took (recomputeStats: 0)
	int attrBelow[8] = {}, skillBelow[27] = {};
	int levelProgress = 0;             // major / minor skill increases since the last level
	float healthBonus = 0.0f;
	int skillCreation[27] = {};        // race + class skills (recomputeStats), before use, training, books, abilities and drains
	int attrCreation[8] = {};          // race + class attributes (recomputeStats), before level-up raises, abilities and drains
	std::vector<std::string> spells;   // bought / given by scripts (race and sign spells are implied)
	std::unordered_set<std::string> booksRead;   // skill books already read (lowercase ids)
	std::string selectedSpell;
	// Quick keys (ZL + D-pad up / right / down / left): "inv:<item>" or "magic:<spell or item:id>"
	std::string quickKeys[4];
	std::unordered_map<std::string, int> powerUsedDay;
};
