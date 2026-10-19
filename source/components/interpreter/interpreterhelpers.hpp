#pragma once

// What the interpreter and the script functions (mwscript/*extensions.cpp) share: the context, arguments
#include "components/interpreter/scriptfunctions.hpp"

#include <optional>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_set>




// ---- Execution context

struct Ctx
{
	World& w;
	ScriptHost& host;
	int inst;            // index into w.scripts, -1 when running without locals
	int self;            // owning reference, -1 for none
	float dt;
	bool returned = false;
};

std::string argStr(Ctx& c, const Node& call, size_t i);
std::string displayCellName(World& w);
void messageBox(Ctx& c, const Node& n);
int magicEffectIndex(const std::string& arg);
bool statFunction(Ctx& c, const Node& n, bool onPlayer, Ref* r, int ref, float* out);
std::string gmstFormat(const std::string& fmt, int count, const std::string& name);
void localToWorld(const float rot[3], bool turnOnly, float v[3]);
std::optional<float> aiExtensions(Ctx& c, const Node& n, World& w, bool onPlayer, int ref, Ref* r);
std::optional<float> animationExtensions(Ctx& c, const Node& n, World& w, bool onPlayer, int ref, Ref* r);
std::optional<float> cellExtensions(Ctx& c, const Node& n, World& w, bool onPlayer, int ref, Ref* r);
std::optional<float> containerExtensions(Ctx& c, const Node& n, World& w, bool onPlayer, int ref, Ref* r);
std::optional<float> controlExtensions(Ctx& c, const Node& n, World& w, bool onPlayer, int ref, Ref* r);
std::optional<float> dialogueExtensions(Ctx& c, const Node& n, World& w, bool onPlayer, int ref, Ref* r);
std::optional<float> guiExtensions(Ctx& c, const Node& n, World& w, bool onPlayer, int ref, Ref* r);
std::optional<float> skyExtensions(Ctx& c, const Node& n, World& w, bool onPlayer, int ref, Ref* r);
std::optional<float> soundExtensions(Ctx& c, const Node& n, World& w, bool onPlayer, int ref, Ref* r);
std::optional<float> statsExtensions(Ctx& c, const Node& n, World& w, bool onPlayer, int ref, Ref* r);
std::optional<float> transformationExtensions(Ctx& c, const Node& n, World& w, bool onPlayer, int ref, Ref* r);
std::optional<float> miscExtensions(Ctx& c, const Node& n, World& w, bool onPlayer, int ref, Ref* r);
float call(Ctx& c, const Node& n);
float eval(Ctx& c, const Node& n);
void assign(Ctx& c, const Node& target, float value);

static inline float argNum(Ctx& c, const Node& call, size_t i, float fallback = 0.0f)
{
	if (i >= call.kids.size())
		return fallback;
	const Node& a = call.kids[i];
	if (a.kind == N_STR)
		return (float)atof(a.a.c_str());
	return eval(c, a);
}

// The other gold coin records are gold_001 to item scripts (OpenMW)
static inline std::string goldId(const std::string& id)
{
	std::string l = lower(id);
	return l == "gold_005" || l == "gold_010" || l == "gold_025" || l == "gold_100" ? "gold_001" : l;
}
