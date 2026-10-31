// The local map: an interior (or the cells round the player) drawn from above into a texture (OpenMW's LocalMap)

#include <algorithm>
#include <citro3d.h>
#include <cmath>
#include <cstring>

#include "mwrender/renderhelpers.hpp"

// ---- Local map: the interior seen straight down, rendered once into a texture in VRAM

static const int kLocalMapSize = 256;

C3D_Tex* rendererLocalMap()
{
	return g_gpu.localTarget ? &g_gpu.localTex : nullptr;
}

// A picture is drawn a share of the GPU command list at a time, the rest on the next frames into the same texture:
// drawn whole in one frame it could take the list up to the renderer's limit, and the map screen's own drawing after it
// ran the list over (an Old 3DS's is half the size): a panic, the game frozen. Only the cells and batches over the
// picture's square are drawn, kChunk batches between looks at the list
static const float kLocalMapShare = 0.3f;
static const int kChunk = 32;
// The light OpenMW's local map camera puts in place of the sun (LocalMapRenderToTexture: ambient 0.3, diffuse 0.7 from
// (-0.3, -0.3, 0.7)) on a floor seen from above: 0.3 + 0.7 x 0.7 / |(-0.3, -0.3, 0.7)|. The cells' light is baked into
// their colours with no normals left, so it is added evenly; a dark mine's floor plan shows as in OpenMW
static const float kMapLight = 0.9f;
static struct LocalMapJob
{
	bool on = false, opaqueWater = false, cellOver = false, mapLight = false;
	float minX = 0, minY = 0, size = 0, topZ = 0, depth = 0;
	int pass = 0, cell = 0, batch = 0;       // where it goes on: the pass, the loaded cell, its next batch
} s_job;
static std::vector<u8> s_over;               // (reused) which batches of the chunk are over the square

static bool overSquare(const CellBatch& b)
{
	return b.bmax[0] >= s_job.minX && b.bmin[0] <= s_job.minX + s_job.size && b.bmax[1] >= s_job.minY
		&& b.bmin[1] <= s_job.minY + s_job.size;
}

static bool drawLocalMapPart(World& w, bool whole)
{
	setLight(nullptr);
	C3D_FrameDrawOn(g_gpu.localTarget);
	bindPipeline();
	C3D_FVUnifSet(GPU_VERTEX_SHADER, g_gpu.uTint, 1.0f, 1.0f, 1.0f, 1.0f);    // interiors: no time of day
	// (set again on each frame a picture goes on: the world's draws in between set their own)
	if (s_job.mapLight && g_gpu.uAmbient >= 0)
		C3D_FVUnifSet(GPU_VERTEX_SHADER, g_gpu.uAmbient, kMapLight, kMapLight, kMapLight, g_gpu.invGamma);
	disableFog();
	// Looking down -Z from topZ: everything above it (upper floors) is behind the camera, and
	// ceilings face away (culled), which leaves the floor plan
	C3D_Mtx projection, view;
	Mtx_Ortho(&projection, s_job.minX, s_job.minX + s_job.size, s_job.minY, s_job.minY + s_job.size, 0.0f, s_job.depth, false);
	Mtx_Identity(&view);
	Mtx_Translate(&view, 0.0f, 0.0f, -s_job.topZ, true);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_gpu.uProjection, &projection);
	setModelView(view);
	g_gpu.opaqueWater = s_job.opaqueWater;
	const float limit = fminf(0.85f, C3D_GetCmdBufUsage() + kLocalMapShare);
	bool stopped = false;
	while (!stopped && s_job.pass < 3)
	{
		if (s_job.cell >= (int)w.loaded.size())
		{
			s_job.pass++;
			s_job.cell = s_job.batch = 0;
			continue;
		}
		Cell& c = w.loaded[s_job.cell]->cell;
		int n = (int)c.batches.size();
		if (s_job.batch == 0)
		{
			s_over.assign(n, 0);
			s_job.cellOver = false;
			for (int i = 0; i < n && !s_job.cellOver; i++)
				s_job.cellOver = overSquare(c.batches[i]);
		}
		if ((int)s_over.size() != n)
			s_over.assign(n, 0);
		// (its batches a chunk at a time, then its shared models: a cell over the square is drawn whole)
		while (s_job.cellOver && s_job.batch <= n)
		{
			if (!whole && C3D_GetCmdBufUsage() > limit)
			{
				stopped = true;
				break;
			}
			if (s_job.batch == n)
			{
				drawSharedPass(c, s_job.pass, view, w.time, w.refs);
				break;
			}
			int end = std::min(n, s_job.batch + kChunk);
			for (int i = s_job.batch; i < end; i++)
				s_over[i] = overSquare(c.batches[i]);
			drawBatches(c, c.batches, s_job.pass, &s_over, w.time);
			for (int i = s_job.batch; i < end; i++)
				s_over[i] = 0;
			s_job.batch = end;
		}
		if (!stopped)
		{
			s_job.cell++;
			s_job.batch = 0;
		}
	}
	if (!stopped)
		s_job.on = false;
	g_gpu.opaqueWater = false;
	if (s_job.mapLight && g_gpu.uAmbient >= 0)
		C3D_FVUnifSet(GPU_VERTEX_SHADER, g_gpu.uAmbient, 0.0f, 0.0f, 0.0f, g_gpu.invGamma);   // (as setLight(nullptr) left it)
	setUvOffset(0.0f, 0.0f);
	invalidateState();
	g_gpu.mvValid = false;
	return !s_job.on;
}

bool rendererLocalMapMore(World& w)
{
	return !s_job.on || !g_gpu.localTarget || drawLocalMapPart(w, false);
}

bool rendererDrawLocalMap(World& w, float minX, float minY, float size, float topZ, float depth, bool opaqueWater,
	bool whole, bool mapLight)
{
	if (!g_gpu.localTarget)
	{
		if (!C3D_TexInitVRAM(&g_gpu.localTex, kLocalMapSize, kLocalMapSize, GPU_RGBA8))
			return false;
		g_gpu.localTarget = C3D_RenderTargetCreateFromTex(&g_gpu.localTex, GPU_TEXFACE_2D, 0, GPU_RB_DEPTH24_STENCIL8);
		if (!g_gpu.localTarget)
		{
			C3D_TexDelete(&g_gpu.localTex);
			return false;
		}
		C3D_TexSetFilter(&g_gpu.localTex, GPU_LINEAR, GPU_LINEAR);
		C3D_TexSetWrap(&g_gpu.localTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
	}
	C3D_RenderTargetClear(g_gpu.localTarget, C3D_CLEAR_ALL, 0x0C0906FF, 0);
	s_job = LocalMapJob();
	s_job.on = true;
	s_job.opaqueWater = opaqueWater;
	s_job.mapLight = mapLight;
	s_job.minX = minX;
	s_job.minY = minY;
	s_job.size = size;
	s_job.topZ = topZ;
	s_job.depth = depth;
	drawLocalMapPart(w, whole);
	return true;
}
