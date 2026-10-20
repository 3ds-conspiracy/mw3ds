#pragma once

// Classes (OpenMW's ESM::Class)
#include <string>

struct ClassDef
{
	std::string id, name, desc;
	int attributes[2], specialization, major[5], minor[5];
	bool playable;
};
