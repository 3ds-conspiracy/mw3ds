#pragma once

// Races (OpenMW's ESM::Race)
#include <string>
#include <vector>

struct RaceDef
{
	std::string id, name, desc;
	std::vector<std::pair<int, int>> skillBonus;   // skill, bonus
	int attributes[8][2];                         // [attribute][male, female]
	float weight[2] = { 1.0f, 1.0f };             // [male, female]: scales how fast a person of the race moves
	bool playable, beast;
	std::vector<std::string> spells;
};
