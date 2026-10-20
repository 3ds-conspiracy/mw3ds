#pragma once

// Factions (OpenMW's ESM::Faction)
#include <string>
#include <unordered_map>
#include <vector>

struct FactionDef
{
	std::string name;
	std::vector<std::string> ranks;
	int attrs[2] = { -1, -1 };               // favoured attributes
	int reqs[10][5] = {};                    // per rank: attribute 1, attribute 2, primary skill, favoured skill, reputation
	std::vector<int> skills;                 // favoured skills
	std::unordered_map<std::string, int> reactions;   // lowercase faction -> how this one feels about it
};
