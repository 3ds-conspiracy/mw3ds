#include "renderer.h"
#include "distant.h"

#include <algorithm>
#include <citro3d.h>
#include <cmath>
#include <cstring>

#include "cell_shbin.h"
#include "linear.h"
#include "log.h"

float g_profActorsMs = 0.0f;          // debug profile accumulators, reported by main once a second
float g_profWorldMs = 0.0f;
int g_drawnBatches = 0, g_culledBatches = 0;
int g_skippedDraws = 0;               // draws dropped because the GPU command buffer was nearly full

static DVLB_s* s_dvlb;
static shaderProgram_s s_program;
static int s_uProjection, s_uModelView, s_uUvOffset, s_uFogVec, s_uPosOffset, s_uPosScale, s_uUvScale, s_uTint = -1;
static int s_uLightPos = -1, s_uLightColor = -1;
static float s_light[3], s_lightRadius = 0.0f, s_lightRgb[3] = { 0, 0, 0 };
// Day and night outdoors (World::daylight): tint on the baked light, the sky's, and the fog colour
static float s_land[3] = { 1, 1, 1 }, s_skyTint[3] = { 1, 1, 1 };
static u32 s_dayFog = 0;
static bool s_daylight = false;
static bool s_formatValid = false;                // vertex format / quantization currently bound
static bool s_packedBound = false;
static float s_boundQuant[8];                     // pos offset, pos scale, uv scale
static float s_uvOffset[2];
static C3D_Tex s_fogRamp;
static void initFogRamp();
static C3D_Tex s_localTex;                        // local map (rendererDrawLocalMap)
static C3D_RenderTarget* s_localTarget = nullptr;

static const float kFovY = 60.0f;
static const float kNear = 5.0f;
static const float kInteriorFar = 12000.0f;

void rendererInit()
{
	s_dvlb = DVLB_ParseFile((u32*)cell_shbin, cell_shbin_size);
	shaderProgramInit(&s_program);
	shaderProgramSetVsh(&s_program, &s_dvlb->DVLE[0]);
	s_uProjection = shaderInstanceGetUniformLocation(s_program.vertexShader, "projection");
	s_uModelView = shaderInstanceGetUniformLocation(s_program.vertexShader, "modelView");
	s_uUvOffset = shaderInstanceGetUniformLocation(s_program.vertexShader, "uvOffset");
	s_uFogVec = shaderInstanceGetUniformLocation(s_program.vertexShader, "fogVec");
	s_uPosOffset = shaderInstanceGetUniformLocation(s_program.vertexShader, "posOffset");
	s_uPosScale = shaderInstanceGetUniformLocation(s_program.vertexShader, "posScale");
	s_uUvScale = shaderInstanceGetUniformLocation(s_program.vertexShader, "uvScale");
	s_uTint = shaderInstanceGetUniformLocation(s_program.vertexShader, "tint");
	s_uLightPos = shaderInstanceGetUniformLocation(s_program.vertexShader, "lightPos");
	s_uLightColor = shaderInstanceGetUniformLocation(s_program.vertexShader, "lightColor");
	initFogRamp();
}

void rendererExit()
{
	if (s_localTarget)
	{
		C3D_RenderTargetDelete(s_localTarget);
		C3D_TexDelete(&s_localTex);
		s_localTarget = nullptr;
	}
	C3D_TexDelete(&s_fogRamp);
	shaderProgramFree(&s_program);
	DVLB_Free(s_dvlb);
}

static void invalidateState();

// Model-view uniform, uploaded only when it changes (skinned parts of an actor share one)
static C3D_Mtx s_boundMv;
static bool s_mvValid = false;

static void setModelView(const C3D_Mtx& mv)
{
	if (s_mvValid && memcmp(&mv, &s_boundMv, sizeof(mv)) == 0)
		return;
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uModelView, &mv);
	s_boundMv = mv;
	s_mvValid = true;
}

static void bindPipeline()
{
	invalidateState();
	s_mvValid = false;
	// citro2d changes the program and vertex layout, so set ours up every frame
	C3D_BindProgram(&s_program);
	s_formatValid = false;      // citro2d changed the layout: the first batch sets ours up again
	for (int i = 1; i < 6; i++)
		C3D_TexEnvInit(C3D_GetTexEnv(i));
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uUvOffset, 0.0f, 0.0f, 0.0f, 0.0f);
	s_uvOffset[0] = s_uvOffset[1] = 0.0f;
	// Colour tint: the time of day's outdoors, none indoors (uniforms start at 0: always set)
	const float* t = s_daylight ? s_land : nullptr;
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uTint, t ? t[0] : 1.0f, t ? t[1] : 1.0f, t ? t[2] : 1.0f, 1.0f);
}

void rendererSetCarriedLight(const float pos[3], float radius, const float rgb[3])
{
	memcpy(s_light, pos, sizeof(s_light));
	s_lightRadius = radius;
	memcpy(s_lightRgb, rgb, sizeof(s_lightRgb));
}

// The carried light in this view's eye space (off: no colour)
static void setLight(const C3D_Mtx* view)
{
	if (s_uLightColor < 0 || s_uLightPos < 0)
		return;
	if (!view || s_lightRadius <= 0.0f)
	{
		C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uLightColor, 0, 0, 0, 0);
		C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uLightPos, 0, 0, 0, 0);
		return;
	}
	float e[3];
	for (int r = 0; r < 3; r++)
		e[r] = view->r[r].x * s_light[0] + view->r[r].y * s_light[1] + view->r[r].z * s_light[2] + view->r[r].w;
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uLightPos, e[0], e[1], e[2], 1.0f / (s_lightRadius * s_lightRadius));
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uLightColor, s_lightRgb[0], s_lightRgb[1], s_lightRgb[2], 0.0f);
}

static void setUvOffset(float u, float v)
{
	if (u == s_uvOffset[0] && v == s_uvOffset[1])
		return;
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uUvOffset, u, v, 0.0f, 0.0f);
	s_uvOffset[0] = u;
	s_uvOffset[1] = v;
}

// ---- Fog: linear from fogStart to fogEnd (eye depth). The vertex shader writes the fog amount
// to texture coordinate 1, texture unit 1 holds a 0..1 alpha ramp, and combiner stage 1 blends
// towards the fog colour by it. (The PICA's own fog table is indexed by the non-linear depth
// buffer value, which leaves almost no resolution past a few hundred units.)

static const int kFogRampWidth = 64;
static bool s_fogOn = false;
static u32 s_fogColor = 0;

void rendererSetDaylight(const float land[3], const float sky[3], const float fog[3], bool outdoors)
{
	memcpy(s_land, land, sizeof(s_land));
	memcpy(s_skyTint, sky, sizeof(s_skyTint));
	s_dayFog = 0xFF000000 | ((u32)(fog[2] * 255) << 16) | ((u32)(fog[1] * 255) << 8) | (u32)(fog[0] * 255);
	s_daylight = outdoors;
}

static void setTint(const float t[3])
{
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uTint, t[0], t[1], t[2], 1.0f);
}

static void initFogRamp()
{
	C3D_TexInit(&s_fogRamp, kFogRampWidth, 8, GPU_A8);
	u8* data = (u8*)s_fogRamp.data;
	for (int x = 0; x < kFogRampWidth; x++)
		for (int y = 0; y < 8; y++)
		{
			// 8x8 tiles, texels in Morton order within a tile
			int morton = (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
			data[(x / 8) * 64 + morton] = (u8)(255 * x / (kFogRampWidth - 1));
		}
	C3D_TexFlush(&s_fogRamp);
	C3D_TexSetFilter(&s_fogRamp, GPU_LINEAR, GPU_LINEAR);
	C3D_TexSetWrap(&s_fogRamp, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
}

static void setFogVec(float start, float end)
{
	// View space looks down -Z: fog = (-z - start) / (end - start)
	float k = end > start ? 1.0f / (end - start) : 0.0f;
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uFogVec, 0.0f, 0.0f, -k, -start * k);
}

static void enableFog(const Cell& cell, float fogStart, float fogEnd)
{
	invalidateState();
	setFogVec(fogStart, fogEnd);
	s_fogColor = s_daylight ? s_dayFog : 0xFF000000 | (cell.fog[2] << 16) | (cell.fog[1] << 8) | cell.fog[0];    // 0xAABBGGRR
	C3D_TexBind(1, &s_fogRamp);
	C3D_TexEnv* env = C3D_GetTexEnv(1);
	C3D_TexEnvInit(env);
	// rgb = fog * ramp.a + previous * (1 - ramp.a); alpha unchanged
	C3D_TexEnvSrc(env, C3D_RGB, GPU_CONSTANT, GPU_PREVIOUS, GPU_TEXTURE1);
	C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA);
	C3D_TexEnvFunc(env, C3D_RGB, GPU_INTERPOLATE);
	C3D_TexEnvSrc(env, C3D_Alpha, GPU_PREVIOUS);
	C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
	C3D_TexEnvColor(env, s_fogColor);
	s_fogOn = true;
}

static void disableFog()
{
	invalidateState();
	setFogVec(0.0f, 0.0f);
	C3D_TexEnvInit(C3D_GetTexEnv(1));
	s_fogOn = false;
}

// ---- Culling: batch bounds against the view frustum and the fog distance

struct Frustum
{
	float eye[3];
	float planes[4][3];     // inward normals through the eye (left, right, bottom, top)
	float fwd[3];
	float farDist;          // depth past which everything is fog; 0 = no limit
};

static Frustum makeFrustum(const RenderCamera& cam, float farDist)
{
	Frustum fr;
	float cp = cosf(cam.pitch), sp = sinf(cam.pitch), cy = cosf(cam.yaw), sy = sinf(cam.yaw);
	float f[3] = { sy * cp, cy * cp, sp };
	float r[3] = { cy, -sy, 0.0f };
	float u[3] = { -sy * sp, -cy * sp, cp };           // r x f
	// A little wider than the real view so nothing pops at the edges
	float tanV = tanf(C3D_AngleFromDegrees(kFovY) * 0.5f) * 1.15f;
	float tanH = tanV * C3D_AspectRatioTop;
	for (int k = 0; k < 3; k++)
	{
		fr.eye[k] = cam.pos[k];
		fr.fwd[k] = f[k];
		fr.planes[0][k] = f[k] * tanH + r[k];
		fr.planes[1][k] = f[k] * tanH - r[k];
		fr.planes[2][k] = f[k] * tanV + u[k];
		fr.planes[3][k] = f[k] * tanV - u[k];
	}
	fr.farDist = farDist;
	return fr;
}

static bool boxVisible(const Frustum& fr, const float* bmin, const float* bmax, float maxDist = 0.0f)
{
	for (auto& n : fr.planes)
	{
		// Box corner farthest along the plane normal
		float d = 0.0f;
		for (int k = 0; k < 3; k++)
			d += ((n[k] >= 0.0f ? bmax[k] : bmin[k]) - fr.eye[k]) * n[k];
		if (d < 0.0f)
			return false;
	}
	if (maxDist > 0.0f)
	{
		float d2 = 0.0f;
		for (int k = 0; k < 3; k++)
		{
			float c = fmaxf(bmin[k], fminf(fr.eye[k], bmax[k])) - fr.eye[k];
			d2 += c * c;
		}
		if (d2 > maxDist * maxDist)
			return false;
	}
	if (fr.farDist > 0.0f)
	{
		// Fog grows with depth along the view direction, so cull on the nearest corner's depth
		float d = 0.0f;
		for (int k = 0; k < 3; k++)
			d += ((fr.fwd[k] >= 0.0f ? bmin[k] : bmax[k]) - fr.eye[k]) * fr.fwd[k];
		if (d > fr.farDist)
			return false;
	}
	return true;
}

// ---- Batch drawing

enum DrawMode { DRAW_WORLD, DRAW_SKY };

static C3D_Mtx s_view;                            // of the last rendererDrawWorld, for the extra draws
static C3D_Mtx s_projection;
static float s_eyeShift = 0.0f, s_farPlane = 1000.0f;
static float s_time;
static int s_glowCount = 0;

static C3D_Tex* batchTexture(Cell& cell, const CellBatch& b)
{
	return b.tex >= 0 && b.tex < (int)cell.textures.size() ? cell.textures[b.tex] : nullptr;
}

// The last render state set, so consecutive draws with the same state skip re-setting it (each
// citro3d state call re-sends GPU commands). Anything else that touches state invalidates it.
static struct { bool valid; C3D_Tex* tex; u8 flags, alphaRef; int mode; } s_last;

static void invalidateState()
{
	s_last.valid = false;
}

static void setBatchState(C3D_Tex* tex, const CellBatch& b, DrawMode mode, float time)
{
	if (s_last.valid && s_last.tex == tex && s_last.flags == b.flags && s_last.alphaRef == b.alphaRef
		&& s_last.mode == mode)
		return;
	s_last = { true, tex, b.flags, b.alphaRef, (int)mode };
	if (b.flags & BATCH_BLEND)
	{
		GPU_BLENDFACTOR dst = (b.flags & BATCH_ADDITIVE) ? GPU_ONE : GPU_ONE_MINUS_SRC_ALPHA;
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, dst, GPU_SRC_ALPHA, dst);
	}
	else
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	if (mode == DRAW_SKY)
		C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
	else if (b.flags & BATCH_DECAL)
		C3D_DepthTest(true, GPU_GEQUAL, GPU_WRITE_COLOR);
	else
		C3D_DepthTest(true, GPU_GREATER, (b.flags & BATCH_BLEND) ? GPU_WRITE_COLOR : GPU_WRITE_ALL);
	C3D_AlphaTest((b.flags & BATCH_TEST) != 0, GPU_GREATER, b.alphaRef);
	C3D_CullFace((b.flags & BATCH_TWO_SIDED) ? GPU_CULL_NONE : GPU_CULL_BACK_CCW);

	if (b.flags & BATCH_SCROLL)
	{
		// Clouds drift slowly overhead, water a little faster
		float speed = mode == DRAW_SKY ? 0.006f : 0.02f;
		float s = fmodf(time * speed, 1.0f);
		setUvOffset(s, s * 0.6f);
	}
	else if ((b.flags & (BATCH_RISE | BATCH_CLAMP)) == (BATCH_RISE | BATCH_CLAMP))
	{
		// Flames: the picture flickers and bobs on its quad (no wrapping: it's clamped)
		setUvOffset(0.05f * sinf(time * 9.0f) + 0.03f * sinf(time * 23.0f), 0.04f * sinf(time * 13.0f));
	}
	else if (b.flags & BATCH_RISE)
	{
		// Smoke and steam: the puff climbs the quad and sways a little
		setUvOffset(0.05f * sinf(time * 1.3f), fmodf(time * 0.35f, 1.0f));
	}
	else
		setUvOffset(0.0f, 0.0f);

	if (s_fogOn)
		C3D_TexEnvColor(C3D_GetTexEnv(1), (b.flags & BATCH_ADDITIVE) ? 0xFF000000 : s_fogColor);

	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	if (tex)
	{
		GPU_TEXTURE_WRAP_PARAM wrap = (b.flags & BATCH_CLAMP) ? GPU_CLAMP_TO_EDGE : GPU_REPEAT;
		C3D_TexSetWrap(tex, wrap, wrap);
		C3D_TexBind(0, tex);
		C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR);
		C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
	}
	else
	{
		C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR);
		C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	}
}

// Vertex layout and quantization uniforms: packed s16 (cell format 5) or float (actors, older cells)
static void bindVertexFormat(const CellBatch& b)
{
	bool packed = b.stride == CELL_VERTEX_SIZE_PACKED;
	if (!s_formatValid || packed != s_packedBound)
	{
		C3D_AttrInfo* attrInfo = C3D_GetAttrInfo();
		AttrInfo_Init(attrInfo);
		if (packed)
		{
			AttrInfo_AddLoader(attrInfo, 0, GPU_SHORT, 4);         // v0 = position (w unused)
			AttrInfo_AddLoader(attrInfo, 1, GPU_SHORT, 2);         // v1 = texcoord
		}
		else
		{
			AttrInfo_AddLoader(attrInfo, 0, GPU_FLOAT, 3);
			AttrInfo_AddLoader(attrInfo, 1, GPU_FLOAT, 2);
		}
		AttrInfo_AddLoader(attrInfo, 2, GPU_UNSIGNED_BYTE, 4); // v2 = baked color
		s_packedBound = packed;
	}
	float q[8] = { b.posOffset[0], b.posOffset[1], b.posOffset[2], b.posScale[0], b.posScale[1], b.posScale[2],
		b.uvScale[0], b.uvScale[1] };
	if (!s_formatValid || memcmp(q, s_boundQuant, sizeof(q)) != 0)
	{
		C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uPosOffset, q[0], q[1], q[2], 0.0f);
		C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uPosScale, q[3], q[4], q[5], 0.0f);
		C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uUvScale, q[6], q[7], 1.0f, 1.0f);
		memcpy(s_boundQuant, q, sizeof(q));
	}
	s_formatValid = true;
}

// Overflowing the command buffer is a panic (a freeze on hardware): past this share the world
// stops drawing, leaving room for the HUD and the bottom screen
static const float kCmdBufLimit = 0.9f;

static void drawBatchTex(C3D_Tex* tex, const CellBatch& b, DrawMode mode, float time)
{
	// A draw of no triangles hangs the PICA on hardware (emulators skip it)
	if (b.numIndices < 3 || !b.verts || !b.indices)
		return;
	if (C3D_GetCmdBufUsage() > kCmdBufLimit)
	{
		g_skippedDraws++;
		return;
	}
	setBatchState(tex, b, mode, time);
	bindVertexFormat(b);
	C3D_BufInfo* bufInfo = C3D_GetBufInfo();
	BufInfo_Init(bufInfo);
	BufInfo_Add(bufInfo, b.verts, b.stride, 3, 0x210);
	C3D_DrawElements(GPU_TRIANGLES, b.numIndices, C3D_UNSIGNED_SHORT, b.indices);
}

static void drawBatch(Cell& cell, const CellBatch& b, DrawMode mode, float time)
{
	drawBatchTex(batchTexture(cell, b), b, mode, time);
}

// Pass: 0 opaque (incl. alpha tested), 1 decal overlays, 2 blended
static int batchPass(const CellBatch& b)
{
	if (b.flags & BATCH_DECAL)
		return 1;
	return (b.flags & BATCH_BLEND) ? 2 : 0;
}

static int drawBatches(Cell& cell, std::vector<CellBatch>& batches, int pass, const std::vector<u8>* visible, float time)
{
	int draws = 0;
	for (size_t i = 0; i < batches.size(); i++)
	{
		const CellBatch& b = batches[i];
		if (batchPass(b) != pass || (visible && !(*visible)[i]))
			continue;
		drawBatch(cell, b, DRAW_WORLD, time);
		draws++;
	}
	return draws;
}

std::vector<SkyBillboard> g_skyBillboards;
float g_starAlpha = 0.0f;
C3D_Tex* g_starTexture = nullptr;

// The stars (a dome, tiled) and the sun / moons (squares toward the eye) around the camera, no depth
static void drawSkyBodies(const C3D_Mtx& view, bool secondEye)
{
	static const int kMax = 16;
	static u8* ring = nullptr;                   // this frame's billboard vertices (each eye its own)
	static u16* quad = nullptr;
	static u8* dome = nullptr;
	static u16* domeIdx = nullptr;
	static int domeVerts = 0, domeIndices = 0, slot = 0;
	if (!ring)
	{
		ring = (u8*)lockedLinearAlloc(kMax * 2 * 4 * 24);
		quad = (u16*)lockedLinearAlloc(6 * 2);
		static const u16 q[6] = { 0, 1, 2, 0, 2, 3 };
		if (quad)
		{
			memcpy(quad, q, sizeof(q));
			GSPGPU_FlushDataCache(quad, sizeof(q));
		}
		// star dome: elevation rings 5..90 degrees, 12 around; the texture repeats 4 times around
		const int seg = 12, rings = 5;
		const float elev[rings] = { 0, 15, 35, 60, 90 };
		domeVerts = (seg + 1) * rings;
		dome = (u8*)lockedLinearAlloc(domeVerts * 24);
		std::vector<u16> idx;
		for (int r = 0; r < rings && dome; r++)
			for (int i = 0; i <= seg; i++)
			{
				float e = elev[r] * 0.0174533f, a = 6.2831853f * i / seg;
				float* p = (float*)(dome + (r * (seg + 1) + i) * 24);
				p[0] = cosf(e) * sinf(a) * 2800.0f;
				p[1] = cosf(e) * cosf(a) * 2800.0f;
				p[2] = sinf(e) * 2800.0f;
				p[3] = i * 4.0f / seg;
				p[4] = r * 1.5f / (rings - 1);
				u32 white = 0xFFFFFFFF;
				memcpy(dome + (r * (seg + 1) + i) * 24 + 20, &white, 4);
				if (r < rings - 1 && i < seg)
				{
					u16 v = r * (seg + 1) + i, u = v + seg + 1;
					u16 t[6] = { v, (u16)(v + 1), (u16)(u + 1), v, (u16)(u + 1), u };
					idx.insert(idx.end(), t, t + 6);
				}
			}
		domeIndices = idx.size();
		domeIdx = (u16*)lockedLinearAlloc(idx.size() * 2);
		if (domeIdx)
		{
			memcpy(domeIdx, idx.data(), idx.size() * 2);
			GSPGPU_FlushDataCache(domeIdx, idx.size() * 2);
		}
		if (dome)
			GSPGPU_FlushDataCache(dome, domeVerts * 24);
	}
	if (!ring || !quad)
		return;
	C3D_Mtx skyView = view;
	skyView.r[0].w = skyView.r[1].w = skyView.r[2].w = 0.0f;
	setModelView(skyView);
	static const float one[3] = { 1, 1, 1 };
	setTint(one);
	// the stars, fading in at night
	if (g_starTexture && g_starAlpha > 0.01f && dome && domeIdx)
	{
		u8 a = (u8)(fminf(1.0f, g_starAlpha) * 255.0f);
		for (int i = 0; i < domeVerts; i++)
			dome[i * 24 + 23] = a;
		GSPGPU_FlushDataCache(dome, domeVerts * 24);
		CellBatch b = {};
		b.tex = 0;
		b.flags = BATCH_BLEND | BATCH_ADDITIVE | BATCH_TWO_SIDED;
		b.numVerts = domeVerts;
		b.numIndices = domeIndices;
		b.verts = dome;
		b.indices = domeIdx;
		b.stride = CELL_VERTEX_SIZE;
		drawBatchTex(g_starTexture, b, DRAW_SKY, 0.0f);
	}
	if (!secondEye)
		slot = 0;
	float right[3] = { skyView.r[0].x, skyView.r[0].y, skyView.r[0].z };
	float up[3] = { skyView.r[1].x, skyView.r[1].y, skyView.r[1].z };
	for (auto& sb : g_skyBillboards)
	{
		if (slot >= kMax * 2 || !sb.tex)
			break;
		u8* v = ring + (slot++) * 4 * 24;
		static const float corner[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
		static const float uv[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
		float cr = cosf(sb.roll), sr = sinf(sb.roll);
		for (int i = 0; i < 4; i++)
		{
			float cx = corner[i][0] * cr - corner[i][1] * sr, cy = corner[i][0] * sr + corner[i][1] * cr;
			float* p = (float*)(v + i * 24);
			for (int k = 0; k < 3; k++)
				p[k] = sb.dir[k] * 2700.0f + (right[k] * cx + up[k] * cy) * sb.size;
			p[3] = uv[i][0];
			p[4] = uv[i][1];
			memcpy(v + i * 24 + 20, &sb.rgba, 4);
		}
		GSPGPU_FlushDataCache(v, 4 * 24);
		CellBatch b = {};
		b.tex = 0;
		b.flags = BATCH_BLEND | BATCH_TWO_SIDED | BATCH_CLAMP | (sb.additive ? BATCH_ADDITIVE : 0);
		b.numVerts = 4;
		b.numIndices = 6;
		b.verts = v;
		b.indices = quad;
		b.stride = CELL_VERTEX_SIZE;
		drawBatchTex(sb.tex, b, DRAW_SKY, 0.0f);
	}
}

// Distance at which the stereo image sits on the screen surface (world units, ~1.7 m): nearer
// things come out of the screen, the rest is behind it
static const float kStereoFocus = 120.0f;
// ... and for the first-person arms and weapon, drawn with half the eye separation
static const float kViewModelFocus = 30.0f;
// Terrain texture blend layers (decal batches) are drawn within this distance
static const float kTerrainLayerDistance = 4000.0f;
// Actor meshes under kActorDetailTris triangles are skipped past this distance
static const float kActorDetailDistance = 1800.0f;
static const u32 kActorDetailTris = 60;

// Parts of the world drawn (RENDER_* bits); the first frames after a load add them one at a time
// so a GPU hang on hardware names the part that caused it
u32 g_renderParts = ~0u;
// Where a point straight ahead at that depth lands for an eye (pixels along the screen's width, + right),
// relative to where a flat (non-stereo) view puts it: what the 2D overlay adds to sit at that depth.
// Measured through the same matrices the world uses, so axis and signs follow the tilt
float rendererStereoPixels(float depth, float eyeShift)
{
	if (eyeShift == 0.0f || depth <= kNear)
		return 0.0f;
	C3D_Mtx flat, eye;
	Mtx_PerspTilt(&flat, C3D_AngleFromDegrees(kFovY), C3D_AspectRatioTop, kNear, depth * 2.0f, false);
	Mtx_PerspStereoTilt(&eye, C3D_AngleFromDegrees(kFovY), C3D_AspectRatioTop, kNear, depth * 2.0f, eyeShift,
		kStereoFocus, false);
	auto ndc = [](const C3D_Mtx& m, float x, float z, float out[2]) {
		C3D_FVec c = Mtx_MultiplyFVec4(&m, FVec4_New(x, 0.0f, -z, 1.0f));
		out[0] = c.x / c.w;
		out[1] = c.y / c.w;
	};
	float centre[2], right[2], seen[2];
	ndc(flat, 0.0f, depth, centre);
	ndc(flat, depth * 0.1f, depth, right);
	ndc(eye, 0.0f, depth, seen);
	int axis = fabsf(right[0] - centre[0]) > fabsf(right[1] - centre[1]) ? 0 : 1;
	float sign = right[axis] > centre[axis] ? 1.0f : -1.0f;
	return (seen[axis] - centre[axis]) * sign * 200.0f;
}

int rendererDrawWorld(World& w, const RenderCamera& cam, float eyeShift, bool secondEye, float fogScale)
{
	MARK("draw world");
	Cell& here = w.here();
	bindPipeline();
	bool fog = here.fogEnd > 0.0f;
	float fogStart = here.fogStart * fogScale, fogEnd = here.fogEnd * fogScale;
	float farPlane = fog ? fogEnd + 500.0f : kInteriorFar;
	float time = w.time;

	C3D_Mtx projection, view;
	if (eyeShift != 0.0f)
		Mtx_PerspStereoTilt(&projection, C3D_AngleFromDegrees(kFovY), C3D_AspectRatioTop, kNear, farPlane,
			eyeShift, kStereoFocus, false);
	else
		Mtx_PerspTilt(&projection, C3D_AngleFromDegrees(kFovY), C3D_AspectRatioTop, kNear, farPlane, false);
	float cp = cosf(cam.pitch);
	C3D_FVec eye = FVec3_New(cam.pos[0], cam.pos[1], cam.pos[2]);
	C3D_FVec target = FVec3_New(cam.pos[0] + sinf(cam.yaw) * cp, cam.pos[1] + cosf(cam.yaw) * cp,
		cam.pos[2] + sinf(cam.pitch));
	Mtx_LookAt(&view, eye, target, FVec3_New(0.0f, 0.0f, 1.0f), false);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uProjection, &projection);
	s_projection = projection;
	s_eyeShift = eyeShift;
	s_farPlane = farPlane;
	s_view = view;
	s_time = time;
	if (!secondEye)
		s_glowCount = 0;              // the second eye's glows get their own vertices

	int draws = 0;
	disableFog();
	setLight(nullptr);
	static const float kOne[3] = { 1, 1, 1 };

	// Sky: centred on the camera (rotation only), no depth, no fog
	if (!here.sky.empty() && (g_renderParts & RENDER_SKY))
	{
		setTint(s_daylight ? s_skyTint : kOne);
		C3D_Mtx skyView = view;
		skyView.r[0].w = skyView.r[1].w = skyView.r[2].w = 0.0f;
		setModelView(skyView);
		for (auto& b : here.sky)
		{
			drawBatch(here, b, DRAW_SKY, time);
			draws++;
		}
	}
	if (!here.sky.empty() && (g_renderParts & RENDER_SKY))
		drawSkyBodies(view, secondEye);
	setModelView(view);
	setTint(s_daylight ? s_land : kOne);
	setLight(&view);
	if (fog)
		enableFog(here, fogStart, fogEnd);

	// Which static batches of each loaded cell can be seen this frame. The second eye (a few
	// units to the side, inside the frustum's margin) reuses the first eye's lists.
	Frustum fr = makeFrustum(cam, fog ? fogEnd : 0.0f);
	static std::vector<std::vector<u8>> visible;
	int shown = 0, total = 0;
	if (!secondEye)
		visible.resize(w.loaded.size());
	for (size_t k = 0; k < w.loaded.size() && !secondEye; k++)
	{
		Cell& cell = w.loaded[k]->cell;
		visible[k].resize(cell.batches.size());
		for (size_t i = 0; i < cell.batches.size(); i++)
		{
			const CellBatch& b = cell.batches[i];
			// Clutter's own cutoff shrinks with the view; terrain blend layers only near the player
			// (far chunks show their main texture, mostly under fog anyway)
			float maxDist = b.maxDist * fogScale;
			if (b.flags & BATCH_DECAL)
				maxDist = kTerrainLayerDistance * fogScale;
			visible[k][i] = boxVisible(fr, b.bmin, b.bmax, maxDist);
			shown += visible[k][i];
		}
		total += cell.batches.size();
	}
	if (!secondEye)
	{
		g_drawnBatches = shown;
		g_culledBatches = total - shown;
	}

	// Opaque batches of all loaded cells sorted by texture and state, so the state cache above
	// skips most changes (order doesn't matter for them); terrain layers keep their order
	struct OpaqueDraw { C3D_Tex* tex; u32 state; const CellBatch* b; };
	static std::vector<OpaqueDraw> opaque;
	if (!secondEye)
		opaque.clear();
	for (size_t k = 0; k < w.loaded.size() && !secondEye; k++)
	{
		Cell& cell = w.loaded[k]->cell;
		for (size_t i = 0; i < cell.batches.size(); i++)
			if (visible[k][i] && batchPass(cell.batches[i]) == 0)
				opaque.push_back({ batchTexture(cell, cell.batches[i]),
					(u32)cell.batches[i].flags | ((u32)cell.batches[i].alphaRef << 8), &cell.batches[i] });
	}
	if (!secondEye)
		std::sort(opaque.begin(), opaque.end(), [](const OpaqueDraw& a, const OpaqueDraw& b) {
			return a.tex != b.tex ? a.tex < b.tex : a.state < b.state;
		});
	if (g_renderParts & RENDER_OPAQUE)
		for (auto& d : opaque)
			drawBatchTex(d.tex, *d.b, DRAW_WORLD, time);
	draws += opaque.size();

	// Distant land past the loaded cells (drawn after them: what they cover fails the depth test)
	if (fog && !(here.flags & CELL_INTERIOR) && (g_renderParts & RENDER_OPAQUE))
	{
		int gx = (int)floorf(cam.pos[0] / 8192.0f), gy = (int)floorf(cam.pos[1] / 8192.0f);
		if (!secondEye)
			distantUpdate(gx, gy, (int)ceilf(fogEnd / 8192.0f) + 1);
		for (auto& d : distantCells())
		{
			int ci = w.gridCell(d.gx, d.gy);
			if (ci >= 0 && w.cells[ci].live)
				continue;
			if (boxVisible(fr, d.batch.bmin, d.batch.bmax, fogEnd + 500.0f))
			{
				drawBatchTex(nullptr, d.batch, DRAW_WORLD, time);
				draws++;
			}
			if (d.statics.numIndices && boxVisible(fr, d.statics.bmin, d.statics.bmax, fogEnd + 500.0f))
			{
				drawBatchTex(nullptr, d.statics, DRAW_WORLD, time);
				draws++;
			}
		}
	}
	for (size_t k = 0; k < w.loaded.size() && (g_renderParts & RENDER_LAYERS); k++)
		draws += drawBatches(w.loaded[k]->cell, w.loaded[k]->cell.batches, 1, &visible[k], time);

	// Swinging doors: object-space meshes with the door's placement and swing angle
	for (LoadedCell* l : w.loaded)
		for (auto& d : l->cell.doors)
		{
			if (!(g_renderParts & RENDER_DOORS))
				break;
			const Ref& r = w.refs[d.ref];
			if (!r.visible() || (r.hasBox && !boxVisible(fr, r.boxMin, r.boxMax)))
				continue;
			C3D_Mtx model, mv;
			Mtx_Identity(&model);
			Mtx_Translate(&model, r.pos[0], r.pos[1], r.pos[2], true);
			Mtx_RotateX(&model, -r.rot[0], true);
			Mtx_RotateY(&model, -r.rot[1], true);
			Mtx_RotateZ(&model, -r.rot[2], true);
			Mtx_RotateZ(&model, -r.doorAngle, true);
			Mtx_Scale(&model, r.scale, r.scale, r.scale);
			Mtx_Multiply(&mv, &view, &model);
			setModelView(mv);
			draws += drawBatches(l->cell, d.batches, 0, nullptr, time);
			draws += drawBatches(l->cell, d.batches, 2, nullptr, time);
		}

	// Animated actors: skinned meshes are already in world space, rigid parts ride their bone.
	// Skip actors behind the camera or past the view distance; refresh deformation at 30 Hz
	// (alternating actors), and at 7.5 Hz for actors far enough that nobody can tell.
	static u32 frame = 0;
	if (!secondEye)
		frame++;
	u64 actorStart = svcGetSystemTick();
	float maxDist = fog ? fminf(fogEnd, kActorViewDistance) : 4000.0f;
	for (int pass = 0; pass < 2 && (g_renderParts & RENDER_ACTORS); pass++)
		for (LoadedCell* l : w.loaded)
			for (size_t ai = 0; ai < l->actors.actors.size(); ai++)
			{
				Actor& a = l->actors.actors[ai];
				if (a.ref < 0 || !w.refs[a.ref].visible())
					continue;
				float d[3] = { a.place[3] - cam.pos[0], a.place[7] - cam.pos[1], a.place[11] + 64.0f - cam.pos[2] };
				float dist2 = d[0] * d[0] + d[1] * d[1];
				float bmin[3] = { a.place[3] - 80.0f, a.place[7] - 80.0f, a.place[11] - 10.0f };
				float bmax[3] = { a.place[3] + 80.0f, a.place[7] + 80.0f, a.place[11] + 150.0f };
				if (!boxVisible(fr, bmin, bmax) || dist2 > maxDist * maxDist)
					continue;
				// Re-skin up close often, less often the smaller they are on screen
				if (pass == 0 && !secondEye && ((frame + ai) % actorSkinEvery(dist2) == 0 || !a.deformed))
				{
					actorDeform(l->actors, a);
					a.deformed = true;
				}
				// Far away, small pieces (rings, belts, hair strands: a few pixels) aren't worth a draw
				bool far = dist2 > kActorDetailDistance * kActorDetailDistance;
				// Skinned meshes are in actor space and share the placement; rigid ones ride their bone
				C3D_Mtx placeMv;
				{
					C3D_Mtx place;
					for (int r = 0; r < 3; r++)
						place.r[r] = FVec4_New(a.place[r * 4], a.place[r * 4 + 1], a.place[r * 4 + 2], a.place[r * 4 + 3]);
					place.r[3] = FVec4_New(0, 0, 0, 1);
					Mtx_Multiply(&placeMv, &view, &place);
				}
				for (auto& m : a.meshes)
				{
					if (((m.flags & BATCH_BLEND) != 0) != (pass == 1) || ((m.flags & ACTOR_MESH_WEAPON) && !a.showWeapon))
						continue;
					if (far && m.numIndices < kActorDetailTris * 3)
						continue;
					if (m.bone >= 0 && m.kind != ACTOR_SKINNED)
					{
						C3D_Mtx model, mv;
						actorMeshMatrix(a, m, &model);
						Mtx_Multiply(&mv, &view, &model);
						setModelView(mv);
					}
					else
						setModelView(placeMv);
					CellBatch b = { m.tex, m.flags, m.alphaRef, m.numVerts, m.numIndices, m.verts, m.indices, {}, {}, 0.0f };
					drawBatch(l->cell, b, DRAW_WORLD, time);
					draws++;
				}
			}

	g_profActorsMs += (svcGetSystemTick() - actorStart) * 1000.0f / SYSCLOCK_ARM11;

	setModelView(view);
	for (size_t k = 0; k < w.loaded.size() && (g_renderParts & RENDER_BLENDED); k++)
		draws += drawBatches(w.loaded[k]->cell, w.loaded[k]->cell.batches, 2, &visible[k], time);
	disableFog();
	setUvOffset(0.0f, 0.0f);
	return draws;
}

// ---- Local map: the interior seen straight down, rendered once into a texture in VRAM

static const int kLocalMapSize = 256;

C3D_Tex* rendererLocalMap()
{
	return s_localTarget ? &s_localTex : nullptr;
}

bool rendererDrawLocalMap(World& w, float minX, float minY, float size, float topZ, float depth)
{
	setLight(nullptr);
	if (!s_localTarget)
	{
		if (!C3D_TexInitVRAM(&s_localTex, kLocalMapSize, kLocalMapSize, GPU_RGBA8))
			return false;
		s_localTarget = C3D_RenderTargetCreateFromTex(&s_localTex, GPU_TEXFACE_2D, 0, GPU_RB_DEPTH24_STENCIL8);
		if (!s_localTarget)
		{
			C3D_TexDelete(&s_localTex);
			return false;
		}
		C3D_TexSetFilter(&s_localTex, GPU_LINEAR, GPU_LINEAR);
		C3D_TexSetWrap(&s_localTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
	}
	C3D_RenderTargetClear(s_localTarget, C3D_CLEAR_ALL, 0x0C0906FF, 0);
	C3D_FrameDrawOn(s_localTarget);
	bindPipeline();
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uTint, 1.0f, 1.0f, 1.0f, 1.0f);    // interiors: no time of day
	disableFog();
	// Looking down -Z from topZ: everything above it (upper floors) is behind the camera, and
	// ceilings face away (culled), which leaves the floor plan
	C3D_Mtx projection, view;
	Mtx_Ortho(&projection, minX, minX + size, minY, minY + size, 0.0f, depth, false);
	Mtx_Identity(&view);
	Mtx_Translate(&view, 0.0f, 0.0f, -topZ, true);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uProjection, &projection);
	setModelView(view);
	for (int pass = 0; pass < 3; pass++)
		for (LoadedCell* l : w.loaded)
			drawBatches(l->cell, l->cell.batches, pass, nullptr, w.time);
	setUvOffset(0.0f, 0.0f);
	invalidateState();
	s_mvValid = false;
	return true;
}

// ---- Extra meshes after the world: projectiles in flight, then the first-person view model

void rendererDrawMesh(const ActorMesh& m, const C3D_Mtx* model, C3D_Tex* tex)
{
	C3D_Mtx mv;
	if (model)
		Mtx_Multiply(&mv, &s_view, model);
	else
		mv = s_view;
	setModelView(mv);
	CellBatch b = { -1, m.flags, m.alphaRef, m.numVerts, m.numIndices, m.verts, m.indices, {}, {}, 0.0f };
	drawBatchTex(tex, b, DRAW_WORLD, s_time);
}

void rendererDrawActor(Actor& a, const std::vector<C3D_Tex*>& textures, bool viewModel)
{
	// The view model's depth is squeezed in front of the world, so arms never sink into walls
	if (viewModel)
		C3D_DepthMap(true, -0.1f, 0.9f);
	// In 3D the arms (a few dozen units away) get their own focus near them: with the world's they
	// would leap far out of the screen
	if (viewModel && s_eyeShift != 0.0f)
	{
		C3D_Mtx projection;
		Mtx_PerspStereoTilt(&projection, C3D_AngleFromDegrees(kFovY), C3D_AspectRatioTop, kNear, s_farPlane,
			s_eyeShift * 0.5f, kViewModelFocus, false);
		C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uProjection, &projection);
	}
	for (int pass = 0; pass < 2; pass++)
		for (auto& m : a.meshes)
		{
			if (((m.flags & BATCH_BLEND) != 0) != (pass == 1))
				continue;
			C3D_Tex* tex = m.tex >= 0 && m.tex < (int)textures.size() ? textures[m.tex] : nullptr;
			C3D_Mtx model;
			actorMeshMatrix(a, m, &model);
			if (m.glow)
			{
				// an enchanted item: its colour added over the lit texture (a free combiner stage), pulsing
				float k = 0.30f + 0.12f * sinf(osGetTime() * 0.004f);
				u32 r = (u32)(((m.glow >> 16) & 255) * k), g = (u32)(((m.glow >> 8) & 255) * k), b = (u32)((m.glow & 255) * k);
				C3D_TexEnv* env = C3D_GetTexEnv(2);
				C3D_TexEnvInit(env);
				C3D_TexEnvSrc(env, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT);
				C3D_TexEnvFunc(env, C3D_RGB, GPU_ADD);
				C3D_TexEnvSrc(env, C3D_Alpha, GPU_PREVIOUS);
				C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
				C3D_TexEnvColor(env, 0xFF000000 | (b << 16) | (g << 8) | r);
				rendererDrawMesh(m, &model, tex);
				C3D_TexEnvInit(C3D_GetTexEnv(2));
			}
			else
				rendererDrawMesh(m, &model, tex);
		}
	if (viewModel)
	{
		C3D_DepthMap(true, -1.0f, 0.0f);
		if (s_eyeShift != 0.0f)
			C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uProjection, &s_projection);
	}
	setModelView(s_view);
}

// Untextured camera-facing quad (spell bolts): size in world units, rgba 0xAABBGGRR
void rendererDrawGlow(const float pos[3], float size, u32 rgba)
{
	// Each glow of a frame needs its own vertices: the GPU reads them after the frame is built
	static const int kMaxGlows = 64;              // both eyes' glows
	static u8* ring = nullptr;
	static u16* indices = nullptr;
	if (!ring)
	{
		ring = (u8*)lockedLinearAlloc(kMaxGlows * 4 * 24);
		indices = (u16*)lockedLinearAlloc(6 * 2);
		static const u16 idx[6] = { 0, 1, 2, 0, 2, 3 };
		memcpy(indices, idx, sizeof(idx));
		GSPGPU_FlushDataCache(indices, sizeof(idx));
	}
	if (!ring || s_glowCount >= kMaxGlows)
		return;
	void* verts = ring + (s_glowCount++) * 4 * 24;
	// Billboard: the view matrix's rotation rows are the camera axes in world space
	float right[3] = { s_view.r[0].x, s_view.r[0].y, s_view.r[0].z };
	float up[3] = { s_view.r[1].x, s_view.r[1].y, s_view.r[1].z };
	static const float corner[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
	u8* v = (u8*)verts;
	for (int i = 0; i < 4; i++)
	{
		float* p = (float*)(v + i * 24);
		for (int k = 0; k < 3; k++)
			p[k] = pos[k] + (right[k] * corner[i][0] + up[k] * corner[i][1]) * size;
		p[3] = p[4] = 0.0f;
		memcpy(v + i * 24 + 20, &rgba, 4);
	}
	GSPGPU_FlushDataCache(verts, 4 * 24);
	ActorMesh m = {};
	m.flags = BATCH_BLEND | BATCH_ADDITIVE | BATCH_TWO_SIDED;
	m.numVerts = 4;
	m.numIndices = 6;
	m.verts = verts;
	m.indices = indices;
	rendererDrawMesh(m, nullptr, nullptr);
}
