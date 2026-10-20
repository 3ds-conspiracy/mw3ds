#pragma once

// Path grid points (PGRD)
#include <functional>

// Path grid point (PGRD): where NPCs can walk, with the points it connects to
struct PathPoint
{
	float pos[3];
	std::vector<int> links;           // global indices
	int cell;
};
