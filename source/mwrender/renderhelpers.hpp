#pragma once

// What the renderer's files share (renderingmanager.cpp, fogmanager.cpp, sky.cpp, water.cpp, localmap.cpp and the
// lights in components/sceneutil/lightmanager.cpp): the pipeline and state helpers, the combiner stages
#include "mwrender/renderingmanager.hpp"
#include "mwrender/gpustate.hpp"
#include "mwrender/objectpaging.hpp"

#include <algorithm>
#include <citro3d.h>
#include <cmath>
#include <cstring>

#include "cell_shbin.h"
#include "skin_shbin.h"
#include "platform/linear.hpp"
#include "platform/console.hpp"
#include "components/debug/debuglog.hpp"

static const float kTorchAtEye = 80.0f;     // ... only while it is this near the camera
// ... reaching twice its radius, Morrowind's light falloff along the way: whole to a third of the radius, then
// radius / (3 x distance) (LinearValue 3), faded out over the second radius as the converter's lamps are; x 0.3 for
// the surfaces' facing (no normals here: the average lit side's), against OpenMW in a cave
static const float kTorchReach = 2.0f, kTorchFacing = 0.3f;
// Texture combiner stages: 0 the lit texture (setBatchState), then the player's torch per pixel, the fog, an enchanted
// mesh's glow or shimmer, and an alpha scale (see-through actors)
static const int kTevTorch = 1, kTevFog = 2, kTevGlow = 3, kTevAlpha = 4;

static const float kNoTint[3] = { 1, 1, 1 };

// ---- Batch drawing

enum DrawMode { DRAW_WORLD, DRAW_SKY };

static const float kWaterScroll = 0.06f;         // water texture repeats (1024 units) a second

void uploadTransform();
void bindPipeline();
void setLight(const C3D_Mtx* view);
void actorTint(const Actor& a, float out[3]);
void setLamp(const CellBatch& b);
void initFogRamp();
void updateLightRamp();
void enableFog(const Cell& cell, float fogStart, float fogEnd);
void drawBatchTex(C3D_Tex* tex, const CellBatch& b, DrawMode mode, float time);
int drawBatches(Cell& cell, std::vector<CellBatch>& batches, int pass, const std::vector<u8>* visible, float time);
void drawSkyBodies(const C3D_Mtx& view, bool secondEye);
extern int g_torchPerPixel;
extern int g_lampLitBatches;
static void setModelView(const C3D_Mtx& mv);
static u32 curveColor(u32 c);
static void setUvOffset(float u, float v);
static void setTint(const float t[3]);
static const float* worldTint();
static void setFogVec(float start, float end);
static void disableFog();
static void invalidateState();

static inline void setModelView(const C3D_Mtx& mv)
{
	g_gpu.mv = mv;
	uploadTransform();
}

// A 0xAABBGGRR colour through Gamma's curve (the fog: what the world fades into is curved as the world is)
static inline u32 curveColor(u32 c)
{
	if (!g_gpu.gammaOn)
		return c;
	u32 out = c & 0xFF000000;
	for (int sh = 0; sh < 24; sh += 8)
		out |= (u32)(rendererCurve(((c >> sh) & 0xFF) / 255.0f) * 255.0f + 0.5f) << sh;
	return out;
}

static inline void setUvOffset(float u, float v)
{
	if (u == g_gpu.uvOffset[0] && v == g_gpu.uvOffset[1])
		return;
	C3D_FVUnifSet(GPU_VERTEX_SHADER, g_gpu.uUvOffset, u, v, 0.0f, 0.0f);
	g_gpu.uvOffset[0] = u;
	g_gpu.uvOffset[1] = v;
}

static inline void setTint(const float t[3])
{
	C3D_FVUnifSet(GPU_VERTEX_SHADER, g_gpu.uTint, t[0], t[1], t[2], 1.0f);
}

// The world's tint: the hour's outdoors, none indoors
static inline const float* worldTint()
{
	return g_gpu.daylight ? g_gpu.land : kNoTint;
}

static inline void setFogVec(float start, float end)
{
	// View space looks down -Z: fog = (-z - start) / (end - start)
	float k = end > start ? 1.0f / (end - start) : 0.0f;
	C3D_FVUnifSet(GPU_VERTEX_SHADER, g_gpu.uFogVec, 0.0f, 0.0f, -k, -start * k);
}

static inline void disableFog()
{
	invalidateState();
	setFogVec(0.0f, 0.0f);
	C3D_TexEnvInit(C3D_GetTexEnv(kTevFog));
	g_gpu.fogOn = false;
}

static inline void invalidateState()
{
	g_gpu.last.valid = false;
}
