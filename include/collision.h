#pragma once

#include <3ds.h>
#include <cstdio>
#include <vector>

// Static collision triangles of a cell, bucketed into a 2D (XY) grid.
struct CollisionMesh
{
	std::vector<float> verts;     // xyz per vertex
	std::vector<u32> tris;        // 3 vertex indices per triangle
	float originX = 0, originY = 0, cellSize = 128;
	u32 nx = 0, ny = 0;
	std::vector<u32> cellStart;   // nx*ny+1 offsets into cellTris
	std::vector<u32> cellTris;
	std::vector<u32> stamp;       // per-triangle visit mark for queries
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

// Highest surface directly below (x, y) with zBottom <= z <= zTop.
bool collisionFloor(CollisionMesh& mesh, float x, float y, float zTop, float zBottom, float* zOut);

// The floor an actor's feet rest on: the highest of collisionFloor under (x, y) and four spots
// `reach` units around it (as OpenMW's actor box rests on the highest point under it, so on a slope
// or a ledge's edge the middle is a little above the floor under it)
bool collisionFootFloor(CollisionMesh& mesh, float x, float y, float reach, float zTop, float zBottom, float* zOut);
static const float kActorFootReach = 15.0f;

// Whether the mesh's grid covers (x, y), give or take `margin` (a query outside it would still look at the edge buckets)
inline bool collisionReaches(const CollisionMesh& m, float x, float y, float margin)
{
	return m.nx > 0 && x >= m.originX - margin && y >= m.originY - margin && x <= m.originX + m.nx * m.cellSize + margin
		&& y <= m.originY + m.ny * m.cellSize + margin;
}
