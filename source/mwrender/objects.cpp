#include "mwrender/objects.hpp"
#include <sys/stat.h>
#include "components/misc/planes.hpp"
#include "components/files/datapath.hpp"

#include <cstdio>
#include <cstring>
#include <tex3ds.h>

#include "platform/console.hpp"
#include "platform/linear.hpp"
#include "components/debug/debuglog.hpp"
#include "platform/memstat.hpp"
#include "components/files/zfile.hpp"

struct __attribute__((packed)) CellHeader
{
	char magic[4];
	u32 version, flags;
	float spawn[3], yaw, pitch;
	u8 ambient[4], fog[4];
	float fogDensity;
};

struct __attribute__((packed)) CellHeaderV4
{
	float fogStart, fogEnd, waterZ;
	float bounds[4];
};

struct __attribute__((packed)) BatchHeader
{
	s32 tex;
	u8 flags, alphaRef;
	u16 pad;              // v4: maximum draw distance / 8
	u32 numVerts, numIndices;
};

bool texImportFile(C3D_Tex* tex, const char* path)
{
	MarkScope mark("texture read");
	FILE* f = fopen(path, "rb");
	if (!f)
		return false;
	Tex3DS_Texture t3x = Tex3DS_TextureImportStdio(f, tex, nullptr, false);
	fclose(f);
	if (!t3x)
		return false;
	Tex3DS_TextureFree(t3x);
	return true;
}

static bool loadTexture(C3D_Tex* tex, const char* path)
{
	if (!texImportFile(tex, path))
		return false;
	C3D_TexSetFilter(tex, GPU_LINEAR, GPU_LINEAR);
	C3D_TexSetFilterMipmap(tex, GPU_LINEAR);
	if (!g_profile.textureMips)
		C3D_TexSetLodBias(tex, -16.0f);      // the full-size picture at every distance (the lowest bias there is)
	return true;
}

// A blended batch's triangles whose three corners are fully see-through add nothing to the picture but cost the GPU
// their fill: the sky dome's rings below the horizon (its share of the sky's colour is 0 there) cover half the screen.
// They are left out of the index list (the colour's alpha is a vertex's last byte)
static void trimClearTriangles(CellBatch& b)
{
	if (!(b.flags & BATCH_BLEND) || !b.verts || !b.indices)
		return;
	const u8* v = (const u8*)b.verts;
	auto clear = [&](u16 i) { return i >= b.numVerts || v[i * b.stride + b.stride - 1] == 0; };
	u32 kept = 0;
	for (u32 t = 0; t + 2 < b.numIndices; t += 3)
	{
		if (clear(b.indices[t]) && clear(b.indices[t + 1]) && clear(b.indices[t + 2]))
			continue;
		for (int k = 0; k < 3; k++)
			b.indices[kept + k] = b.indices[t + k];
		kept += 3;
	}
	if (kept == b.numIndices)
		return;
	b.numIndices = kept;
	GSPGPU_FlushDataCache(b.indices, kept * 2);
}

static bool readBatches(FILE* f, std::vector<CellBatch>& out, Cell& cell, u32 version)
{
	u32 count = 0;
	if (fread(&count, 4, 1, f) != 1)
		return false;
	// (a big interior's are one 160 KB block: if the heap has none, the load fails and waits, rather than `new` stopping the game)
	if (!tryReserve(out, out.size() + count))
		return false;
	for (u32 i = 0; i < count; i++)
	{
		BatchHeader bh;
		if (fread(&bh, sizeof(bh), 1, f) != 1)
		{
			logf("cell: truncated at batch %lu", i);
			return false;
		}
		CellBatch b = { bh.tex, bh.flags, bh.alphaRef, bh.numVerts, bh.numIndices, nullptr, nullptr, {}, {},
			version >= 4 ? bh.pad * 8.0f : 0.0f };
		if (version >= 4)
		{
			fread(b.bmin, 4, 3, f);
			fread(b.bmax, 4, 3, f);
		}
		if (version >= 5)
		{
			// Quantized vertices: pos = offset + s16 * scale, uv = s16 * uvScale
			fread(b.posOffset, 4, 3, f);
			fread(b.posScale, 4, 3, f);
			fread(b.uvScale, 4, 2, f);
			b.stride = CELL_VERTEX_SIZE_PACKED;
			if (version >= 8)
			{
				fread(&b.flipFrames, 1, 1, f);
				fread(&b.flipCols, 1, 1, f);
				fread(&b.flipRows, 1, 1, f);
				fread(&b.flipFps, 1, 1, f);
			}
			if (version >= 9)
			{
				s16 su = 0, sv = 0;
				fread(&su, 2, 1, f);
				fread(&sv, 2, 1, f);
				b.scrollU = su / 256.0f;
				b.scrollV = sv / 256.0f;
			}
			if (version >= 10)
			{
				// The lamp light's colour; the vertices' w is its brightness (convert_cell.py lamp_channel)
				u8 lamp[4] = { 0, 0, 0, 0 };
				fread(lamp, 1, 4, f);
				for (int k = 0; k < 3; k++)
					b.lamp[k] = lamp[k] / 255.0f;
				b.side = lamp[3] != 0;     // (the colours' alpha is the side light's facing: convert_cell.py side_facing)
			}
		}
		else
		{
			// Older files have no bounds: never culled
			b.bmin[0] = b.bmin[1] = b.bmin[2] = -1e9f;
			b.bmax[0] = b.bmax[1] = b.bmax[2] = 1e9f;
		}
		u32 vbytes = bh.numVerts * b.stride, ibytes = bh.numIndices * 2;
		if (bh.numIndices < 3 || bh.numVerts == 0)
		{
			// Nothing to draw (a draw of no triangles would hang the GPU): kept without buffers, so
			// references' batch numbers still match, and never drawn
			fseek(f, vbytes + ibytes + ((bh.numIndices & 1) ? 2 : 0), SEEK_CUR);
			logf("cell: empty batch %lu", i);
			b.numIndices = 0;
			out.push_back(b);
			continue;
		}
		b.verts = lockedLinearAlloc(vbytes);
		b.indices = (u16*)lockedLinearAlloc(ibytes);
		if (!b.verts || !b.indices)
		{
			logf("cell: out of linear memory at batch %lu (%lu KB free)", i, linearSpaceFree() / 1024);
			lockedLinearFree(b.verts);
			lockedLinearFree(b.indices);
			return false;
		}
		fread(b.verts, vbytes, 1, f);
		fread(b.indices, ibytes, 1, f);
		if (version >= 7)
		{
			// Planes, positions and indices as differences (components/misc/planes.hpp)
			unplane((u8*)b.verts, bh.numVerts, b.stride);
			for (int k = 0; k < 3; k++)
				undelta16((u8*)b.verts, bh.numVerts, b.stride, k * 2);
			unpackInts((u8*)b.indices, bh.numIndices, 2);
		}
		// The CPU wrote them through its data cache: the GPU reads memory, so write it back first
		// (without this a real 3DS draws from stale memory and can hang; emulators have no cache)
		GSPGPU_FlushDataCache(b.verts, vbytes);
		GSPGPU_FlushDataCache(b.indices, ibytes);
		if (bh.numIndices & 1)
			fseek(f, 2, SEEK_CUR);
		cell.geometryBytes += vbytes + ibytes;
		cell.numTris += bh.numIndices / 3;
		out.push_back(b);
	}
	return true;
}

volatile bool g_streamCancel = false;

// Holds a TextureCache's own lock for a scope (the map and the entries are shared between the
// main and the streaming thread). Taken before any linear-heap lock, never after one.
struct CacheGuard
{
	LightLock* l;
	CacheGuard(LightLock* lock) : l(lock) { LightLock_Lock(l); }
	~CacheGuard() { LightLock_Unlock(l); }
};

// A failed texture is tried again up to 3 times: the first failure is often linear memory full at that
// moment, and a cached failure kept the weapon or hair white for good.
// Called with the cache's lock held; the SD read runs without it (the main thread takes it for
// first-person pieces, the sky and the retries, and waited out the streaming thread's reads). The
// texture loads into a copy that goes in under the lock. The caller holds a reference, so the entry
// outlives the read; when both threads loaded it meanwhile, the second copy is dropped
static void tryLoad(TextureCache& c, TextureCache::Entry* e, const char* dataDir, const std::string& name)
{
	// (the quit waited for the streaming thread to read every texture of a cell it then threw away: up to 5 s on
	// hardware after the save, on a screen that looked frozen)
	if (g_streamCancel && threadGetCurrent())
		return;
	char path[256];
	snprintf(path, sizeof(path), "%s", shardedPath(dataDir, "tex", name).c_str());
	C3D_Tex tex = {};
	LightLock_Unlock(&c.lock);
	bool ok = loadTexture(&tex, path);
	LightLock_Lock(&c.lock);
	if (e->ok)
	{
		if (ok)
			C3D_TexDelete(&tex);      // never drawn
		return;
	}
	if (ok)
	{
		e->tex = tex;
		e->ok = true;
		c.bytes += C3D_TexCalcTotalSize(e->tex.size, e->tex.maxLevel);
	}
	else
	{
		// (no such file, the converter wrote none: not worth a read every few seconds while a cell holds it)
		struct stat st;
		e->missing = stat(path, &st) != 0;
		if (e->fails++ == 0)
			logf("cell: texture failed: %s%s", path, e->missing ? " (no such file)" : "");
	}
}

C3D_Tex* TextureCache::acquire(const char* dataDir, const std::string& name)
{
	CacheGuard guard(&lock);      // not the linear heap's lock: every allocation in the game waited on it
	Entry*& slot = entries[name];
	if (!slot)
		slot = new Entry();       // (zeroed: not loaded, no references, no failures)
	Entry* e = slot;
	// The reference first: the load lets go of the lock, and the other thread may release the name meanwhile
	e->refs++;
	if (!e->ok && !e->missing && e->fails < 3)
		tryLoad(*this, e, dataDir, name);
	return e->ok ? &e->tex : nullptr;
}

C3D_Tex* TextureCache::recheck(const char* dataDir, const std::string& name)
{
	CacheGuard guard(&lock);
	auto it = entries.find(name);
	if (it == entries.end())
		return nullptr;
	if (!it->second->ok && !it->second->missing && it->second->fails < 3)
		tryLoad(*this, it->second, dataDir, name);
	return it->second->ok ? &it->second->tex : nullptr;
}

C3D_Tex* TextureCache::retry(const char* dataDir, const std::string& name)
{
	static const u32 kRoom = 2 * 1024 * 1024;     // free linear memory to try with
	CacheGuard guard(&lock);
	auto it = entries.find(name);
	if (it == entries.end())
		return nullptr;
	if (!it->second->ok && !it->second->missing && (it->second->fails < 3 || linearSpaceFree() > kRoom))
		tryLoad(*this, it->second, dataDir, name);
	return it->second->ok ? &it->second->tex : nullptr;
}

void TextureCache::release(const std::string& name)
{
	CacheGuard guard(&lock);
	auto it = entries.find(name);
	if (it == entries.end() || --it->second->refs > 0)
		return;
	if (it->second->ok)
	{
		bytes -= C3D_TexCalcTotalSize(it->second->tex.size, it->second->tex.maxLevel);
		deferredTexDelete(&it->second->tex);
	}
	delete it->second;
	entries.erase(it);
}

bool cellLoad(Cell& cell, const char* dataDir, const char* cellFile, TextureCache& cache)
{
	std::string name = cellFile, ext;
	size_t dot = name.rfind('.');
	if (dot != std::string::npos)
	{
		ext = name.substr(dot);
		name.resize(dot);
	}
	std::string p = cellPath(dataDir, name, ext.c_str());
	const char* path = p.c_str();
	MarkScope mark("cell read");
	FILE* f = zopen(path);
	if (!f)
	{
		logf("cell: cannot open %s", path);
		return false;
	}

	CellHeader h;
	if (fread(&h, sizeof(h), 1, f) != 1 || memcmp(h.magic, "MWC1", 4) != 0 || h.version < 1 || h.version > 10)
	{
		logf("cell: bad header in %s", path);
		zclose(f);
		return false;
	}
	cell.flags = h.version >= 4 ? h.flags : CELL_INTERIOR;
	if (h.version >= 4)
	{
		CellHeaderV4 h4;
		fread(&h4, sizeof(h4), 1, f);
		cell.fogStart = h4.fogStart;
		cell.fogEnd = h4.fogEnd;
		// Outdoors the view reaches past the loaded cells now (distant land): the converter's
		// 1800 / 6000 was the loaded cells' edge. An interior under the sky (Mournhold) has the weather's fog over the
		// same view, as OpenMW's quasi-exteriors
		if (cell.fogEnd > 0.0f && (!(cell.flags & CELL_INTERIOR) || (cell.flags & CELL_SKY)))
		{
			cell.fogStart = kExteriorFogStart;
			cell.fogEnd = kExteriorFogEnd;
		}
		cell.waterZ = cell.waterZBaked = h4.waterZ;
		memcpy(cell.bounds, h4.bounds, sizeof(cell.bounds));
	}
	memcpy(cell.spawn, h.spawn, sizeof(cell.spawn));
	cell.yaw = h.yaw;
	cell.pitch = h.pitch;
	memcpy(cell.ambient, h.ambient, 4);
	memcpy(cell.fog, h.fog, 4);
	cell.fogDensity = h.fogDensity;
	// Indoors the cell's fog density sets how near its fog colour closes in (OpenMW: viewing distance x (1 - density)
	// to the viewing distance; density 0, none). Shurdan-Raplay's red haze and the shrines' green-grey come from it
	if ((cell.flags & CELL_INTERIOR) && cell.fogEnd <= 0.0f && cell.fogDensity != 0.0f)
	{
		cell.fogStart = kInteriorFogView * (1.0f - cell.fogDensity);
		cell.fogEnd = kInteriorFogView;
	}

	u32 numTextures = 0;
	fread(&numTextures, 4, 1, f);
	cell.textures.resize(numTextures);
	cell.textureNames.resize(numTextures);
	int missing = 0;
	u32 before = cache.bytes;
	for (u32 i = 0; i < numTextures; i++)
	{
		char name[65] = {};
		fread(name, 64, 1, f);
		cell.textureNames[i] = name;
		cell.textures[i] = cache.acquire(dataDir, name);
		missing += cell.textures[i] == nullptr;
		if (!cell.textures[i] && !(g_streamCancel && threadGetCurrent()))     // (a cancelled read skips them: not missing)
			monitorOnce((std::string("tex:") + name).c_str(), "missing texture %s (first seen in %s)", name, cellFile);
	}
	// newly loaded; the rest was shared (textures freed meanwhile by the main thread can make it negative)
	cell.textureBytes = cache.bytes > before ? cache.bytes - before : 0;

	bool ok = readBatches(f, cell.batches, cell, h.version);
	if (ok && h.version >= 2)
	{
		if (!collisionRead(cell.collision, f, h.version))
		{
			// No block of heap for it: the cell fails (a cell without its collision would let the player fall through the
			// floor) and waits for room; a short file is only logged, as ever
			if (g_heapProbeFailed)
			{
				logf("cell: %s: no free block of heap for its collision, it waits for room", cellFile);
				ok = false;
			}
			else
				logf("cell: collision data truncated");
		}
		u32 numActors = 0;
		if (ok)
		{
			fread(&numActors, 4, 1, f);
			cell.actors.resize(numActors);
			fread(cell.actors.data(), sizeof(CellActor), numActors, f);
		}
	}
	if (ok && h.version >= 3)
	{
		u32 numDoors = 0;
		fread(&numDoors, 4, 1, f);
		for (u32 i = 0; i < numDoors && ok; i++)
		{
			DoorMesh d;
			u32 ref = 0;
			fread(&ref, 4, 1, f);
			d.ref = ref;
			ok = readBatches(f, d.batches, cell, h.version);
			cell.doors.push_back(std::move(d));
		}
	}
	if (ok && h.version >= 4)
		ok = readBatches(f, cell.sky, cell, h.version);
	if (ok)
		for (CellBatch& b : cell.sky)
			trimClearTriangles(b);
	zclose(f);
	int lamps = 0;
	for (auto& b : cell.batches)
		lamps += b.lamp[0] > 0.0f || b.lamp[1] > 0.0f || b.lamp[2] > 0.0f;
	logf("cell: %s: %d batches (%d lamp lit), %lu tris, %d doors, %d sky, %d actors, collision %d tris, geometry %lu KB, textures %lu KB (%d missing)",
		cellFile, (int)cell.batches.size(), lamps, cell.numTris, (int)cell.doors.size(), (int)cell.sky.size(), (int)cell.actors.size(),
		(int)cell.collision.tris.size() / 3, cell.geometryBytes / 1024, cell.textureBytes / 1024, missing);
	return ok;
}

static void freeBatches(std::vector<CellBatch>& batches)
{
	for (auto& b : batches)
	{
		lockedLinearFree(b.verts);
		lockedLinearFree(b.indices);
	}
	batches.clear();
}

void cellFree(Cell& cell, TextureCache& cache)
{
	freeBatches(cell.batches);
	cell.batchBoundsOf = 0;
	freeBatches(cell.sky);
	for (auto& d : cell.doors)
		freeBatches(d.batches);
	for (auto& name : cell.textureNames)
		cache.release(name);
	cell = Cell();
}
