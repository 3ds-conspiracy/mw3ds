#pragma once

#include <3ds.h>
#include <cstdio>
#include <vector>

// Indices kept in 16 bits while they all fit (the converter writes them so: half the memory, and a town's cells hold a
// few hundred thousand), in 32 when the file has wider ones or one is set past 16 bits
struct IndexList
{
	std::vector<u16> narrow;
	std::vector<u32> wide;
	bool isWide = false;
	u32 operator[](size_t i) const { return isWide ? wide[i] : narrow[i]; }
	size_t size() const { return isWide ? wide.size() : narrow.size(); }
	void set(size_t i, u32 v)
	{
		if (!isWide && v > 0xFFFF)
		{
			wide.assign(narrow.begin(), narrow.end());
			std::vector<u16>().swap(narrow);
			isWide = true;
		}
		if (isWide)
			wide[i] = v;
		else
			narrow[i] = (u16)v;
	}
};

// Static collision triangles of a cell, bucketed into a 2D (XY) grid.
struct CollisionMesh
{
	std::vector<float> verts;     // xyz per vertex
	IndexList tris;               // 3 vertex indices per triangle
	float originX = 0, originY = 0, cellSize = 128;
	u32 nx = 0, ny = 0;
	std::vector<u32> cellStart;   // nx*ny+1 offsets into cellTris
	IndexList cellTris;
	std::vector<u16> stamp;       // per-triangle visit mark for queries (queryId, started over when it passes 16 bits)
	u32 queryId = 0;
	u32 farVert = 0;              // 0: none yet; else 3 vertices far below everything (hidden triangles)
};

// A reference's own triangles (first, count) stop / start colliding (it was disabled / enabled): hidden
// ones point at a small triangle far below the world. saved holds the originals while hidden.
void collisionHideTris(CollisionMesh& mesh, u32 first, u32 count, std::vector<u32>& saved);
void collisionShowTris(CollisionMesh& mesh, u32 first, u32 count, std::vector<u32>& saved);

// version: the cell file's (6: welded s16 vertices and narrow indices, 7: also as planes of differences)
bool collisionRead(CollisionMesh& mesh, FILE* f, u32 version);

// Pushes a sphere out of every triangle it overlaps. With horizontalOnly the push is
// confined to XY (used for body spheres so floors and ramps never shove the player).
// Returns true if anything was touched.
bool collisionPushSphere(CollisionMesh& mesh, float center[3], float radius, bool horizontalOnly);

// First hit of the segment a -> b: *t = fraction of the way (0..1)
bool collisionRaycast(CollisionMesh& mesh, const float a[3], const float b[3], float* t);

// Highest surface directly below (x, y) with zBottom <= z <= zTop. A surface steeper than maxSlope degrees is a
// wall, not a floor (60 for placing things and resting actors; the player walks on kWalkSlope). A limit under 60
// is for the land only: objects (stair ramps, rocks) keep 60, unless objectSlope is under that (90: no limit of its own)
bool collisionFloor(CollisionMesh& mesh, float x, float y, float zTop, float zBottom, float* zOut, float maxSlope = 60.0f,
	float objectSlope = 90.0f);
// The steepest land the player walks on: OpenMW's Constants::sMaxSlope (46 degrees); steeper slides as a wall
static const float kWalkSlope = 46.0f;
// The land's surface over (x, y) with zBottom <= z <= zTop, whatever its slope (land triangles only, terrain.py's):
// a body under the land, where no floor test from its feet up finds the surface
bool collisionLandFloor(CollisionMesh& mesh, float x, float y, float zTop, float zBottom, float* zOut);

// The floor an actor's feet rest on: the highest of collisionFloor under (x, y) and four spots
// `reach` units around it (as OpenMW's actor box rests on the highest point under it, so on a slope
// or a ledge's edge the middle is a little above the floor under it)
bool collisionFootFloor(CollisionMesh& mesh, float x, float y, float reach, float zTop, float zBottom, float* zOut,
	float maxSlope = 60.0f, float objectSlope = 90.0f);
static const float kActorFootReach = 15.0f;

// Whether the mesh's grid covers (x, y), give or take `margin` (a query outside it would still look at the edge buckets)
inline bool collisionReaches(const CollisionMesh& m, float x, float y, float margin)
{
	return m.nx > 0 && x >= m.originX - margin && y >= m.originY - margin && x <= m.originX + m.nx * m.cellSize + margin
		&& y <= m.originY + m.ny * m.cellSize + margin;
}
