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

bool rendererDrawLocalMap(World& w, float minX, float minY, float size, float topZ, float depth, bool opaqueWater)
{
	setLight(nullptr);
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
	C3D_FrameDrawOn(g_gpu.localTarget);
	bindPipeline();
	C3D_FVUnifSet(GPU_VERTEX_SHADER, g_gpu.uTint, 1.0f, 1.0f, 1.0f, 1.0f);    // interiors: no time of day
	disableFog();
	// Looking down -Z from topZ: everything above it (upper floors) is behind the camera, and
	// ceilings face away (culled), which leaves the floor plan
	C3D_Mtx projection, view;
	Mtx_Ortho(&projection, minX, minX + size, minY, minY + size, 0.0f, depth, false);
	Mtx_Identity(&view);
	Mtx_Translate(&view, 0.0f, 0.0f, -topZ, true);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_gpu.uProjection, &projection);
	setModelView(view);
	g_gpu.opaqueWater = opaqueWater;
	for (int pass = 0; pass < 3; pass++)
		for (LoadedCell* l : w.loaded)
			drawBatches(l->cell, l->cell.batches, pass, nullptr, w.time);
	g_gpu.opaqueWater = false;
	setUvOffset(0.0f, 0.0f);
	invalidateState();
	g_gpu.mvValid = false;
	return true;
}
