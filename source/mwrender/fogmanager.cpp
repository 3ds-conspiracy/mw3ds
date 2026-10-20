// Fog: its range and colour ramp, under water, the horizon (OpenMW's FogManager)

#include <algorithm>
#include <citro3d.h>
#include <cmath>
#include <cstring>

#include "mwrender/renderhelpers.hpp"

void rendererFogRange(float& start, float& end)
{
	start = g_gpu.lastFog[0];
	end = g_gpu.lastFog[1];
}

void rendererHorizonFog(float& start, float& end)
{
	start = g_gpu.lastHorizonFog[0];
	end = g_gpu.lastHorizonFog[1];
}

void rendererSetUnderwater(bool on, float fogDepth)
{
	g_gpu.underwaterOn = on;
	g_gpu.underwaterFog = fogDepth;
}

// ---- Fog: linear from fogStart to fogEnd (eye depth). The vertex shader writes the fog amount
// to texture coordinate 1's x, texture unit 1 holds the light ramp, and the fog stage blends
// towards the fog colour by its alpha, a 0..1 ramp across. (The PICA's own fog table is indexed by the non-linear depth
// buffer value, which leaves almost no resolution past a few hundred units.) Down the ramp, the coordinate's y: the
// player's torch lit per pixel, its colour x its falloff at y = depth / (twice its radius) (setLight), in the rgb

static const int kFogRampWidth = 64, kLightRampRows = 32;

// The ramp's texel (x, y): 8x8 tiles in rows, texels in Morton order within a tile, RGBA8 stored a b g r. The first
// row in memory is the texture's top (v = 1)
static u8* rampTexel(int x, int y)
{
	int morton = (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
	return (u8*)g_gpu.fogRamp.data + (((y / 8) * (kFogRampWidth / 8) + x / 8) * 64 + morton) * 4;
}

void initFogRamp()
{
	C3D_TexInit(&g_gpu.fogRamp, kFogRampWidth, kLightRampRows, GPU_RGBA8);
	for (int x = 0; x < kFogRampWidth; x++)
		for (int y = 0; y < kLightRampRows; y++)
		{
			u8* t = rampTexel(x, y);
			t[0] = (u8)(255 * x / (kFogRampWidth - 1));
			t[1] = t[2] = t[3] = 0;
		}
	C3D_TexFlush(&g_gpu.fogRamp);
	C3D_TexSetFilter(&g_gpu.fogRamp, GPU_LINEAR, GPU_LINEAR);
	C3D_TexSetWrap(&g_gpu.fogRamp, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
}

// The ramp's rgb for the torch now carried (its colour, through Gamma's curve as the vertex light is), when either changed
void updateLightRamp()
{
	if (memcmp(g_gpu.rampRgb, g_gpu.lightRgb, sizeof(g_gpu.rampRgb)) == 0 && g_gpu.rampGamma == g_gpu.invGamma)
		return;
	memcpy(g_gpu.rampRgb, g_gpu.lightRgb, sizeof(g_gpu.rampRgb));
	g_gpu.rampGamma = g_gpu.invGamma;
	for (int y = 0; y < kLightRampRows; y++)
	{
		// (row 0, v = 1: twice the radius, nothing; the last row, v = 0: at the eye)
		float d = kTorchReach * (1.0f - (float)y / (kLightRampRows - 1)), over = fminf(fmaxf(d - 1.0f, 0.0f), 1.0f);
		float fall = kTorchFacing * fminf(1.0f, 1.0f / (3.0f * fmaxf(d, 0.001f))) * (1.0f - over * over);
		u8 c[3];
		for (int k = 0; k < 3; k++)
			c[k] = y == 0 ? 0 : (u8)(fminf(1.0f, rendererCurve(g_gpu.lightRgb[k] * fall)) * 255.0f + 0.5f);
		for (int x = 0; x < kFogRampWidth; x++)
		{
			u8* t = rampTexel(x, y);
			t[1] = c[2];
			t[2] = c[1];
			t[3] = c[0];
		}
	}
	C3D_TexFlush(&g_gpu.fogRamp);
}
void rendererSetHorizon(float scale)
{
	g_gpu.horizonScale = scale;
}

void enableFog(const Cell& cell, float fogStart, float fogEnd)
{
	invalidateState();
	setFogVec(fogStart, fogEnd);
	g_gpu.fogColor = curveColor(g_gpu.daylight ? g_gpu.dayFog : 0xFF000000 | (cell.fog[2] << 16) | (cell.fog[1] << 8) | cell.fog[0]);    // 0xAABBGGRR
	C3D_TexBind(1, &g_gpu.fogRamp);
	C3D_TexEnv* env = C3D_GetTexEnv(kTevFog);
	C3D_TexEnvInit(env);
	// rgb = fog * ramp.a + previous * (1 - ramp.a); alpha unchanged
	C3D_TexEnvSrc(env, C3D_RGB, GPU_CONSTANT, GPU_PREVIOUS, GPU_TEXTURE1);
	C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA);
	C3D_TexEnvFunc(env, C3D_RGB, GPU_INTERPOLATE);
	C3D_TexEnvSrc(env, C3D_Alpha, GPU_PREVIOUS);
	C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
	C3D_TexEnvColor(env, g_gpu.fogColor);
	g_gpu.fogOn = true;
}
