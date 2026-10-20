#pragma once

// Races (OpenMW's ESM::Race)
#include <string>
#include <vector>

struct RaceDef
{
	std::string id, name, desc;
	std::vector<std::pair<int, int>> skillBonus;   // skill, bonus
	int attributes[8][2];                         // [attribute][male, female]
	bool playable, beast;
	std::vector<std::string> spells;
};
