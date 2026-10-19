// Distant land: see mwrender/objectpaging.hpp and tools/convert/distant.py (the file layout).
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <cstdlib>
#include <cstring>

#include "platform/console.hpp"
#include "mwrender/objectpaging.hpp"
#include "platform/linear.hpp"
#include "components/debug/debuglog.hpp"
#include "components/files/zfile.hpp"

namespace
{
struct __attribute__((packed)) Sample { s16 h; u8 r, g, b; };
struct __attribute__((packed)) Vertex { s16 x, y, z, w; s16 u, v; u8 r, g, b, a; };

const float kCell = 8192.0f;
const float kSkirt = 400.0f;      // edges hang down this far: no cracks against the loaded cells' finer land

std::vector<Sample> s_grid;
int s_x0 = 0, s_y0 = 0, s_nx = 0, s_ny = 0, s_per = 0, s_cols = 0;
u16* s_indices = nullptr;         // shared by every cell (same layout)
u32 s_numIndices = 0;
std::vector<DistantCell> s_cells;
bool s_rescan = true;             // distantUpdate looks for cells to build (moved, data changed, or some still owed)
int s_lastGx = 0, s_lastGy = 0, s_lastRadius = -1;
// Big objects per cell: where each cell's mesh starts in s_statics (the file's STAT section). A distant.bin
// stored as it is (not zlib'd: the converter's current output) stays on the card instead, read a cell at a time
// from s_file at that offset: the section is ~9 MB, a quarter of an Old 3DS's heap
struct __attribute__((packed)) StaticVertex { s16 x, y, z; u8 r, g, b, pad; };
std::vector<u8> s_statics;
std::unordered_map<long long, size_t> s_staticAt;
FILE* s_file = nullptr;
std::vector<u8> s_read;           // one cell's objects, read from s_file
long long cellKey(int gx, int gy) { return ((long long)gx << 32) ^ (u32)gy; }

void buildStatics(int gx, int gy, CellBatch& b)
{
	b = CellBatch();
	b.numIndices = 0;
	auto it = s_staticAt.find(cellKey(gx, gy));
	if (it == s_staticAt.end())
		return;
	const u8* p;
	u32 nv, ni;
	if (s_file)
	{
		u8 head[16];
		if (fseek(s_file, (long)it->second, SEEK_SET) != 0 || fread(head, 1, 16, s_file) != 16)
			return;
		memcpy(&nv, head + 8, 4);
		memcpy(&ni, head + 12, 4);
		size_t len = 16 + nv * sizeof(StaticVertex) + ni * sizeof(u16);
		s_read.resize(len);
		memcpy(s_read.data(), head, 16);
		if (fread(s_read.data() + 16, 1, len - 16, s_file) != len - 16)
			return;
		p = s_read.data();
	}
	else
	{
		p = s_statics.data() + it->second;
		memcpy(&nv, p + 8, 4);
		memcpy(&ni, p + 12, 4);
	}
	const StaticVertex* sv = (const StaticVertex*)(p + 16);
	const u8* ip = p + 16 + nv * sizeof(StaticVertex);
	Vertex* v = (Vertex*)lockedLinearAlloc(nv * sizeof(Vertex));
	u16* idx = (u16*)lockedLinearAlloc(ni * sizeof(u16));
	if (!v || !idx)
	{
		if (v) lockedLinearFree(v);
		if (idx) lockedLinearFree(idx);
		return;
	}
	float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
	for (u32 k = 0; k < nv; k++)
	{
		StaticVertex s;
		memcpy(&s, sv + k, sizeof(s));
		v[k] = { s.x, s.y, s.z, 0, 0, 0, s.r, s.g, s.b, 255 };
		float wp[3] = { gx * kCell + s.x * 0.5f, gy * kCell + s.y * 0.5f, (float)s.z };
		for (int a = 0; a < 3; a++)
		{
			lo[a] = fminf(lo[a], wp[a]);
			hi[a] = fmaxf(hi[a], wp[a]);
		}
	}
	memcpy(idx, ip, ni * sizeof(u16));
	GSPGPU_FlushDataCache(v, nv * sizeof(Vertex));
	GSPGPU_FlushDataCache(idx, ni * sizeof(u16));
	b.tex = -1;
	b.flags = BATCH_TWO_SIDED;
	b.numVerts = nv;
	b.numIndices = ni;
	b.verts = v;
	b.indices = idx;
	memcpy(b.bmin, lo, sizeof(lo));
	memcpy(b.bmax, hi, sizeof(hi));
	b.posOffset[0] = gx * kCell; b.posOffset[1] = gy * kCell; b.posOffset[2] = 0.0f;
	b.posScale[0] = b.posScale[1] = 0.5f; b.posScale[2] = 1.0f;
	b.stride = CELL_VERTEX_SIZE_PACKED;
}

void freeCell(DistantCell& c)
{
	lockedLinearFree(c.batch.verts);
	if (c.statics.numIndices)
	{
		lockedLinearFree(c.statics.verts);
		lockedLinearFree(c.statics.indices);
	}
}

void buildIndices()
{
	int n = s_per + 1;
	std::vector<u16> idx;
	for (int j = 0; j < s_per; j++)
		for (int i = 0; i < s_per; i++)
		{
			u16 a = j * n + i;
			u16 q[6] = { a, (u16)(a + 1), (u16)(a + n + 1), a, (u16)(a + n + 1), (u16)(a + n) };
			idx.insert(idx.end(), q, q + 6);
		}
	// the skirt: the edge ring, then the same ring lowered
	int ring = 4 * s_per, base = n * n;
	std::vector<u16> edge;
	for (int i = 0; i < s_per; i++) edge.push_back(i);                           // bottom, left to right
	for (int j = 0; j < s_per; j++) edge.push_back(j * n + s_per);               // right, upwards
	for (int i = s_per; i > 0; i--) edge.push_back(s_per * n + i);               // top, right to left
	for (int j = s_per; j > 0; j--) edge.push_back(j * n);                       // left, downwards
	for (int k = 0; k < ring; k++)
	{
		int k1 = (k + 1) % ring;
		u16 q[6] = { edge[k], edge[k1], (u16)(base + k1), edge[k], (u16)(base + k1), (u16)(base + k) };
		idx.insert(idx.end(), q, q + 6);
	}
	s_numIndices = idx.size();
	s_indices = (u16*)lockedLinearAlloc(idx.size() * sizeof(u16));
	if (s_indices)
		memcpy(s_indices, idx.data(), idx.size() * sizeof(u16));
}

bool buildCell(int gx, int gy, DistantCell& out)
{
	int cx = gx - s_x0, cy = gy - s_y0;
	if (cx < 0 || cy < 0 || cx >= s_nx || cy >= s_ny || !s_indices)
		return false;
	int n = s_per + 1, ring = 4 * s_per;
	Vertex* v = (Vertex*)lockedLinearAlloc((n * n + ring) * sizeof(Vertex));
	if (!v)
		return false;
	float step = kCell / s_per;
	float lo = 1e9f, hi = -1e9f;
	for (int j = 0; j < n; j++)
		for (int i = 0; i < n; i++)
		{
			const Sample& s = s_grid[(cy * s_per + j) * s_cols + cx * s_per + i];
			Vertex& o = v[j * n + i];
			o = { (s16)(i * step * 2.0f), (s16)(j * step * 2.0f), s.h, 0, 0, 0, s.r, s.g, s.b, 255 };
			lo = fminf(lo, s.h);
			hi = fmaxf(hi, s.h);
		}
	int k = 0;
	auto lower = [&](int at) { Vertex o = v[at]; o.z = (s16)fmaxf(-32768.0f, o.z - kSkirt); v[n * n + k++] = o; };
	for (int i = 0; i < s_per; i++) lower(i);
	for (int j = 0; j < s_per; j++) lower(j * n + s_per);
	for (int i = s_per; i > 0; i--) lower(s_per * n + i);
	for (int j = s_per; j > 0; j--) lower(j * n);
	GSPGPU_FlushDataCache(v, (n * n + ring) * sizeof(Vertex));

	CellBatch& b = out.batch;
	b = CellBatch();
	b.tex = -1;
	b.flags = BATCH_TWO_SIDED;
	b.alphaRef = 0;
	b.numVerts = n * n + ring;
	b.numIndices = s_numIndices;
	b.verts = v;
	b.indices = s_indices;
	b.bmin[0] = gx * kCell; b.bmin[1] = gy * kCell; b.bmin[2] = lo - kSkirt;
	b.bmax[0] = b.bmin[0] + kCell; b.bmax[1] = b.bmin[1] + kCell; b.bmax[2] = hi;
	b.maxDist = 0.0f;
	b.posOffset[0] = gx * kCell; b.posOffset[1] = gy * kCell; b.posOffset[2] = 0.0f;
	b.posScale[0] = b.posScale[1] = 0.5f; b.posScale[2] = 1.0f;
	b.stride = CELL_VERTEX_SIZE_PACKED;
	out.gx = gx;
	out.gy = gy;
	buildStatics(gx, gy, out.statics);
	return true;
}
}

static std::string s_loadedFrom;

// distant.bin stored plain (f just past "MWD1"): the grid into memory, the objects' places only
static bool distantOpen(FILE* f, const char* path)
{
	s32 hdr[2];
	u32 dims[3];
	bool ok = fread(hdr, 4, 2, f) == 2 && fread(dims, 4, 3, f) == 3;
	if (ok)
	{
		s_x0 = hdr[0]; s_y0 = hdr[1]; s_nx = dims[0]; s_ny = dims[1]; s_per = dims[2];
		s_cols = s_nx * s_per + 1;
		size_t count = (size_t)s_cols * (s_ny * s_per + 1);
		ok = s_per > 0 && s_per <= 16;
		if (ok)
		{
			s_grid.resize(count);
			ok = fread(s_grid.data(), sizeof(Sample), count, f) == count;
		}
		char tag[4];
		u32 cells = 0;
		if (ok && fread(tag, 1, 4, f) == 4 && memcmp(tag, "STAT", 4) == 0 && fread(&cells, 4, 1, f) == 1)
		{
			long off = ftell(f);
			for (u32 k = 0; k < cells; k++)
			{
				u8 head[16];
				if (fseek(f, off, SEEK_SET) != 0 || fread(head, 1, 16, f) != 16)
					break;
				s32 g[2];
				u32 nv, ni;
				memcpy(g, head, 8);
				memcpy(&nv, head + 8, 4);
				memcpy(&ni, head + 12, 4);
				s_staticAt[cellKey(g[0], g[1])] = off;
				off += 16 + nv * sizeof(StaticVertex) + ((ni * 2 + 3) & ~3u);
			}
		}
	}
	if (!ok)
	{
		fclose(f);
		logf("distant: bad %s", path);
		s_grid.clear();
		s_staticAt.clear();
		return false;
	}
	s_file = f;
	buildIndices();
	s_loadedFrom = path;
	logf("distant: %d x %d cells from %d %d, %d samples a side, big objects in %d cells (read from the card as needed)",
		s_nx, s_ny, s_x0, s_y0, s_per, (int)s_staticAt.size());
	return true;
}

// A zlib'd distant.bin (MWZ1, from a converter that packs it) unpacked once to the card, then read from there as the plain
// file is: reading it into memory needs ~20 MB at once (the unpacked file, then the objects copied out), which an Old
// 3DS's 41 MB heap can't give after the game data (used on a small heap only). The key file holds the packed size and
// zlib's closing Adler-32 of the unpacked data, so a new conversion unpacks again. Returns the copy just past "MWD1", or
// null.
static FILE* unpackedCopy(const char* path)
{
	const char* kCopy = "sdmc:/3ds/mw3ds/distant.bin";
	const char* kKey = "sdmc:/3ds/mw3ds/distant.key";
	const char* kTmp = "sdmc:/3ds/mw3ds/distant.tmp";
	FILE* raw = fopen(path, "rb");
	if (!raw)
		return nullptr;
	char head[8];
	u32 size = 0;
	u8 tail[4] = {};
	bool packed = fread(head, 1, 8, raw) == 8 && memcmp(head, "MWZ1", 4) == 0;
	long packedSize = 0;
	if (packed)
	{
		memcpy(&size, head + 4, 4);
		fseek(raw, 0, SEEK_END);
		packedSize = ftell(raw);
		packed = packedSize > 12 && fseek(raw, -4, SEEK_END) == 0 && fread(tail, 1, 4, raw) == 4;
	}
	fclose(raw);
	if (!packed)
		return nullptr;
	char key[64];
	snprintf(key, sizeof(key), "%ld %lu %02x%02x%02x%02x", packedSize, (unsigned long)size, tail[0], tail[1], tail[2], tail[3]);

	char had[64] = {};
	if (FILE* k = fopen(kKey, "rb"))
	{
		fread(had, 1, sizeof(had) - 1, k);
		fclose(k);
	}
	if (strcmp(had, key) != 0)
	{
		remove(kKey);
		FILE* in = zopen(path);
		FILE* out = in ? fopen(kTmp, "wb") : nullptr;
		static char buf[16 * 1024];
		u32 done = 0;
		while (in && out)
		{
			size_t got = fread(buf, 1, sizeof(buf), in);
			if (got == 0 || fwrite(buf, 1, got, out) != got)
				break;
			done += got;
		}
		bool ok = out && fclose(out) == 0 && done == size;
		if (in)
			zclose(in);
		if (!ok)
		{
			remove(kTmp);
			logf("distant: could not unpack %s to the card (%lu of %lu KB)", path, (unsigned long)done / 1024,
				(unsigned long)size / 1024);
			return nullptr;
		}
		remove(kCopy);
		if (rename(kTmp, kCopy) != 0)
			return nullptr;
		if (FILE* k = fopen(kKey, "wb"))
		{
			fputs(key, k);
			fclose(k);
		}
		logf("distant: unpacked %s to the card (%lu KB)", path, (unsigned long)size / 1024);
	}
	FILE* f = fopen(kCopy, "rb");
	char magic[4] = {};
	if (f && fread(magic, 1, 4, f) == 4 && memcmp(magic, "MWD1", 4) == 0)
		return f;
	if (f)
		fclose(f);
	remove(kKey);
	return nullptr;
}

bool distantLoad(const char* dataDir)
{
	char path[256];
	snprintf(path, sizeof(path), "%s/distant.bin", dataDir);
	// the same data again (a save loaded): keep it. Reading it needs ~20 MB at once (the file, then the
	// objects copied out), which a heap fragmented by a session or two can't give.
	if (s_loadedFrom == path && !s_grid.empty())
	{
		for (auto& c : s_cells)
			freeCell(c);
		s_cells.clear();
		s_rescan = true;
		logf("distant: kept from the last session");
		return true;
	}
	distantFree();
	if (FILE* f = fopen(path, "rb"))
	{
		char magic[4] = {};
		bool plain = fread(magic, 1, 4, f) == 4 && memcmp(magic, "MWD1", 4) == 0;
		if (plain)
			return distantOpen(f, path);
		fclose(f);
	}
	// (an Old 3DS: the heap is too small for it in memory; a New 3DS keeps it there, as it always has)
	if (FILE* f = g_profile.smallHeap ? unpackedCopy(path) : nullptr)
		return distantOpen(f, path);
	size_t size = 0;
	char* data = zreadAll(path, &size);
	if (!data)
	{
		logf("distant: no %s", path);
		return false;
	}
	bool ok = size >= 24 && memcmp(data, "MWD1", 4) == 0;
	if (ok)
	{
		s32 hdr[2];
		u32 dims[3];
		memcpy(hdr, data + 4, 8);
		memcpy(dims, data + 12, 12);
		s_x0 = hdr[0]; s_y0 = hdr[1]; s_nx = dims[0]; s_ny = dims[1]; s_per = dims[2];
		s_cols = s_nx * s_per + 1;
		size_t count = (size_t)s_cols * (s_ny * s_per + 1);
		ok = s_per > 0 && s_per <= 16 && size >= 24 + count * sizeof(Sample);
		if (ok)
		{
			s_grid.resize(count);
			memcpy(s_grid.data(), data + 24, count * sizeof(Sample));
			// Big objects (older files end with the grid)
			size_t at = 24 + count * sizeof(Sample);
			if (size >= at + 8 && memcmp(data + at, "STAT", 4) == 0)
			{
				u32 cells;
				memcpy(&cells, data + at + 4, 4);
				s_statics.assign((const u8*)data + at + 8, (const u8*)data + size);
				size_t off = 0;
				for (u32 k = 0; k < cells && off + 16 <= s_statics.size(); k++)
				{
					s32 g[2];
					u32 nv, ni;
					memcpy(g, &s_statics[off], 8);
					memcpy(&nv, &s_statics[off + 8], 4);
					memcpy(&ni, &s_statics[off + 12], 4);
					size_t len = 16 + nv * sizeof(StaticVertex) + ((ni * 2 + 3) & ~3u);
					if (off + len > s_statics.size())
						break;
					s_staticAt[cellKey(g[0], g[1])] = off;
					off += len;
				}
			}
		}
	}
	free(data);
	if (!ok)
	{
		logf("distant: bad %s", path);
		s_grid.clear();
		return false;
	}
	buildIndices();
	s_loadedFrom = path;
	logf("distant: %d x %d cells from %d %d, %d samples a side, big objects in %d cells (%u KB)", s_nx, s_ny,
		s_x0, s_y0, s_per, (int)s_staticAt.size(), (unsigned)(s_statics.size() / 1024));
	return true;
}

void distantFree()
{
	if (s_file)
		fclose(s_file);
	s_file = nullptr;
	s_read.clear();
	s_read.shrink_to_fit();
	s_loadedFrom.clear();
	for (auto& c : s_cells)
		freeCell(c);
	s_cells.clear();
	s_rescan = true;
	s_statics.clear();
	s_statics.shrink_to_fit();
	s_staticAt.clear();
	if (s_indices)
		lockedLinearFree(s_indices);
	s_indices = nullptr;
	s_grid.clear();
	s_grid.shrink_to_fit();
}

void distantUpdate(int gx, int gy, int radius)
{
	if (s_grid.empty())
		return;
	// free what is out of reach
	for (size_t i = 0; i < s_cells.size();)
		if (abs(s_cells[i].gx - gx) > radius + 1 || abs(s_cells[i].gy - gy) > radius + 1)
		{
			freeCell(s_cells[i]);
			s_cells[i] = s_cells.back();
			s_cells.pop_back();
		}
		else
			i++;
	// build what came into reach: a few a frame, and of those with objects on the card (an Old 3DS's distant.bin stays
	// there) one a frame, each a seek and a read inside the frame. Nothing to look for until the player moves to another
	// cell or something is still owed
	if (gx != s_lastGx || gy != s_lastGy || radius != s_lastRadius)
		s_rescan = true;
	if (!s_rescan)
		return;
	s_lastGx = gx, s_lastGy = gy, s_lastRadius = radius;
	int built = 0, reads = 0;
	bool owed = false;
	for (int dy = -radius; dy <= radius; dy++)
		for (int dx = -radius; dx <= radius; dx++)
		{
			int cx = gx + dx - s_x0, cy = gy + dy - s_y0;
			if (cx < 0 || cy < 0 || cx >= s_nx || cy >= s_ny)
				continue;
			bool have = false;
			for (auto& c : s_cells)
				have |= c.gx == gx + dx && c.gy == gy + dy;
			if (have)
				continue;
			bool read = s_file && s_staticAt.count(cellKey(gx + dx, gy + dy));
			DistantCell c;
			if (built >= 4 || (read && reads >= 1))
				owed = true;
			else if (buildCell(gx + dx, gy + dy, c))
			{
				s_cells.push_back(c);
				built++;
				reads += read;
			}
			else
				owed = true;               // (no linear memory for it now: tried again next frame)
		}
	s_rescan = owed;
}

u32 distantBytes()
{
	u32 b = s_numIndices * 2;
	for (auto& c : s_cells)
		b += c.batch.numVerts * sizeof(Vertex) + (c.statics.numIndices ? c.statics.numVerts * sizeof(Vertex) + c.statics.numIndices * 2 : 0);
	return b;
}

const std::vector<DistantCell>& distantCells()
{
	return s_cells;
}
