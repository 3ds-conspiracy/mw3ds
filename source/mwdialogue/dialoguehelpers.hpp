#pragma once

// What the dialogue files share: who is being talked to, and the filter's helpers

#include <algorithm>
#include <cstdio>
#include <map>
#include <cstring>


bool compare(char op, float a, float b);
extern int s_talkRef;
extern bool s_talked;
std::string explicitTopic(const GameData& g, const std::string& name, std::string* shown);

static inline std::string raceName(World& w, const std::string& id)
{
	for (auto& r : w.game.races)
		if (lower(r.id) == lower(id))
			return r.name;
	return id;
}

static inline std::string className(World& w, const std::string& id)
{
	for (auto& c : w.game.classes)
		if (lower(c.id) == lower(id))
			return c.name;
	return id;
}
