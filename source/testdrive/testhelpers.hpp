#pragma once

// What the test driver files share
#include <3ds.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <queue>
#include <tuple>

extern const char* g_playerBlock;
#include "mwgui/windowmanagerimp.hpp"
#include <malloc.h>

extern float s_movedFrom[2];
std::vector<std::string> split(const std::string& s);
bool cellNameMatches(const std::string& cell, std::string want);
float doorWalkRank(Session& s, const Ref& r, float d3);
bool hasArg(const std::string& what);
bool numberOf(Session& s, const std::string& what, const std::string& arg, float& v);
extern std::map<std::string, float> s_snap;
float wanted(const std::string& spec, const std::string& key, bool& ok);
int screenByName(const std::string& n);
bool leadSpot(Session& s, int victim, float* out);
bool mechanicsOp(Session& s, const std::vector<std::string>& a);
bool splitDeadEnd(Session& s, const float from[3], const float to[3], float entry[3], int* entryIndex);
void walkTarget(const Ref& r, bool activate, float out[3]);
bool planPath(Session& s, const float from[3], const float to[3], bool spot, std::vector<int>& path, bool ownPiece = false);
bool standsOnFloor(Session& s);
std::string handItem(Session& s, bool takeWeapon);
bool spotBeside(Session& s, const float aim[3], float fromZ, float out[2]);

static inline std::string spaced(std::string s)
{
	for (auto& c : s)
		if (c == '_')
			c = ' ';
	return s;
}

static inline std::string squash(const std::string& s)
{
	std::string out;
	for (char c : s)
		if (c != ' ' && c != '_' && c != '-')
			out += (char)tolower((unsigned char)c);
	return out;
}

// A skill / attribute by number or by name ("longblade", "Long_Blade")
static inline int skillIndex(const World& w, const std::string& name)
{
	if (!name.empty() && isdigit((unsigned char)name[0]))
		return atoi(name.c_str());
	for (int k = 0; k < 27; k++)
		if (squash(skillName(w, k)) == squash(name))
			return k;
	return -1;
}

static inline int attrIndex(const World& w, const std::string& name)
{
	if (!name.empty() && isdigit((unsigned char)name[0]))
		return atoi(name.c_str());
	for (int k = 0; k < 8; k++)
		if (squash(attrName(w, k)) == squash(name))
			return k;
	return -1;
}

// preferLocked: of shared door / container ids the locked ones first (a lockpick is held up to what is locked)
static inline int findRef(Session& s, const std::string& id, bool byWalk = true, bool preferLocked = false)
{
	int ri = s.testFindRef(id);
	if (ri < 0)
		ri = s.testFindRef(spaced(id));
	if (ri < 0)
	{
		// an id with both underscores and spaces (ghost_npc_conoon chodal): a token can't hold the space
		const std::string want = lower(spaced(id));
		s.w.forLoadedRefs([&](int i) {
			if (ri < 0 && lower(spaced(s.w.refs[i].idLower)) == want)
				ri = i;
		});
	}
	// several share the id (a creature placed again after one died, a field of identical plants, shared door ids): the
	// nearest living one in the loaded cells, as a player takes the one in front of them (a dead one only if all are)
	if (ri >= 0 && ri != s.testPlaced && !s.testPlacedAll.count(ri))
	{
		const std::string want = s.w.refs[ri].idLower;
		const bool wantDead = s.w.refs[ri].dead;
		const float* pf = s.w.player.feet;
		float best = 1e30f;
		int pick = -1;
		s.w.forLoadedRefs([&](int i) {
			const Ref& c = s.w.refs[i];
			if (c.idLower != want || !s.w.active(i) || (wantDead && !c.dead))
				return;
			if (c.dead && !wantDead)
				return;
			float dx = c.pos[0] - pf[0], dy = c.pos[1] - pf[1], dz = c.pos[2] - pf[2], d = dx * dx + dy * dy + dz * dz;
			// (shared door ids: the one the player can walk to soonest, not the one nearest through a wall or a floor)
			if (byWalk && c.recType == REC_DOOR && d < 1e11f)
				d = doorWalkRank(s, c, d);
			if (d < best)
			{
				best = d;
				pick = i;
			}
		});
		if (pick >= 0)
			ri = pick;
	}
	return ri;
}

static inline int carried(const World& w, const std::string& id)
{
	std::string a = lower(id), b = lower(spaced(id));
	for (size_t k = 0; k < w.inventory.size(); k++)
		if (lower(w.inventory[k].id) == a || lower(w.inventory[k].id) == b)
			return (int)k;
	return -1;
}

// The last item the player enchanted that they still carry
static inline const InventoryItem* lastMade(const World& w)
{
	for (int k = (int)w.madeItems.size() - 1; k >= 0; k--)
	{
		int i = carried(w, w.madeItems[k]);
		if (i >= 0)
			return &w.inventory[i];
	}
	return nullptr;
}

// "<a>,<b>" as two numbers
static inline void twoNumbers(const std::string& arg, float& a, float& b)
{
	a = (float)atof(arg.c_str());
	size_t c = arg.find(',');
	b = c == std::string::npos ? 0.0f : (float)atof(arg.c_str() + c + 1);
}

// An enchanted item the test makes, carried: a copy of the base item with the enchantment, as makeEnchantment
// (mwmechanics/enchanting.cpp) makes one, without the enchanter, the soul, the points, the price or the roll
// where a float is, for ending it at a door: an interior, or outdoors (-2: the grid cells' changes are not doors)
static inline int floatPlace(const World& w)
{
	return w.current >= 0 && w.cells[w.current].interior ? w.current : -2;
}

// ge / gt / eq / ne / lt / le (words: '+' and '>' can't go in a harness token)
static inline bool compare(float a, const std::string& op, float b)
{
	if (op == "ge") return a >= b;
	if (op == "gt") return a > b;
	if (op == "ne") return a != b;
	if (op == "lt") return a < b;
	if (op == "le") return a <= b;
	return a == b;
}

// Whether the crosshair is on that reference now, from the view as it is this frame (the session's own pick of last frame is
// the view before the driver turned it)
static inline bool aimsAt(Session& s, int ref)
{
	const Player& p = s.w.player;
	float cp = cosf(p.pitch);
	float eye[3] = { p.feet[0], p.feet[1], playerEyeZ(p) };
	float dir[3] = { sinf(p.yaw) * cp, cosf(p.yaw) * cp, sinf(p.pitch) };
	return worldPick(s.w, eye, dir, 192.0f + s.w.effectTotal(59) * 22.0f) == ref;
}

// An escort fallen behind (ESCORT:<npc>): the player stands and lets them catch up, as a player waits. Not when they
// no longer follow (a quest script stopped them: their package is the script's idle, or they wander)
static inline bool escortLagging(Session& s, const std::string& escort, float limit, int* who = nullptr)
{
	if (escort.empty())
		return false;
	int e = findRef(s, escort);
	if (who)
		*who = e;
	if (e < 0 || !s.w.active(e) || s.w.refs[e].dead)
		return false;
	int pkg = s.w.refs[e].aiPackage;
	if (pkg == AIPKG_IDLE || pkg == AIPKG_NONE || pkg == AIPKG_WANDER)
		return false;
	float d = s.w.distanceToPlayer(e);
	return d > limit && d < 20000.0f;
}

// A route out of where the player is stuck, found by trying the player's own steps (the game's playerUpdate on a copy: run or
// running jump, sixteen headings, swimming up / level / down) and following the cheapest-looking reached spot towards the goal.
// Gives the spots reached on the way, from the first step. Stops after a number of spots tried, or 90 s of the machine's time.
// shore: the way ashore, not to the goal: any dry ground above the water's surface ends it (the goal only steers the search).
// A swimmer under a cliff has no route to a door on the hill above, and a search for one never finds the beach
static inline bool floodRoute(Session& s, const float goal[3], float radius, std::vector<TestDriver::SwimLeg>& out, float maxDz = 200.0f,
	bool dive = false, bool shore = false)
{
	struct Node { Player p; int parent; bool jump; int depth; float yaw, pitch; int frames; };
	Scene sc = s.w.scene();
	const bool wet = shore && sc.here && sc.here->hasWater();
	const float dryZ = wet ? sc.here->waterZ + 10.0f : 0.0f;
	// (a deque: room for 6000 reserved at once was a 726 KB block on the main thread, which an Old 3DS late in a session
	// didn't have, and a `new` that fails there stops the game)
	std::deque<Node> nodes;
	std::map<std::tuple<int, int, int>, int> seen;
	auto key = [](const Player& p) { return std::make_tuple((int)floorf(p.feet[0] / 64), (int)floorf(p.feet[1] / 64), (int)floorf(p.feet[2] / 48)); };
	auto cost = [&](const Node& n) {
		return hypotf(n.p.feet[0] - goal[0], n.p.feet[1] - goal[1]) + fabsf(n.p.feet[2] - goal[2]) * 0.5f + n.depth * 25.0f;
	};
	typedef std::pair<float, int> Entry;
	std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry> > open;
	nodes.push_back({ s.w.player, -1, false, 0, 0.0f, 0.0f, 0 });
	nodes[0].p.levitate = 0.0f;
	seen[key(nodes[0].p)] = 0;
	open.push(Entry(cost(nodes[0]), 0));
	float minZ = fminf(s.w.player.feet[2], goal[2]) - 1500.0f;
	u64 t0 = osGetTime();
	int found = -1;
	// A stuck walk's search under the v2 kit stops after 690 spots, the same on every machine: it stopped after 10 s of the
	// machine's time, which a loaded PC spent on fewer spots, and uber-tribunal-temple's way out of the Puzzle Canal (found
	// at spot 648 alone) was missed in a full suite run. None of the uber runs' searches found a way past 690. Others try
	// 6000 (a dive's search is its way); 90 s of the machine's time stays as the backstop on a slow console
	const int maxSpots = s.kitV2() && !dive ? 690 : 6000;
	const u64 limit = 90000;
	while (!open.empty() && found < 0 && (int)nodes.size() < maxSpots && osGetTime() - t0 < limit)
	{
		int bi = open.top().second;
		open.pop();
		for (int h = 0; h < 16 && found < 0; h++)
			for (int jmp = 0; jmp < (nodes[bi].p.onGround ? 2 : 1) && found < 0; jmp++)
				for (int pi = 0; pi < (nodes[bi].p.swimming ? 3 : 1) && found < 0; pi++)
				{
					Player p = nodes[bi].p;
					p.yaw = h * 3.14159265f / 8.0f;
					p.pitch = pi == 1 ? -0.7f : pi == 2 ? 0.7f : 0.0f;
					p.vz = 0.0f;
					for (int f = 0; f < 24 && found < 0; f++)
					{
						PlayerInput in = {};
						in.moveY = 1.0f;
						in.jump = jmp && f == 0;
						playerUpdate(p, sc, in, 1.0f / 30.0f);
						if (f % 4 == 3 && (p.onGround || p.swimming) && p.feet[2] > minZ)
						{
							auto k = key(p);
							if (!seen.count(k))
							{
								int ni = (int)nodes.size();
								nodes.push_back({ p, bi, jmp != 0, nodes[bi].depth + 1, h * 3.14159265f / 8.0f, pi == 1 ? -0.7f : pi == 2 ? 0.7f : 0.0f, f + 1 });
								seen[k] = ni;
								open.push(Entry(cost(nodes[ni]), ni));
								if (wet ? (p.onGround && !p.swimming && p.feet[2] > dryZ)
									: hypotf(p.feet[0] - goal[0], p.feet[1] - goal[1]) < radius && fabsf(p.feet[2] - goal[2]) < maxDz)
									found = ni;
							}
						}
					}
				}
	}
	logf("drive: route by trial steps: %d spots tried in %.1f s, %s", (int)nodes.size(), (osGetTime() - t0) / 1000.0f, found >= 0 ? "found" : "none");
	if (found < 0)
		return false;
	out.clear();
	for (int i = found; nodes[i].parent >= 0; i = nodes[i].parent)
	{
		TestDriver::SwimLeg leg = { { nodes[i].p.feet[0], nodes[i].p.feet[1], nodes[i].p.feet[2] }, nodes[i].p.swimming, nodes[i].jump, nodes[i].yaw, nodes[i].pitch, nodes[i].frames / 30.0f };
		out.push_back(leg);
	}
	std::reverse(out.begin(), out.end());
	return true;
}
