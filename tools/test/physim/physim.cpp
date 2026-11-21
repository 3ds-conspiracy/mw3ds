// Headless player physics on the PC: the game's player.cpp and collision.cpp against a cell's real
// collision, driven by random walks at hardware-like frame times, flagging where the player ends up
// outside the room (most horizontal rays escape) or drops through a floor.
//   python tools/test/physim/run.py <cell file stem> x y z [walks] [seconds]
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <map>
#include <random>
#include <tuple>

#include "mwworld/player.hpp"

extern const char* g_playerBlock;

static int g_playerReverts;

static bool readCel(const char* path, Cell& cell)
{
	FILE* f = fopen(path, "rb");
	if (!f)
		return false;
	char magic[4];
	u32 version, flags;
	fread(magic, 4, 1, f);
	fread(&version, 4, 1, f);
	fread(&flags, 4, 1, f);
	if (memcmp(magic, "MWC1", 4) || version < 6 || version > 10)
		return false;
	cell.flags = flags;
	// CellHeader's tail (spawn, yaw, pitch, ambient, fog, fogDensity), then CellHeaderV4's bounds
	fseek(f, 5 * 4 + 8 + 4, SEEK_CUR);
	fseek(f, 3 * 4, SEEK_CUR);
	fread(cell.bounds, 4, 4, f);
	u32 n;
	fread(&n, 4, 1, f);
	fseek(f, 64 * n, SEEK_CUR);
	// Batches, skipped: v4 adds bounds, v5 the quantization, v8 flip frames, v9 the scroll, v10 the lamp
	fread(&n, 4, 1, f);
	const int hdr = 16 + (version >= 4 ? 24 : 0) + (version >= 5 ? 32 + (version >= 8 ? 4 : 0)
		+ (version >= 9 ? 4 : 0) + (version >= 10 ? 4 : 0) : 0);
	for (u32 i = 0; i < n; i++)
	{
		u8 h[16];
		fread(h, 16, 1, f);
		u32 nv, ni;
		memcpy(&nv, h + 8, 4);
		memcpy(&ni, h + 12, 4);
		fseek(f, hdr - 16, SEEK_CUR);
		fseek(f, nv * 16 + ni * 2 + ((ni & 1) ? 2 : 0), SEEK_CUR);
	}
	if (!collisionRead(cell.collision, f, version))
		return false;
	fread(&n, 4, 1, f);
	cell.actors.resize(n);
	fread(cell.actors.data(), sizeof(CellActor), n, f);
	fclose(f);
	return true;
}

// Share of 16 horizontal rays from the chest that hit nothing within 4000 units
static int escapes(Cell& cell, const float feet[3])
{
	int out = 0;
	for (int k = 0; k < 16; k++)
	{
		// Out that way when rays at three heights all miss (a seam between wall pieces lets one through)
		bool hit = false;
		for (float h : { 50.0f, 70.0f, 90.0f })
		{
			float a = k * 3.14159265f / 8, from[3] = { feet[0], feet[1], feet[2] + h };
			float to[3] = { from[0] + cosf(a) * 4000, from[1] + sinf(a) * 4000, from[2] };
			float t;
			hit |= collisionRaycast(cell.collision, from, to, &t);
		}
		out += !hit;
	}
	return out;
}

int main(int argc, char** argv)
{
	if (argc < 5)
	{
		fprintf(stderr, "physim cell.raw x y z [walks] [seconds] [seed]\n");
		return 2;
	}
	Cell cell;
	if (!readCel(argv[1], cell))
	{
		fprintf(stderr, "cannot read %s\n", argv[1]);
		return 1;
	}
	{
		// The decoded collision mesh next to the cell (<raw>.col) for slice.py: counts, xyz floats, indices
		std::string col = std::string(argv[1]) + ".col";
		if (FILE* f = fopen(col.c_str(), "wb"))
		{
			uint32_t nv = cell.collision.verts.size() / 3, nt = cell.collision.tris.size() / 3;
			fwrite(&nv, 4, 1, f);
			fwrite(&nt, 4, 1, f);
			fwrite(cell.collision.verts.data(), 4, nv * 3, f);
			for (uint32_t i = 0; i < nt * 3; i++)
			{
				uint32_t v = cell.collision.tris[i];
				fwrite(&v, 4, 1, f);
			}
			fclose(f);
		}
	}
	float start[3] = { (float)atof(argv[2]), (float)atof(argv[3]), (float)atof(argv[4]) };
	if (argc > 5 && strcmp(argv[5], "brute") == 0)
	{
		// Every triangle tested by hand (no grid): what the nearest hit really is, with the triangle it is,
		// so a raycast through the grid can be checked against the same triangles one by one
		auto sub3 = [](const float* x, const float* y, float* out) {
			out[0] = x[0] - y[0]; out[1] = x[1] - y[1]; out[2] = x[2] - y[2];
		};
		auto dot3 = [](const float* x, const float* y) { return x[0] * y[0] + x[1] * y[1] + x[2] * y[2]; };
		float deg = argc > 9 ? (float)atof(argv[9]) : 180.0f;
		float a = deg * 3.14159265f / 180.0f;
		float from[3] = { start[0], start[1], start[2] + 70 };
		float d[3] = { cosf(a) * 4000, sinf(a) * 4000, 0 };
		const CollisionMesh& m = cell.collision;
		float best = 2.0f;
		size_t which = 0;
		for (size_t i = 0; i + 2 < m.tris.size(); i += 3)
		{
			const float* p0 = &m.verts[m.tris[i] * 3];
			const float* p1 = &m.verts[m.tris[i + 1] * 3];
			const float* p2 = &m.verts[m.tris[i + 2] * 3];
			float e1[3], e2[3], pv[3], tv[3], qv[3];
			sub3(p1, p0, e1);
			sub3(p2, p0, e2);
			pv[0] = d[1] * e2[2] - d[2] * e2[1]; pv[1] = d[2] * e2[0] - d[0] * e2[2]; pv[2] = d[0] * e2[1] - d[1] * e2[0];
			float det = dot3(e1, pv);
			if (fabsf(det) < 1e-6f)
				continue;
			float inv = 1.0f / det;
			sub3(from, p0, tv);
			float u = dot3(tv, pv) * inv;
			if (u < -1e-3f || u > 1.001f)
				continue;
			qv[0] = tv[1] * e1[2] - tv[2] * e1[1]; qv[1] = tv[2] * e1[0] - tv[0] * e1[2]; qv[2] = tv[0] * e1[1] - tv[1] * e1[0];
			float v = dot3(d, qv) * inv;
			if (v < -1e-3f || u + v > 1.001f)
				continue;
			float tt = dot3(e2, qv) * inv;
			if (tt >= 0.0f && tt <= 1.0f && tt < best)
				best = tt, which = i / 3;
		}
		if (best > 1.0f)
			printf("brute over %zu tris at %.0f deg: nothing within 4000\n", m.tris.size() / 3, deg);
		else
		{
			printf("brute over %zu tris at %.0f deg: hit at %.0f units, triangle %zu\n", m.tris.size() / 3, deg,
				best * 4000, which);
			for (int k = 0; k < 3; k++)
			{
				const float* p = &m.verts[m.tris[which * 3 + k] * 3];
				printf("   corner %d: %.3f %.3f %.3f (vertex %u)\n", k, p[0], p[1], p[2], m.tris[which * 3 + k]);
			}
		}
		return 0;
	}
	if (argc > 5 && strcmp(argv[5], "dump") == 0)
	{
		// Every collision triangle whose box overlaps the box around the spot: what the engine really has
		// there, to set beside what a decoder of the same file reads
		float r = argc > 9 ? (float)atof(argv[9]) : 80.0f;
		const CollisionMesh& m = cell.collision;
		for (size_t i = 0; i + 2 < m.tris.size(); i += 3)
		{
			float v[3][3];
			for (int k = 0; k < 3; k++)
				memcpy(v[k], &m.verts[m.tris[i + k] * 3], 3 * sizeof(float));
			float lo[3] = { v[0][0], v[0][1], v[0][2] }, hi[3] = { v[0][0], v[0][1], v[0][2] };
			for (int k = 1; k < 3; k++)
				for (int d = 0; d < 3; d++)
				{
					lo[d] = fminf(lo[d], v[k][d]);
					hi[d] = fmaxf(hi[d], v[k][d]);
				}
			bool in = true;
			for (int d = 0; d < 3; d++)
				in &= lo[d] <= start[d] + r && hi[d] >= start[d] - r;
			if (!in)
				continue;
			printf("tri %zu %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f\n", i / 3, v[0][0], v[0][1], v[0][2],
				v[1][0], v[1][1], v[1][2], v[2][0], v[2][1], v[2][2]);
		}
		printf("grid origin %.0f %.0f, %u x %u cells of %.0f, %zu tris, %zu verts\n", m.originX, m.originY,
			m.nx, m.ny, m.cellSize, m.tris.size() / 3, m.verts.size() / 3);
		return 0;
	}
	if (argc > 5 && strcmp(argv[5], "rays") == 0)
	{
		// The escape test's 16 rays from that spot (feet): where each hits
		for (int k = 0; k < 16; k++)
		{
			float a = k * 3.14159265f / 8, from[3] = { start[0], start[1], start[2] + 70 };
			float to[3] = { from[0] + cosf(a) * 4000, from[1] + sinf(a) * 4000, from[2] };
			float t;
			if (collisionRaycast(cell.collision, from, to, &t))
				printf("ray %2d hits at %.0f\n", k, t * 4000);
			else
				printf("ray %2d escapes\n", k);
		}
		printf("grid origin %.0f %.0f, %u x %u cells of %.0f\n", cell.collision.originX, cell.collision.originY,
			cell.collision.nx, cell.collision.ny, cell.collision.cellSize);
		return 0;
	}
	int walks = argc > 5 ? atoi(argv[5]) : 200;
	float seconds = argc > 6 ? atof(argv[6]) : 60;
	unsigned seed = argc > 7 ? atoi(argv[7]) : 1;
	Scene scene;
	scene.cells.push_back(&cell);
	scene.here = &cell;
	printf("collision %zu tris, %zu actors\n", cell.collision.tris.size() / 3, cell.actors.size());
	// Below every collision triangle: fallen out of the world (caves sit well below 0)
	float floorZ = 1e9f;
	for (size_t v = 2; v < cell.collision.verts.size(); v += 3)
		floorZ = fminf(floorZ, cell.collision.verts[v]);
	floorZ -= 256.0f;
	{
		// Where a door drops the player: a floor must be under it (else they fall out of the world)
		Player p = {};
		float eye[3] = { start[0], start[1], start[2] + PLAYER_EYE_HEIGHT };
		playerSpawn(p, scene, eye, 0, 0);
		if (p.onGround)
			printf("spawn: floor at %.0f (%.0f below the destination)\n", p.feet[2], start[2] - p.feet[2]);
		else
			printf("spawn: no floor under the destination\n");
	}
	if (walks < 0)
	{
		// Scripted: walk straight at yaw (degrees, 0 = +Y) for `seconds`, printing the path
		Player p = {};
		float eye[3] = { start[0], start[1], start[2] + PLAYER_EYE_HEIGHT };
		playerSpawn(p, scene, eye, seconds * 0, 0);
		p.yaw = atof(argv[7]) * 3.14159265f / 180.0f;
		// PHYSIM_DT: the frame time (the game's fixed test step is 1/30 s)
		float fdt = getenv("PHYSIM_DT") ? (float)atof(getenv("PHYSIM_DT")) : 1.0f / 60.0f;
		int frames = (int)(3.0f / fdt);
		for (int f = 0; f < frames; f++)
		{
			PlayerInput in = {};
			in.moveY = 1.0f;
			playerUpdate(p, scene, in, fdt);
			if (f % (int)(10 * (1.0f / 60.0f) / fdt + 0.5f > 0 ? (int)((1.0f / 6.0f) / fdt + 0.5f) : 1) == 0)
				printf("  %.0f %.0f %.0f %s block %s\n", p.feet[0], p.feet[1], p.feet[2], p.onGround ? "ground" : "air",
					g_playerBlock);
		}
		return 0;
	}
	int bad = 0, upstairs = 0;
	std::map<std::tuple<int, int, int>, int> frozen;   // frames without any movement, by 50-unit spot
	double moved = 0, time = 0;
	for (int wlk = 0; wlk < walks; wlk++)
	{
		std::mt19937 rng(seed * 7919 + wlk);
		std::uniform_real_distribution<float> U(0, 1);
		Player p = {};
		float eye[3] = { start[0], start[1], start[2] + PLAYER_EYE_HEIGHT };
		playerSpawn(p, scene, eye, U(rng) * 6.283f, 0);
		int baseEsc = escapes(cell, p.feet);
		float t = 0, turnIn = 0, turn = 0, jumpIn = 1 + U(rng) * 5;
		float hist[64][4];
		float trace[64][6] = {};
		int tn = 0;
		int hn = 0;
		bool reported = false;
		float maxZ = p.feet[2];
		while (t < seconds && !reported)
		{
			float dt = getenv("PHYSIM_SLOW") ? 1.0f / (4 + U(rng) * 20) : 1.0f / (25 + U(rng) * 35);
			PlayerInput in = {};
			in.moveY = 1.0f;
			in.moveX = (U(rng) - 0.5f) * 0.4f;
			if ((turnIn -= dt) <= 0)
			{
				turnIn = 0.3f + U(rng) * 2.0f;
				turn = (U(rng) - 0.5f) * 2.0f;
			}
			in.lookX = turn;
			if ((jumpIn -= dt) <= 0 && !getenv("PHYSIM_NOJUMP"))
			{
				in.jump = true;
				jumpIn = 1 + U(rng) * 6;
			}
			float before[3] = { p.feet[0], p.feet[1], p.feet[2] };
			playerUpdate(p, scene, in, dt);
			t += dt;
			if (p.landedFall > 0.0f)
			{
				// a landing: the drop a walk must not make (fall damage starts at 400)
				if (p.landedFall > 150.0f)
					printf("fall %.0f: from %.0f %.0f %.0f to %.0f %.0f %.0f (walk %d, t=%.2f, dt=%.3f)\n", p.landedFall, before[0], before[1],
						before[2], p.feet[0], p.feet[1], p.feet[2], wlk, t, dt);
				if (getenv("PHYSIM_TRACE") && p.landedFall > 150.0f)
					for (int i = 0; i < 60; i++)
					{
						float* h = trace[(tn + 64 - 60 + i) % 64];
						printf("   %.1f %.1f %.2f ground %.0f top %.2f vz %.0f\n", h[0], h[1], h[2], h[3], h[4], h[5]);
					}
				p.landedFall = 0.0f;
			}
			{
				float* h = trace[tn++ % 64];
				h[0] = p.feet[0]; h[1] = p.feet[1]; h[2] = p.feet[2]; h[3] = p.onGround; h[4] = p.fallTop; h[5] = p.vz;
			}
			moved += sqrtf((p.feet[0] - before[0]) * (p.feet[0] - before[0]) + (p.feet[1] - before[1]) * (p.feet[1] - before[1]));
			static int lastReverts = 0;
			bool reverted = g_playerReverts != lastReverts;
			lastReverts = g_playerReverts;
			static int stuckRun = 0, shown = 0;
			if (p.feet[0] == before[0] && p.feet[1] == before[1] && p.feet[2] == before[2] && reverted)
			{
				frozen[{ (int)floorf(p.feet[0] / 50), (int)floorf(p.feet[1] / 50), (int)floorf(p.feet[2] / 50) }]++;
				if (++stuckRun == 200 && shown++ < 3)
					printf("stuck: walk %d feet %.1f %.1f %.1f vz %.1f onGround %d yaw %.2f\n", wlk, p.feet[0], p.feet[1],
						p.feet[2], p.vz, p.onGround, p.yaw);
			}
			else
				stuckRun = 0;
			maxZ = fmaxf(maxZ, p.onGround ? p.feet[2] : maxZ);
			float* h = hist[hn++ % 64];
			h[0] = p.feet[0]; h[1] = p.feet[1]; h[2] = p.feet[2]; h[3] = p.yaw;
			int esc = escapes(cell, p.feet);
			const char* why = nullptr;
			if (esc >= 8 && esc > baseEsc + 4)
				why = "outside";
			else if (p.feet[2] < floorZ)
				why = "fell out of the world";
			if (why)
			{
				bad++;
				reported = true;
				printf("walk %d: %s at t=%.2f feet %.0f %.0f %.0f (was %.0f %.0f %.0f) escapes %d/16 onGround %d\n", wlk, why, t,
					p.feet[0], p.feet[1], p.feet[2], before[0], before[1], before[2], esc, p.onGround);
				int from = hn > 64 ? hn - 64 : 0;
				for (int i = hn - 12 > from ? hn - 12 : from; i < hn; i++)
					printf("   %.0f %.0f %.0f yaw %.2f\n", hist[i % 64][0], hist[i % 64][1], hist[i % 64][2], hist[i % 64][3]);
			}
		}
		time += t;
		upstairs += maxZ > start[2] + 150;
	}
	printf("%d reverts\n", g_playerReverts);
	printf("%d of %d walks escaped; %d went 150+ units up; average speed %.0f units/s\n", bad, walks, upstairs,
		moved / time);
	std::vector<std::pair<int, std::tuple<int, int, int>>> top;
	for (auto& f : frozen)
		top.push_back({ f.second, f.first });
	std::sort(top.rbegin(), top.rend());
	for (size_t i = 0; i < top.size() && i < 8; i++)
		printf("  frozen %d frames near %d %d %d\n", top[i].first, std::get<0>(top[i].second) * 50,
			std::get<1>(top[i].second) * 50, std::get<2>(top[i].second) * 50);
	return bad ? 3 : 0;
}

// The parts of the engine this harness never runs, defined so the game's own physics files link without
// pulling in their .cpp's: the cell arena, the profile counters, the combat and world calls the movement
// solver only reaches through code paths a walk never takes.
#include "components/debug/debuglog.hpp"
#include "components/debug/profile.hpp"
#include "platform/cellarena.hpp"
#include "platform/memstat.hpp"
#include "mwworld/worldimp.hpp"
#include "mwbase/session.hpp"
#include "mwmechanics/combathelpers.hpp"

int g_profCalls[PCALL_COUNT];
unsigned g_pickSerial;
volatile bool g_heapProbeFailed = false;

bool cadenceOn(int) { return false; }
void logf(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); fputc('\n', stdout); }

// malloc under the arena's name: the Cell's block comes from and goes back to the same place
bool cellScopeOpen() { return false; }
void* cellArenaAlloc(size_t bytes) { return malloc(bytes); }
bool cellArenaRelease(void* p) { free(p); return true; }
bool cellArenaFits(size_t) { return true; }

float zMoverTop(World&, const Ref&, const Cell&) { return 0.0f; }
float trailHeightNear(float, float) { return 0.0f; }

float Session::fatigueTermOf(float, float) { return 0.0f; }
float Session::fallDamage(float, int, float) { return 0.0f; }
void Session::killNpc(int, bool) {}
void Session::knockDown(int, bool) {}
void Session::npcOpenDoor(int, int) {}

int World::gridCell(int, int) const { return -1; }
bool World::underWater(float) const { return false; }
float World::actorBoxLift(int) { return 0.0f; }
Actor* World::actorOf(int) { return nullptr; }
void World::syncActor(int) {}
int World::actorSkill(int, int) const { return 0; }
float World::distanceToPlayer(int) const { return 0.0f; }
