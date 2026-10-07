#pragma once

#include <3ds.h>
#include <citro3d.h>
#include <string>
#include <unordered_map>
#include <vector>

#include "collision.h"

// Textures shared by the cells that are loaded at the same time (the exterior keeps 3 x 3)
struct TextureCache
{
	struct Entry { C3D_Tex tex; bool ok; int refs; int fails; };
	std::unordered_map<std::string, Entry*> entries;
	u32 bytes = 0;

	// Loads <dataDir>/textures/<name> the first time; nullptr when it can't be read
	C3D_Tex* acquire(const char* dataDir, const std::string& name);
	void release(const std::string& name);
	// A texture that failed (linear memory full at the time) is tried again; no count taken. nullptr when still failed
	C3D_Tex* recheck(const char* dataDir, const std::string& name);
	// The same past recheck's three tries, while there is room for it now (a loaded cell's textures, now and then)
	C3D_Tex* retry(const char* dataDir, const std::string& name);
};

// Batch render-state flags, matching tools/convert/convert_cell.py
enum
{
	BATCH_BLEND = 1, BATCH_TEST = 2, BATCH_TWO_SIDED = 4, BATCH_CLAMP = 8, BATCH_ADDITIVE = 16,
	BATCH_DECAL = 32,     // coplanar overlay (terrain layers): equal depth passes, no depth write
	BATCH_SCROLL = 64,    // texture coordinates drift over time (water, clouds)
	BATCH_RISE = 128,     // texture coordinates climb (smoke, steam and spark particles); with CLAMP: a flame's flicker
};

// Cell header flags
enum { CELL_INTERIOR = 1, CELL_WATER = 2, CELL_SKY = 4 };

// Vertex layout in .cel files: f32 pos[3], f32 uv[2], u8 rgba[4]
static const int CELL_VERTEX_SIZE = 24;
// Packed layout: s16 pos[4] (w unused), s16 uv[2], u8 rgba[4]
static const int CELL_VERTEX_SIZE_PACKED = 16;

// Exterior view distance (linear fog, world units); distant land fills in past the loaded 3 x 3
static const float kExteriorFogStart = 3000.0f, kExteriorFogEnd = 12000.0f;
// People and creatures further than this are a few pixels tall: not posed or drawn
static const float kActorViewDistance = 6000.0f;

struct CellBatch
{
	int tex;              // index into Cell::textures, -1 = untextured
	u8 flags, alphaRef;
	u32 numVerts, numIndices;
	void* verts;          // linear memory
	u16* indices;         // linear memory
	float bmin[3], bmax[3];   // world-space bounds (object space for door meshes)
	float maxDist;            // not drawn farther than this (small exterior clutter); 0 = any distance
	// Cell format 5 stores s16 positions / UVs (16-byte vertices); older files float (24 bytes)
	float posOffset[3] = { 0, 0, 0 }, posScale[3] = { 1, 1, 1 }, uvScale[2] = { 1, 1 };
	u32 stride = CELL_VERTEX_SIZE;
};

struct __attribute__((packed)) CellActor
{
	char id[32], name[32];
	float pos[3], yaw, radius, height;
};

// Axis-aligned blocker updated every frame (closed swinging doors)
struct CollisionBox { float min[3], max[3]; };

// A swinging door: its own batches in object space, drawn with the door's transform
struct DoorMesh
{
	int ref;
	std::vector<CellBatch> batches;
};

struct Cell
{
	u32 flags = 0;
	float spawn[3], yaw, pitch;
	u8 ambient[4], fog[4];
	float fogDensity;
	float fogStart = 0, fogEnd = 0;   // linear distance fog; fogEnd 0 = none (interiors)
	float waterZ = 0;                 // with CELL_WATER
	float bounds[4] = {};             // walkable area: min x, min y, max x, max y (all 0 = unbounded)
	std::vector<CellBatch> sky;       // drawn around the camera, before everything else
	std::vector<C3D_Tex*> textures;       // from the TextureCache; nullptr = missing
	std::vector<std::string> textureNames;
	std::vector<CellBatch> batches;
	CollisionMesh collision;
	std::vector<CellActor> actors;
	std::vector<DoorMesh> doors;
	std::vector<CollisionBox> blockers;

	bool hasWater() const { return flags & CELL_WATER; }
	bool bounded() const { return bounds[2] > bounds[0]; }
	u32 textureBytes = 0, geometryBytes = 0, numTris = 0;
};

// Loads <dataDir>/cells/<cellFile> and the textures it references.
bool cellLoad(Cell& cell, const char* dataDir, const char* cellFile, TextureCache& cache);
void cellFree(Cell& cell, TextureCache& cache);
