// Line of sight and path grid routes for NPCs that chase or flee.
//
// Morrowind stores a path grid per cell (PGRD: points with links); NPCs walk straight at their
// goal while nothing is in the way, and along the grid (A*) when a wall is.
#include <algorithm>
#include <cmath>
#include <queue>

#include "mwworld/worldimp.hpp"

// The segment a..b passes through the box (the slab test)
static bool segmentHitsBox(const float a[3], const float b[3], const float lo[3], const float hi[3])
{
	float t0 = 0.0f, t1 = 1.0f;
	for (int k = 0; k < 3; k++)
	{
		float d = b[k] - a[k];
		if (fabsf(d) < 1e-6f)
		{
			if (a[k] < lo[k] || a[k] > hi[k])
				return false;
			continue;
		}
		float u = (lo[k] - a[k]) / d, v = (hi[k] - a[k]) / d;
		if (u > v)
			std::swap(u, v);
		t0 = fmaxf(t0, u);
		t1 = fminf(t1, v);
		if (t0 > t1)
			return false;
	}
	return true;
}

// Walls, the land and closed doors (and gates a script swings shut) block it (OpenMW's getLineOfSight: World, HeightMap
// and Door): an actor neither sees nor strikes through a shut door
bool World::lineOfSight(const float a[3], const float b[3], bool doors)
{
	for (LoadedCell* l : loaded)
	{
		float t;
		if (collisionRaycast(l->cell.collision, a, b, &t))
			return false;
		for (const CollisionBox& box : l->cell.blockers)
			if (doors && segmentHitsBox(a, b, box.min, box.max))
				return false;
	}
	return true;
}

static float dist3(const float* a, const float* b)
{
	float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
	return sqrtf(dx * dx + dy * dy + dz * dz);
}

// Nearest loaded path point that can be walked to straight from p (up to a few candidates)
int World::nearestPathPoint(const float p[3])
{
	std::vector<std::pair<float, int>> near;
	for (size_t i = 0; i < pathPoints.size(); i++)
	{
		const PathPoint& pp = pathPoints[i];
		if (pp.cell < 0 || !cells[pp.cell].live)
			continue;
		float d = dist3(pp.pos, p);
		if (d < 2500.0f)
			near.push_back({ d, (int)i });
	}
	std::sort(near.begin(), near.end());
	float from[3] = { p[0], p[1], p[2] + 50.0f };
	for (size_t k = 0; k < near.size() && k < 6; k++)
	{
		const PathPoint& pp = pathPoints[near[k].second];
		float to[3] = { pp.pos[0], pp.pos[1], pp.pos[2] + 50.0f };
		if (lineOfSight(from, to, false))
			return near[k].second;
	}
	return near.empty() ? -1 : near[0].second;
}

bool World::findPath(const float from[3], const float to[3], std::vector<int>& out)
{
	out.clear();
	int start = nearestPathPoint(from), goal = nearestPathPoint(to);
	if (start < 0 || goal < 0)
		return false;
	if (start == goal)
	{
		out.push_back(goal);
		return true;
	}
	// A* over the loaded points
	std::unordered_map<int, float> cost;
	std::unordered_map<int, int> came;
	typedef std::pair<float, int> Item;
	std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
	cost[start] = 0.0f;
	open.push({ dist3(pathPoints[start].pos, pathPoints[goal].pos), start });
	int expanded = 0;
	while (!open.empty() && expanded < 4000)
	{
		int cur = open.top().second;
		open.pop();
		if (cur == goal)
			break;
		expanded++;
		for (int nb : pathPoints[cur].links)
		{
			if (pathPoints[nb].cell < 0 || !cells[pathPoints[nb].cell].live)
				continue;
			if (pathPoints[nb].cell != pathPoints[cur].cell)
			{
				float a[3] = { pathPoints[cur].pos[0], pathPoints[cur].pos[1], pathPoints[cur].pos[2] + 50.0f };
				float b[3] = { pathPoints[nb].pos[0], pathPoints[nb].pos[1], pathPoints[nb].pos[2] + 50.0f };
				if (!lineOfSight(a, b, false))
					continue;             // across a wall: the border link isn't walkable
			}
			float c = cost[cur] + dist3(pathPoints[cur].pos, pathPoints[nb].pos);
			auto it = cost.find(nb);
			if (it != cost.end() && it->second <= c)
				continue;
			cost[nb] = c;
			came[nb] = cur;
			open.push({ c + dist3(pathPoints[nb].pos, pathPoints[goal].pos), nb });
		}
	}
	if (!came.count(goal))
		return false;
	for (int p = goal; p != start; p = came[p])
		out.push_back(p);
	// the first point too: skipped, a walker cut straight to the second across whatever lay between (the
	// prison ship's guard, over the stairwell opening); npcMoveTo drops it once there
	// (only while it's still ahead: once past it, the second point is nearer the walker than to it)
	if (out.empty() || dist3(from, pathPoints[out.back()].pos) > dist3(pathPoints[start].pos, pathPoints[out.back()].pos))
		out.push_back(start);
	std::reverse(out.begin(), out.end());
	return true;
}
