#pragma once

// What the token files share
#include <optional>
#include <3ds.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <queue>
#include <tuple>

extern const char* g_playerBlock;
#include <malloc.h>

bool makeTestSpell(Session& s, const std::string& name, const std::vector<SpellEffect>& fx);
bool makeTestItem(Session& s, const std::string& base, const std::string& id, int type, int charge,
	const std::vector<SpellEffect>& fx);
std::optional<bool> opsStats(Session& s, const std::vector<std::string>& a);
std::optional<bool> opsPose(Session& s, const std::vector<std::string>& a);
std::optional<bool> opsItems(Session& s, const std::vector<std::string>& a);
std::optional<bool> opsWorld(Session& s, const std::vector<std::string>& a);
std::optional<bool> opsSystem(Session& s, const std::vector<std::string>& a);
