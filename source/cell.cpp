#include "cell.h"
#include "planes.h"
#include "datapath.h"

#include <cstdio>
#include <cstring>
#include <tex3ds.h>

#include "linear.h"
#include "log.h"
#include "zfile.h"

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

static bool loadTexture(C3D_Tex* tex, const char* path)
{
	FILE* f = fopen(path, "rb");
	if (!f)
		return false;
	Tex3DS_Texture t3x = Tex3DS_TextureImportStdio(f, tex, nullptr, false);
	fclose(f);
	if (!t3x)
		return false;
	Tex3DS_TextureFree(t3x);
	C3D_TexSetFilter(tex, GPU_LINEAR, GPU_LINEAR);
	C3D_TexSetFilterMipmap(tex, GPU_LINEAR);
	return true;
}

static bool readBatches(FILE* f, std::vector<CellBatch>& out, Cell& cell, u32 version)
{
	u32 count = 0;
	if (fread(&count, 4, 1, f) != 1)
		return false;
	out.reserve(out.size() + count);
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
			// Planes, positions and indices as differences (planes.h)
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

// A failed texture is tried again up to 3 times: the first failure is often linear memory full at that
// moment, and a cached failure kept the weapon or hair white for good
static void tryLoad(TextureCache& c, TextureCache::Entry* e, const char* dataDir, const std::string& name)
{
	char path[256];
	snprintf(path, sizeof(path), "%s", shardedPath(dataDir, "tex", name).c_str());
	e->ok = loadTexture(&e->tex, path);
	if (e->ok)
		c.bytes += C3D_TexCalcTotalSize(e->tex.size, e->tex.maxLevel);
	else if (e->fails++ == 0)
		logf("cell: texture failed: %s", path);
}

C3D_Tex* TextureCache::acquire(const char* dataDir, const std::string& name)
{
	LinearGuard guard;            // the map and the texture memory are shared with the streaming thread
	auto it = entries.find(name);
	if (it == entries.end())
	{
		Entry* e = new Entry();
		e->refs = 0;
		e->fails = 0;
		tryLoad(*this, e, dataDir, name);
		it = entries.emplace(name, e).first;
	}
	else if (!it->second->ok && it->second->fails < 3)
		tryLoad(*this, it->second, dataDir, name);
	it->second->refs++;
	return it->second->ok ? &it->second->tex : nullptr;
}

C3D_Tex* TextureCache::recheck(const char* dataDir, const std::string& name)
{
	LinearGuard guard;
	auto it = entries.find(name);
	if (it == entries.end())
		return nullptr;
	if (!it->second->ok && it->second->fails < 3)
		tryLoad(*this, it->second, dataDir, name);
	return it->second->ok ? &it->second->tex : nullptr;
}

void TextureCache::release(const std::string& name)
{
	LinearGuard guard;
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
	FILE* f = zopen(path);
	if (!f)
	{
		logf("cell: cannot open %s", path);
		return false;
	}

	CellHeader h;
	if (fread(&h, sizeof(h), 1, f) != 1 || memcmp(h.magic, "MWC1", 4) != 0 || h.version < 1 || h.version > 7)
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
		// 1800 / 6000 was the loaded cells' edge
		if (cell.fogEnd > 0.0f && !(cell.flags & CELL_INTERIOR))
		{
			cell.fogStart = kExteriorFogStart;
			cell.fogEnd = kExteriorFogEnd;
		}
		cell.waterZ = h4.waterZ;
		memcpy(cell.bounds, h4.bounds, sizeof(cell.bounds));
	}
	memcpy(cell.spawn, h.spawn, sizeof(cell.spawn));
	cell.yaw = h.yaw;
	cell.pitch = h.pitch;
	memcpy(cell.ambient, h.ambient, 4);
	memcpy(cell.fog, h.fog, 4);
	cell.fogDensity = h.fogDensity;

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
		if (!cell.textures[i])
			monitorOnce((std::string("tex:") + name).c_str(), "missing texture %s (first seen in %s)", name, cellFile);
	}
	// newly loaded; the rest was shared (textures freed meanwhile by the main thread can make it negative)
	cell.textureBytes = cache.bytes > before ? cache.bytes - before : 0;

	bool ok = readBatches(f, cell.batches, cell, h.version);
	if (ok && h.version >= 2)
	{
		if (!collisionRead(cell.collision, f, h.version))
			logf("cell: collision data truncated");
		u32 numActors = 0;
		fread(&numActors, 4, 1, f);
		cell.actors.resize(numActors);
		fread(cell.actors.data(), sizeof(CellActor), numActors, f);
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
	zclose(f);
	logf("cell: %s: %d batches, %lu tris, %d doors, %d sky, %d actors, collision %d tris, geometry %lu KB, textures %lu KB (%d missing)",
		cellFile, (int)cell.batches.size(), cell.numTris, (int)cell.doors.size(), (int)cell.sky.size(), (int)cell.actors.size(),
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
	freeBatches(cell.sky);
	for (auto& d : cell.doors)
		freeBatches(d.batches);
	for (auto& name : cell.textureNames)
		cache.release(name);
	cell = Cell();
}
