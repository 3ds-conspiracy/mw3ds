#pragma once

// Cells: one loaded into memory, and the list of all of them
#include "mwrender/animation.hpp"
#include "mwrender/objects.hpp"
#include "mwworld/esmstore.hpp"

// Geometry, textures and animated NPCs of a cell that is in memory
struct LoadedCell
{
	int index = -1;               // into World::cells
	Cell cell;
	ActorSet actors;
	std::vector<std::pair<int, u32>> glowRefs;   // its enchanted things lying about and their colours (the shimmer)
	std::vector<int> lightRefs;                  // its lights (on by default): people are lit by those near them
};

// An interior's water surface moved to a new height (SetWaterLevel: mwworld/cellstore.cpp)
void moveCellWater(Cell& c, float z);

// One cell of the level: its references stay loaded, its geometry only while the player is in or
// (outdoors) next to it. Exterior cells are 8192-unit grid squares.
struct LevelCell
{
	Atom name, file;
	bool interior = true;
	int gx = 0, gy = 0;               // exterior grid position
	int refBase = 0, refCount = 0;    // its references are refs[refBase .. refBase + refCount)
	LoadedCell* live = nullptr;
	int pathBase = 0, pathCount = 0;  // its path grid points are World::pathPoints[pathBase ..)
	bool refsLoaded = false;          // its objects are in memory (read when needed; evictCells drops far ones)
	u32 lastUsed = 0;                 // World::useTick when last read or loaded
	float askedAt = -1000.0f;         // World::time when its objects were last read or asked for (ensureRefs)
	float droppedAt = -1e9f;          // World::time when its objects last went out of memory (evictCell)
	bool askedBack = false;           // read back soon after they went: a script keeps asking (evictCells keeps it longer)
	int reads = 0;                    // times its objects were read (the tests)
	float freedAt = -1e9f;            // World::time when its geometry was last freed
	float loadedAt = -1e9f;           // World::time when its geometry last came in
	float sea = 0.0f;                 // exterior: share of its ground under open water (converter)
	bool noSleep = false;             // resting is illegal here (waiting only, or a bed)
	bool sky = false;                 // an interior that behaves as an exterior (Tribunal's Mournhold, OpenMW's QuasiEx)
	Atom region;                      // exterior (and an interior under the sky): its region (lowercase id)
	// Under the sky: outdoors, or an interior that behaves as an exterior (the sky, the weather and its light, the sun
	// and moons, region sounds); still an interior for GetInterior, torches, the map and loading
	bool underSky() const { return !interior || sky; }
};
