#include "mwrender/renderingmanager.hpp"

#include <algorithm>
#include <citro3d.h>
#include <cmath>
#include <cstring>

#include "mwrender/renderhelpers.hpp"

float g_profActorsMs = 0.0f;          // debug profile accumulators, reported by main once a second
float g_profWorldMs = 0.0f;
float g_profDraw[DRAWP_COUNT];
float g_detailScale = 1.0f, g_actorDetailScale = 1.0f;
int g_drawnBatches = 0, g_culledBatches = 0;
// The frame's drawing work as sent to the GPU, both eyes (an eye replayed from the other's record counts the record's):
// draw calls and triangles. Logged each second with the console's own GPU time (engine.cpp), and priced by gpuModelMs
u32 g_frameDraws = 0, g_frameTris = 0;
u32 g_lastDraws = 0, g_lastTris = 0;         // the frame just sent (engine.cpp)
float g_lastCpuMs = 0.0f;                   // ... and its CPU time, update and drawing commands
static u32 s_replayTris = 0, s_replayDrawCalls = 0, s_recordTris = 0, s_recordDraws = 0;

static inline void countDraw(u32 indices)
{
	g_frameDraws++;
	g_frameTris += indices / 3;
}

// What a frame's drawing would cost the console's GPU, in ms: per triangle and per draw call, both eyes. A first
// estimate from one report, the Great Bazaar from its entrance along the bridge on a New 3DS (212000 triangles and 660
// draws for the two eyes): about 20 fps with the 3D slider up, its full 30 without; the log's "gpu work" beside the
// console's "gpu" ms is what to fit it to
static const float kGpuMsPerTri = 0.0002f, kGpuMsPerDraw = 0.004f;
float gpuModelMs(u32 draws, u32 tris)
{
	return tris * kGpuMsPerTri + draws * kGpuMsPerDraw;
}
int g_sharedDrawn = 0;                       // shared models' placements the last view drew (tests)
int g_worldDraws = 0;                        // draws the last view of the world made (the left eye's; tests)
int g_skippedDraws = 0;               // draws dropped because the GPU command buffer was nearly full
// Blended draws of the last frame made after a nearer blended one (OpenMW sorts them back to front: a blended surface
// behind drawn after one in front paints over it, as neither writes depth)
int g_blendOutOfOrder = 0;
int g_groundGaps = -1;                // cracks in the last saved shot (counted by the native test build; -1 here)
GpuState g_gpu;
int g_torchPerPixel = 0;                    // the last world view lit the torch per pixel (1), per vertex (0), none (-1)
int g_lampLitBatches = 0;                   // batches the last view drew with lamp light the night left alone (tests)
static const float kCaustFps = 20.0f;
static const float kCaustTiles = 2.0f;           // the 32 x 32 frame repeated twice across the item's texture
static void drawShimmer(CellBatch b, u32 rgb);
static void shimmerStage(u32 rgb);

static const float kFovY = 60.0f;      // the first-person arms' view, always (OpenMW's own first-person field of view)
// The world's: kFovY, or wider seen through a werewolf's eyes in first person (OpenMW's General_Werewolf_FOV; engine.cpp)
float g_worldFovY = kFovY;
static const float kNear = 5.0f;
static const float kInteriorFar = 12000.0f;

void rendererInit()
{
	g_gpu.dvlb = DVLB_ParseFile((u32*)cell_shbin, cell_shbin_size);
	shaderProgramInit(&g_gpu.program);
	shaderProgramSetVsh(&g_gpu.program, &g_gpu.dvlb->DVLE[0]);
	g_gpu.uProjection = shaderInstanceGetUniformLocation(g_gpu.program.vertexShader, "projection");
	g_gpu.uModelView = shaderInstanceGetUniformLocation(g_gpu.program.vertexShader, "modelView");
	g_gpu.uUvOffset = shaderInstanceGetUniformLocation(g_gpu.program.vertexShader, "uvOffset");
	g_gpu.uFogVec = shaderInstanceGetUniformLocation(g_gpu.program.vertexShader, "fogVec");
	g_gpu.uPosOffset = shaderInstanceGetUniformLocation(g_gpu.program.vertexShader, "posOffset");
	g_gpu.uPosScale = shaderInstanceGetUniformLocation(g_gpu.program.vertexShader, "posScale");
	g_gpu.uUvScale = shaderInstanceGetUniformLocation(g_gpu.program.vertexShader, "uvScale");
	g_gpu.uTint = shaderInstanceGetUniformLocation(g_gpu.program.vertexShader, "tint");
	g_gpu.uLightPos = shaderInstanceGetUniformLocation(g_gpu.program.vertexShader, "lightPos");
	g_gpu.uLightColor = shaderInstanceGetUniformLocation(g_gpu.program.vertexShader, "lightColor");
	g_gpu.uLamp = shaderInstanceGetUniformLocation(g_gpu.program.vertexShader, "lamp");
	g_gpu.uSide = shaderInstanceGetUniformLocation(g_gpu.program.vertexShader, "sideLight");
	g_gpu.uTorchVec = shaderInstanceGetUniformLocation(g_gpu.program.vertexShader, "torchVec");
	g_gpu.uAmbient = shaderInstanceGetUniformLocation(g_gpu.program.vertexShader, "ambientAdd");
	g_gpu.bGamma = shaderInstanceGetUniformLocation(g_gpu.program.vertexShader, "gammaOn");
	g_gpu.bLightOn = shaderInstanceGetUniformLocation(g_gpu.program.vertexShader, "lightOn");
	g_gpu.uStereoCurve = shaderInstanceGetUniformLocation(g_gpu.program.vertexShader, "stereoCurve");
	g_gpu.bStereoOn = shaderInstanceGetUniformLocation(g_gpu.program.vertexShader, "stereoOn");
	g_gpu.skinDvlb = DVLB_ParseFile((u32*)skin_shbin, skin_shbin_size);
	shaderProgramInit(&g_gpu.skinProgram);
	shaderProgramSetVsh(&g_gpu.skinProgram, &g_gpu.skinDvlb->DVLE[0]);
	g_gpu.uBones = shaderInstanceGetUniformLocation(g_gpu.skinProgram.vertexShader, "bones");
	// The frame's uniforms carry over between the two programs only if they sit in the same registers
	static const char* const kShared[] = { "projection", "modelView", "uvOffset", "fogVec", "posOffset", "posScale",
		"uvScale", "tint", "lightPos", "lightColor", "ambientAdd", "torchVec", "stereoCurve" };
	g_gpu.skinReady = g_gpu.uBones >= 0;
	for (const char* n : kShared)
		g_gpu.skinReady &= shaderInstanceGetUniformLocation(g_gpu.program.vertexShader, n)
			== shaderInstanceGetUniformLocation(g_gpu.skinProgram.vertexShader, n);
	g_gpu.skinReady &= g_gpu.bGamma == shaderInstanceGetUniformLocation(g_gpu.skinProgram.vertexShader, "gammaOn");
	g_gpu.skinReady &= g_gpu.bLightOn == shaderInstanceGetUniformLocation(g_gpu.skinProgram.vertexShader, "lightOn");
	g_gpu.skinReady &= g_gpu.bStereoOn == shaderInstanceGetUniformLocation(g_gpu.skinProgram.vertexShader, "stereoOn");
	if (!g_gpu.skinReady)
	{
		g_gpuSkinning = false;
		logf("renderer: skin.v.pica's uniforms don't line up with cell.v.pica's: skinning stays on the CPU");
	}
	initFogRamp();
}

void rendererExit()
{
	if (g_gpu.localTarget)
	{
		C3D_RenderTargetDelete(g_gpu.localTarget);
		C3D_TexDelete(&g_gpu.localTex);
		g_gpu.localTarget = nullptr;
	}
	C3D_TexDelete(&g_gpu.fogRamp);
	for (int i = 0; i < g_gpu.caustCount; i++)
		C3D_TexDelete(&g_gpu.caust[i]);
	g_gpu.caustCount = 0;
	for (int i = 0; i < g_gpu.waterCount; i++)
		C3D_TexDelete(&g_gpu.water[i]);
	g_gpu.waterCount = 0;
	shaderProgramFree(&g_gpu.program);
	DVLB_Free(g_gpu.dvlb);
	shaderProgramFree(&g_gpu.skinProgram);
	DVLB_Free(g_gpu.skinDvlb);
}

void uploadTransform()
{
	bool snap = g_gpu.batchPacked && memcmp(&g_gpu.mv, &g_gpu.view, sizeof(g_gpu.mv)) == 0;
	float shift[3] = { 0.0f, 0.0f, 0.0f };
	if (snap)
		memcpy(shift, g_gpu.eyeSnap, sizeof(shift));
	C3D_Mtx m = g_gpu.mv;
	for (int i = 0; i < 4; i++)
		m.r[i].w = g_gpu.mv.r[i].x * shift[0] + g_gpu.mv.r[i].y * shift[1] + g_gpu.mv.r[i].z * shift[2] + g_gpu.mv.r[i].w;
	float off[3] = { g_gpu.batchOffset[0] - shift[0], g_gpu.batchOffset[1] - shift[1], g_gpu.batchOffset[2] - shift[2] };
	if (g_gpu.mvValid && memcmp(&m, &g_gpu.boundMv, sizeof(m)) == 0 && memcmp(off, g_gpu.boundOffset, sizeof(off)) == 0)
		return;
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_gpu.uModelView, &m);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, g_gpu.uPosOffset, off[0], off[1], off[2], 0.0f);
	g_gpu.boundMv = m;
	memcpy(g_gpu.boundOffset, off, sizeof(off));
	g_gpu.mvValid = true;
}

// Stereo 3D's far depth as rendererSetStereoCurve set it, from this log2 of the knee (the sky's: -1000, all of it)
static void setStereoCurve(float kneeLog)
{
	if (g_gpu.uStereoCurve < 0 || g_gpu.bStereoOn < 0)
		return;
	C3D_BoolUnifSet(GPU_VERTEX_SHADER, g_gpu.bStereoOn, g_gpu.stereoCurveOn);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, g_gpu.uStereoCurve, g_gpu.stereoCurve[0], kneeLog, g_gpu.stereoCurve[2], 0.0f);
}

void bindPipeline()
{
	invalidateState();
	g_gpu.mvValid = false;
	// citro2d changes the program and vertex layout, so set ours up every frame
	C3D_BindProgram(&g_gpu.program);
	g_gpu.formatValid = false;      // citro2d changed the layout: the first batch sets ours up again
	for (int i = 1; i < 6; i++)
		C3D_TexEnvInit(C3D_GetTexEnv(i));
	C3D_FVUnifSet(GPU_VERTEX_SHADER, g_gpu.uUvOffset, 0.0f, 0.0f, 0.0f, 0.0f);
	g_gpu.uvOffset[0] = g_gpu.uvOffset[1] = 0.0f;
	// Colour tint: the time of day's outdoors, none indoors (uniforms start at 0: always set)
	const float* t = g_gpu.daylight ? g_gpu.land : nullptr;
	C3D_FVUnifSet(GPU_VERTEX_SHADER, g_gpu.uTint, t ? t[0] : 1.0f, t ? t[1] : 1.0f, t ? t[2] : 1.0f, 1.0f);
	if (g_gpu.uAmbient >= 0)
		C3D_FVUnifSet(GPU_VERTEX_SHADER, g_gpu.uAmbient, 0.0f, 0.0f, 0.0f, g_gpu.invGamma);     // (the world's draws set it: setLight)
	if (g_gpu.bGamma >= 0)
		C3D_BoolUnifSet(GPU_VERTEX_SHADER, g_gpu.bGamma, g_gpu.gammaOn);
	if (g_gpu.uTorchVec >= 0)
		C3D_FVUnifSet(GPU_VERTEX_SHADER, g_gpu.uTorchVec, 0.0f, 0.0f, 0.0f, 1.0f);     // (no torch: past its reach everywhere)
	g_gpu.torchStageOn = false;
	setStereoCurve(g_gpu.stereoCurve[1]);
}

void rendererAlphaScale(float a)
{
	// the alpha stage (after the fog and the glow): alpha x this, colour as it is
	C3D_TexEnv* env = C3D_GetTexEnv(kTevAlpha);
	C3D_TexEnvInit(env);
	if (a >= 0.999f)
		return;
	C3D_TexEnvSrc(env, C3D_RGB, GPU_PREVIOUS);
	C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
	C3D_TexEnvSrc(env, C3D_Alpha, GPU_PREVIOUS, GPU_CONSTANT);
	C3D_TexEnvFunc(env, C3D_Alpha, GPU_MODULATE);
	u32 al = (u32)(fminf(1.0f, fmaxf(0.0f, a)) * 255.0f + 0.5f);
	C3D_TexEnvColor(env, al << 24 | 0xFFFFFF);
}

void rendererUvOverride(bool on, float u, float v)
{
	g_gpu.uvOverrideOn = on;
	g_gpu.uvOverride[0] = u;
	g_gpu.uvOverride[1] = v;
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
	float tanV = tanf(C3D_AngleFromDegrees(g_worldFovY) * 0.5f) * 1.15f;
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

static C3D_Tex* batchTexture(Cell& cell, const CellBatch& b)
{
	return b.tex >= 0 && b.tex < (int)cell.textures.size() ? cell.textures[b.tex] : nullptr;
}

// An enchanted actor mesh shimmers: its colour added over the lit texture in the free combiner stage, pulsing (no
// second draw); glowStage(0) puts the stage back
static void glowStage(u32 glow)
{
	if (!glow)
	{
		C3D_TexEnvInit(C3D_GetTexEnv(kTevGlow));
		return;
	}
	float k = 0.30f + 0.12f * sinf(osGetTime() * 0.004f);
	u32 r = (u32)(((glow >> 16) & 255) * k), g = (u32)(((glow >> 8) & 255) * k), b = (u32)((glow & 255) * k);
	C3D_TexEnv* env = C3D_GetTexEnv(kTevGlow);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT);
	C3D_TexEnvFunc(env, C3D_RGB, GPU_ADD);
	C3D_TexEnvSrc(env, C3D_Alpha, GPU_PREVIOUS);
	C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
	C3D_TexEnvColor(env, 0xFF000000 | (b << 16) | (g << 8) | r);
}

// Depth is written by every batch but a blended one (drawn back to front after the rest). One blended and alpha tested
// too writes it, as OpenMW keeps a blended model's depth writes on (unless its NiZBufferProperty turns them off): what
// the test throws away writes nothing, and the rest hides what is behind it in the same model. Bloodmoon's bears' heads
// are blended and tested (the fur at their edges); without depth, the inside of the mouth, drawn after the snout, showed
// through it (teeth where the nose is) and the head looked torn open
bool batchWritesDepth(u8 flags)
{
	return !(flags & BATCH_BLEND) || (flags & BATCH_TEST);
}

static void setBatchState(C3D_Tex* tex, const CellBatch& b, DrawMode mode, float time)
{
	if (g_gpu.last.valid && g_gpu.last.tex == tex && g_gpu.last.flags == b.flags && g_gpu.last.alphaRef == b.alphaRef
		&& g_gpu.last.mode == mode && g_gpu.last.scrollU == b.scrollU && g_gpu.last.scrollV == b.scrollV)
		return;
	g_gpu.last = { true, tex, b.flags, b.alphaRef, (int)mode, b.scrollU, b.scrollV };
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
		C3D_DepthTest(true, GPU_GREATER, batchWritesDepth(b.flags) ? GPU_WRITE_ALL : GPU_WRITE_COLOR);
	C3D_AlphaTest((b.flags & BATCH_TEST) != 0, GPU_GREATER, b.alphaRef);
	C3D_CullFace((b.flags & BATCH_TWO_SIDED) ? GPU_CULL_NONE : GPU_CULL_BACK_CCW);

	if (b.flipFrames && b.flipCols && b.flipRows)
	{
		// A world spray's looping flipbook: the frame from world time, a cell of its sheet (rows down from the top)
		int f = (int)(time * b.flipFps) % b.flipFrames;
		setUvOffset((float)(f % b.flipCols) / b.flipCols, -(float)(f / b.flipCols) / b.flipRows);
	}
	else if (b.scrollU != 0.0f || b.scrollV != 0.0f)
	{
		// A model's own sliding texture (NiUVController: a waterfall's water falling), repeats a second
		setUvOffset(fmodf(time * b.scrollU, 1.0f), fmodf(time * b.scrollV, 1.0f));
	}
	else if ((b.flags & BATCH_SCROLL) && mode != DRAW_SKY && g_gpu.waterCount)
		setUvOffset(0.0f, 0.0f);             // (the water's frames move it: drawBatchTex picks the frame)
	else if (b.flags & BATCH_SCROLL)
	{
		// Clouds drift slowly overhead; water moves faster (Morrowind plays 32 pictures of it at 12 a second, here one
		// picture slides: at a fiftieth of a repeat a second it looked still)
		float speed = mode == DRAW_SKY ? 0.006f : kWaterScroll;
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

	if (g_gpu.fogOn)
		C3D_TexEnvColor(C3D_GetTexEnv(kTevFog), (b.flags & BATCH_ADDITIVE) ? 0xFF000000 : g_gpu.fogColor);

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
	// The torch per pixel: texture x the ramp's light added (the light alone on an untextured batch); alpha as it is
	env = C3D_GetTexEnv(kTevTorch);
	C3D_TexEnvInit(env);
	if (g_gpu.torchStageOn && mode != DRAW_SKY)
	{
		if (tex)
		{
			C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_TEXTURE1, GPU_PREVIOUS);
			C3D_TexEnvFunc(env, C3D_RGB, GPU_MULTIPLY_ADD);
		}
		else
		{
			C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE1, GPU_PREVIOUS);
			C3D_TexEnvFunc(env, C3D_RGB, GPU_ADD);
		}
		C3D_TexEnvSrc(env, C3D_Alpha, GPU_PREVIOUS);
		C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
	}
}

// Vertex layout and quantization uniforms: packed s16 (cell format 5) or float (actors, older cells)
static void bindVertexFormat(const CellBatch& b)
{
	bool packed = b.stride == CELL_VERTEX_SIZE_PACKED || b.stride == CELL_VERTEX_SIZE_SHARED;
	if (!g_gpu.formatValid || packed != g_gpu.packedBound)
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
		g_gpu.packedBound = packed;
	}
	float q[8] = { b.posOffset[0], b.posOffset[1], b.posOffset[2], b.posScale[0], b.posScale[1], b.posScale[2],
		b.uvScale[0], b.uvScale[1] };
	if (!g_gpu.formatValid || memcmp(q, g_gpu.boundQuant, sizeof(q)) != 0)
	{
		// The offset goes up with the model-view (uploadTransform)
		C3D_FVUnifSet(GPU_VERTEX_SHADER, g_gpu.uPosScale, q[3], q[4], q[5], 0.0f);
		C3D_FVUnifSet(GPU_VERTEX_SHADER, g_gpu.uUvScale, q[6], q[7], 1.0f, 1.0f);
		memcpy(g_gpu.boundQuant, q, sizeof(q));
	}
	if (memcmp(g_gpu.batchOffset, q, sizeof(g_gpu.batchOffset)) != 0 || g_gpu.batchPacked != packed)
	{
		memcpy(g_gpu.batchOffset, q, sizeof(g_gpu.batchOffset));
		g_gpu.batchPacked = packed;
		uploadTransform();
	}
	g_gpu.formatValid = true;
}

// Overflowing the command buffer is a panic (a freeze on hardware): past this share the world
// stops drawing, leaving room for the HUD and the bottom screen
static const float kCmdBufLimit = 0.9f;

void drawBatchTex(C3D_Tex* tex, const CellBatch& b, DrawMode mode, float time)
{
	// A draw of no triangles hangs the PICA on hardware (emulators skip it)
	if (b.numIndices < 3 || !b.verts || !b.indices)
		return;
	if (C3D_GetCmdBufUsage() > kCmdBufLimit)
	{
		g_skippedDraws++;
		return;
	}
	// Water plays Morrowind's surface frames ([Water] SurfaceTexture, SurfaceFrameCount at SurfaceFPS, as OpenMW's
	// simple water): the ripples change shape. One picture sliding looked still on the console
	if ((b.flags & BATCH_SCROLL) && mode != DRAW_SKY && g_gpu.waterCount)
		tex = &g_gpu.water[(int)(fmaxf(0.0f, time) * g_gpu.waterFps) % g_gpu.waterCount];
	setLamp(b);
	// (the world map's baked cells: the sea drawn opaque, one even sea whatever lies under it)
	if (g_gpu.opaqueWater && (b.flags & BATCH_SCROLL) && mode != DRAW_SKY)
	{
		CellBatch flat = b;
		flat.flags &= ~(BATCH_BLEND | BATCH_ADDITIVE);
		setBatchState(tex, flat, mode, time);
	}
	else
		setBatchState(tex, b, mode, time);
	if (g_gpu.uvOverrideOn)
		setUvOffset(g_gpu.uvOverride[0], g_gpu.uvOverride[1]);
	bindVertexFormat(b);
	C3D_BufInfo* bufInfo = C3D_GetBufInfo();
	BufInfo_Init(bufInfo);
	if (b.colors)
	{
		// A shared model's batch: position and texture coordinates from the model, the colour from the placement drawn
		BufInfo_Add(bufInfo, b.verts, b.stride, 2, 0x10);
		BufInfo_Add(bufInfo, b.colors, 4, 1, 0x2);
	}
	else
		BufInfo_Add(bufInfo, b.verts, b.stride, 3, 0x210);
	countDraw(b.numIndices);
	C3D_DrawElements(GPU_TRIANGLES, b.numIndices, C3D_UNSIGNED_SHORT, b.indices);
	if (g_gpu.uvOverrideOn)
		invalidateState();                          // (the next batch sets its own offset again)
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

// An actor's mesh as a batch: its flags' 128 is the weapon's (ACTOR_MESH_WEAPON), not BATCH_RISE, which a torch's fire
// and smoke take from ActorMesh::rise
static u8 actorBatchFlags(const ActorMesh& m)
{
	return (u8)((m.flags & ~ACTOR_MESH_WEAPON) | (m.rise ? BATCH_RISE : 0));
}

static int drawSkinQueue(float time)
{
	if (g_gpu.skinQueue.empty())
		return 0;
	int draws = 0;
	static std::vector<float> pal(kSkinBones * 12);
	C3D_BindProgram(&g_gpu.skinProgram);
	bool layoutSet = false;
	for (auto& d : g_gpu.skinQueue)
	{
		const ActorMesh& m = *d.m;
		if (m.numIndices < 3 || !m.verts || !m.indices)
			continue;
		if (C3D_GetCmdBufUsage() > kCmdBufLimit)
		{
			g_skippedDraws++;
			continue;
		}
		CellBatch b = { m.tex, actorBatchFlags(m), m.alphaRef, m.numVerts, m.numIndices, m.verts, m.indices, {}, {}, 0.0f };
		for (int k = 0; k < 3; k++)
			b.posScale[k] = m.posQ;
		b.uvScale[0] = b.uvScale[1] = m.uvQ;
		b.stride = CELL_VERTEX_SIZE_PACKED;          // (the scales go up as for a packed cell batch)
		bool ghost = d.alpha < 0.999f;
		if (ghost)
			b.flags |= BATCH_BLEND;
		setModelView(d.mv);
		setTint(d.tint);
		setBatchState(batchTexture(*d.cell, b), b, DRAW_WORLD, time);
		// The skin's vertex layout, once for the queue: bindVertexFormat takes it for its own packed one and sends only
		// the quantization (the queue's end tells it otherwise), unless something set another layout in between
		bool layoutLost = !layoutSet || !g_gpu.formatValid || !g_gpu.packedBound;
		bindVertexFormat(b);
		if (layoutLost)
		{
			C3D_AttrInfo* attrInfo = C3D_GetAttrInfo();
			AttrInfo_Init(attrInfo);
			AttrInfo_AddLoader(attrInfo, 0, GPU_SHORT, 4);            // v0 = position
			AttrInfo_AddLoader(attrInfo, 1, GPU_SHORT, 2);            // v1 = texcoord
			AttrInfo_AddLoader(attrInfo, 2, GPU_UNSIGNED_BYTE, 4);    // v2 = colour
			AttrInfo_AddLoader(attrInfo, 3, GPU_UNSIGNED_BYTE, 4);    // v3 = palette entries
			AttrInfo_AddLoader(attrInfo, 4, GPU_UNSIGNED_BYTE, 4);    // v4 = weights
			layoutSet = true;
		}
		actorSkinPalette(*d.a, m, pal.data());
		// (the palette's rows written in one go: three a bone)
		int rows = (int)m.palBone.size() * 3;
		C3D_FVec* bones = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, g_gpu.uBones, rows);
		for (int r = 0; r < rows; r++)
			bones[r] = FVec4_New(pal[r * 4], pal[r * 4 + 1], pal[r * 4 + 2], pal[r * 4 + 3]);
		C3D_BufInfo* bufInfo = C3D_GetBufInfo();
		BufInfo_Init(bufInfo);
		BufInfo_Add(bufInfo, m.verts, 24, 5, 0x43210);
		bool flat = m.glow && !g_gpu.caustCount;
		if (flat)
			glowStage(m.glow);
		if (ghost)
			rendererAlphaScale(d.alpha);
		countDraw(m.numIndices);
		C3D_DrawElements(GPU_TRIANGLES, m.numIndices, C3D_UNSIGNED_SHORT, m.indices);
		if (ghost)
			rendererAlphaScale(1.0f);
		if (flat)
			glowStage(0);
		draws++;
		if (m.glow && g_gpu.caustCount && !ghost)
		{
			// Enchanted armour and clothing shimmer as OpenMW's glow does: the caustic frames in the enchantment's
			// colour added over the item (a second pass, as for a weapon). The colour added flat over the whole item
			// turned a dark one solid blue (the Devil Cephalopod Helm on Baladas Demnevanni)
			CellBatch sb = b;
			sb.flags = (b.flags & BATCH_TWO_SIDED) | BATCH_BLEND | BATCH_ADDITIVE | BATCH_DECAL;
			sb.alphaRef = 0;
			setBatchState(&g_gpu.caust[(int)(time * kCaustFps) % g_gpu.caustCount], sb, DRAW_WORLD, time);
			C3D_FVUnifSet(GPU_VERTEX_SHADER, g_gpu.uUvScale, m.uvQ * kCaustTiles, m.uvQ * kCaustTiles, 1.0f, 1.0f);
			shimmerStage(m.glow);
			countDraw(m.numIndices);
			C3D_DrawElements(GPU_TRIANGLES, m.numIndices, C3D_UNSIGNED_SHORT, m.indices);
			C3D_TexEnvInit(C3D_GetTexEnv(kTevGlow));
			invalidateState();
			draws++;
		}
	}
	g_gpu.skinQueue.clear();
	C3D_BindProgram(&g_gpu.program);
	g_gpu.formatValid = false;
	g_gpu.lampValid = false;                // (the palette wrote over shaders/cell.v.pica's lamp register)
	setTint(worldTint());
	return draws;
}

static int drawBlendQueue(float time)
{
	std::stable_sort(g_gpu.blendQueue.begin(), g_gpu.blendQueue.end(), [](const BlendDraw& x, const BlendDraw& y) { return x.d2 > y.d2; });
	int draws = 0;
	float last = 1e30f;
	g_blendOutOfOrder = 0;
	for (const BlendDraw& bd : g_gpu.blendQueue)
	{
		g_blendOutOfOrder += bd.d2 > last * 1.0001f ? 1 : 0;
		last = bd.d2;
		if (bd.skinned)
		{
			// (a blended skinned mesh: drawn with shaders/skin.v.pica on its own, in its place in the order)
			SkinDraw sd = { bd.cell, bd.skinned, bd.mesh, bd.mv, bd.alpha };
			memcpy(sd.tint, bd.tint, sizeof(sd.tint));
			g_gpu.skinQueue.push_back(sd);
			draws += drawSkinQueue(time);
			continue;
		}
		setModelView(bd.mv);
		setTint(bd.tint);
		if (bd.batch && bd.colors)
		{
			CellBatch b = *bd.batch;
			b.colors = bd.colors;
			drawBatch(*bd.cell, b, DRAW_WORLD, time);
		}
		else if (bd.batch)
			drawBatch(*bd.cell, *bd.batch, DRAW_WORLD, time);
		else
		{
			const ActorMesh& m = *bd.mesh;
			CellBatch b = { m.tex, actorBatchFlags(m), m.alphaRef, m.numVerts, m.numIndices, m.verts, m.indices, {}, {}, 0.0f };
			bool ghost = bd.alpha < 0.999f;
			if (ghost)
			{
				b.flags |= BATCH_BLEND;
				rendererAlphaScale(bd.alpha);
			}
			bool flat = m.glow && !g_gpu.caustCount;
			if (flat)
				glowStage(m.glow);         // (enchanted armour and clothing: flat only without the caustic frames)
			drawBatch(*bd.cell, b, DRAW_WORLD, time);
			if (ghost)
				rendererAlphaScale(1.0f);
			if (flat)
				glowStage(0);
			else if (m.glow && !ghost)
			{
				drawShimmer(b, m.glow);
				draws++;
			}
			if (bd.shimmer)
			{
				drawShimmer(b, bd.shimmer);
				draws++;
			}
		}
		draws++;
	}
	g_gpu.blendQueue.clear();
	setTint(worldTint());
	return draws;
}

int drawBatches(Cell& cell, std::vector<CellBatch>& batches, int pass, const std::vector<u8>* visible, float time)
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

// A shared model's placement in the view: view x its model -> world
void sharedPlaceView(const C3D_Mtx& view, const SharedPlace& p, C3D_Mtx* out)
{
	C3D_Mtx model;
	for (int r = 0; r < 3; r++)
		model.r[r] = FVec4_New(p.m[r * 4], p.m[r * 4 + 1], p.m[r * 4 + 2], p.m[r * 4 + 3]);
	model.r[3] = FVec4_New(0.0f, 0.0f, 0.0f, 1.0f);
	Mtx_Multiply(out, &view, &model);
}

static const void* placeColors(const Cell& cell, const SharedPlace& p, const CellBatch& b)
{
	const SharedModel& sm = cell.models[p.model];
	size_t set = sm.sets == 1 ? 0 : (size_t)(&p - &cell.places[sm.firstPlace]);
	return sm.colors + (set * sm.numVerts + b.colorAt) * 4;
}

// Every placement of the cell's shared models, the batches of one pass (the local map: no culling; a disabled one left
// out); leaves the view's transform bound
int drawSharedPass(Cell& cell, int pass, const C3D_Mtx& view, float time, const std::deque<Ref>& refs)
{
	int draws = 0;
	for (const SharedPlace& p : cell.places)
	{
		if (p.ref >= 0 && p.ref < (int)refs.size() && !refs[p.ref].visible())
			continue;
		const SharedModel& sm = cell.models[p.model];
		bool any = false;
		for (u32 j = sm.firstBatch; j < sm.firstBatch + sm.numBatches; j++)
		{
			const CellBatch& src = cell.shared[j];
			if (batchPass(src) != pass || !sm.colors)
				continue;
			if (!any)
			{
				C3D_Mtx mv;
				sharedPlaceView(view, p, &mv);
				setModelView(mv);
				any = true;
			}
			CellBatch b = src;
			b.colors = placeColors(cell, p, src);
			drawBatch(cell, b, DRAW_WORLD, time);
			draws++;
		}
	}
	setModelView(view);
	return draws;
}

// Distance at which the stereo image sits on the screen surface (world units, ~1.7 m): nearer
// things come out of the screen, the rest is behind it
static const float kStereoFocus = 120.0f;
// ... and for the first-person arms and weapon, drawn with half the eye separation
static const float kViewModelFocus = 30.0f;
// Past this distance (world units, ~8 m: where a plain stereo view has spent 80% of its depth behind the screen)
// each doubling of the distance adds depth (shaders/cell.v.pica stereoCurve), at most kStereoDoublings of them
// (to ~270 m); nearer things are as the plain view draws them
static const float kStereoKnee = 600.0f;
static const float kStereoDoublings = 5.0f;
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
void rendererSetStereoCurve(float share)
{
	g_gpu.stereoCurveOn = share > 0.0f;
	g_gpu.stereoCurve[0] = -share;       // (the eye's offset at infinity is -projection.z)
	g_gpu.stereoCurve[1] = log2f(kStereoKnee);
	g_gpu.stereoCurve[2] = kStereoDoublings;
}

float rendererStereoPixels(float depth, float eyeShift)
{
	if (eyeShift == 0.0f || depth <= kNear)
		return 0.0f;
	C3D_Mtx flat, eye;
	Mtx_PerspTilt(&flat, C3D_AngleFromDegrees(g_worldFovY), C3D_AspectRatioTop, kNear, depth * 2.0f, false);
	Mtx_PerspStereoTilt(&eye, C3D_AngleFromDegrees(g_worldFovY), C3D_AspectRatioTop, kNear, depth * 2.0f, eyeShift,
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
	// (and the far depth the shaders add: the same doublings past the knee)
	if (g_gpu.stereoCurveOn)
	{
		float doublings = fminf(kStereoDoublings, fmaxf(0.0f, log2f(depth) - g_gpu.stereoCurve[1]));
		seen[0] += g_gpu.stereoCurve[0] * doublings * eye.r[0].z;
		seen[1] += g_gpu.stereoCurve[0] * doublings * eye.r[1].z;
	}
	int axis = fabsf(right[0] - centre[0]) > fabsf(right[1] - centre[1]) ? 0 : 1;
	float sign = right[axis] > centre[axis] ? 1.0f : -1.0f;
	return (seen[axis] - centre[axis]) * sign * 200.0f;
}

// ---- Stereo: the right eye from the left eye's commands
//
// The world is built once a frame into the GPU's command list. In 3D the right eye needs the same draws with
// another projection: instead of building them again on the CPU (the frame's biggest cost on the console), the
// left eye's words are copied raw into the list for the right eye. Three things make the copy land right:
// - the GPU's state the copied words start from must be the one the left eye started from: both eyes start from
//   the same primed state (bindPipeline, fog off, no light, the lamp and layout caches cleared), and the left eye
//   ends by priming it again, so the words after the copy stand on that state too
// - the projection, the one thing that differs, is written before the copy and never inside it: the primed
//   state is flushed into the list (a draw of nothing: dirty uniforms and registers go out at a draw) before the
//   record's first word
// - the right eye's framebuffer goes out in that same flush
// The copy holds no CPU work (vertex rings written by the left eye are read again as they are) and no list
// split (clears and target changes come before the record). A frame whose list can't hold a second copy
// draws the right eye the old way
// (the record as addresses: libctru moves the list's base and zeroes its offset at every split, such as the right
// eye's clear and target change, so offsets taken before them don't hold)
static const u32* s_replayFrom = nullptr;
static u32 s_replayWords = 0;
static int s_replayDraws = 0;
int g_replayCount = 0, g_replayMissed = 0;
static bool s_replayOn = false;

// A draw of nothing: citro3d writes the dirty uniforms and registers before any draw, so the list holds the
// primed state before the words that follow (the layout is set for it and left for the next batch to set again)
static void flushState()
{
	C3D_AttrInfo* attrInfo = C3D_GetAttrInfo();
	AttrInfo_Init(attrInfo);
	for (int i = 0; i < 3; i++)
		AttrInfo_AddLoader(attrInfo, i, GPU_FLOAT, 4);
	C3D_ImmDrawBegin(GPU_TRIANGLES);
	for (int v = 0; v < 3; v++)
		for (int i = 0; i < 3; i++)
			C3D_ImmSendAttrib(0.0f, 0.0f, 0.0f, 1.0f);
	C3D_ImmDrawEnd();
	g_gpu.formatValid = false;
}

// The state both eyes start from (and the left one ends on), with this eye's projection
static void primeState(const C3D_Mtx& projection)
{
	bindPipeline();
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_gpu.uProjection, &projection);
	disableFog();
	setLight(nullptr);
	g_gpu.lampValid = false;
	g_gpu.mvValid = false;
	flushState();
}

static void replayStart()
{
	s_replayOn = false;
	primeState(g_gpu.projection);
	u32* buf = nullptr;
	u32 size = 0, at = 0;
	GPUCMD_GetBuffer(&buf, &size, &at);
	s_replayFrom = buf ? buf + at : nullptr;
	s_recordTris = g_frameTris;
	s_recordDraws = g_frameDraws;
}

void rendererReplayEnd(int draws)
{
	primeState(g_gpu.projection);
	u32* buf = nullptr;
	u32 size = 0, at = 0;
	GPUCMD_GetBuffer(&buf, &size, &at);
	s_replayWords = buf && s_replayFrom && buf + at > s_replayFrom ? (u32)(buf + at - s_replayFrom) : 0;
	s_replayDraws = draws;
	s_replayTris = g_frameTris - s_recordTris;
	s_replayDrawCalls = g_frameDraws - s_recordDraws;
	s_replayOn = s_replayWords > 0;
}

int rendererReplayWorld(float eyeShift)
{
	if (!s_replayOn)
		return -1;
	u32* buf = nullptr;
	u32 size = 0, at = 0;
	GPUCMD_GetBuffer(&buf, &size, &at);
	u32 words = s_replayWords;
	// (room for the copy and the HUDs after it: the renderer stops drawing past kCmdBufLimit of the list)
	if (!buf || at + words + 4096 > (u32)(size * kCmdBufLimit))
	{
		g_replayMissed++;
		return -1;
	}
	g_replayCount++;
	C3D_Mtx projection;
	if (eyeShift != 0.0f)
		Mtx_PerspStereoTilt(&projection, C3D_AngleFromDegrees(g_worldFovY), C3D_AspectRatioTop, kNear, g_gpu.farPlane,
			eyeShift, kStereoFocus, false);
	else
		Mtx_PerspTilt(&projection, C3D_AngleFromDegrees(g_worldFovY), C3D_AspectRatioTop, kNear, g_gpu.farPlane, false);
	g_gpu.projection = projection;
	g_gpu.eyeShift = eyeShift;
	primeState(projection);
	GPUCMD_AddRawCommands(s_replayFrom, words);
	g_frameTris += s_replayTris;
	g_frameDraws += s_replayDrawCalls;
	return s_replayDraws;
}

int rendererDrawWorld(World& w, const RenderCamera& cam, float eyeShift, bool secondEye, float fogScale)
{
	g_gpu.spritesLeft = g_profile.spriteBudget;
	if (!secondEye)
		g_lampLitBatches = 0;
	MARK("draw world");
	Cell& here = w.here();
	bindPipeline();
	u64 drawSince = svcGetSystemTick();
	bool fog = here.fogEnd > 0.0f;
	const float viewEnd = here.fogEnd;
	float fogStart = here.fogStart * fogScale, fogEnd = viewEnd * fogScale;
	// (outdoors the weather says where the fog starts, as OpenMW: a storm's from the eye)
	if (fog && here.underSky() && w.weatherFogStart() > -1.5f)
	{
		// (at OpenMW's distance: its view distance x (1 - the weather's land fog depth), 2200 on a clear day, closing in
		// with our view; ours reaches farther, and the fog spread over it left far things crisper than OpenMW's)
		fogStart = fminf(fogEnd, kInteriorFogView * fogScale) * w.weatherFogStart();
	}
	if (fog && g_gpu.underwaterOn)
		fogStart = fogEnd * (1.0f - g_gpu.underwaterFog);
	// (outdoors: the distant land's reach, at least the detailed view's)
	float horizonEnd = fog && !(here.flags & CELL_INTERIOR) ? fmaxf(fogEnd, viewEnd * g_gpu.horizonScale) : fogEnd;
	g_gpu.lastFog[0] = fog ? fogStart : 0.0f;
	g_gpu.lastFog[1] = fog ? fogEnd : 0.0f;
	float farPlane = fog ? horizonEnd + 500.0f : kInteriorFar;
	float time = w.time;

	C3D_Mtx projection, view;
	if (eyeShift != 0.0f)
		Mtx_PerspStereoTilt(&projection, C3D_AngleFromDegrees(g_worldFovY), C3D_AspectRatioTop, kNear, farPlane,
			eyeShift, kStereoFocus, false);
	else
		Mtx_PerspTilt(&projection, C3D_AngleFromDegrees(g_worldFovY), C3D_AspectRatioTop, kNear, farPlane, false);
	float cp = cosf(cam.pitch);
	C3D_FVec eye = FVec3_New(cam.pos[0], cam.pos[1], cam.pos[2]);
	C3D_FVec target = FVec3_New(cam.pos[0] + sinf(cam.yaw) * cp, cam.pos[1] + cosf(cam.yaw) * cp,
		cam.pos[2] + sinf(cam.pitch));
	Mtx_LookAt(&view, eye, target, FVec3_New(0.0f, 0.0f, 1.0f), false);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_gpu.uProjection, &projection);
	g_gpu.projection = projection;
	g_gpu.eyeShift = eyeShift;
	g_gpu.farPlane = farPlane;
	g_gpu.view = view;
	for (int k = 0; k < 3; k++)
	{
		g_gpu.eyeSnap[k] = floorf(cam.pos[k] / 64.0f) * 64.0f;
		g_gpu.eye[k] = cam.pos[k];
	}
	g_gpu.mvValid = false;
	g_gpu.time = time;
	if (!secondEye)
	{
		g_gpu.glowCount = 0;              // the second eye's glows get their own vertices
		g_gpu.spriteSlot = 0;             // (the frame's sprites from the ring's start: both eyes' fit it whole)
	}

	int draws = 0;
	disableFog();
	setLight(nullptr);
	// The left eye's world is recorded from here (rendererReplayWorld draws the right eye from the record)
	if (!secondEye)
		replayStart();
	static const float kOne[3] = { 1, 1, 1 };

	// Sky: centred on the camera (rotation only), no depth, no fog. Not from under water: the water's colour fills what
	// the fog hides (the clear colour), as OpenMW's underwater fog does
	if (!here.sky.empty() && (g_renderParts & RENDER_SKY) && !g_gpu.underwaterOn)
	{
		C3D_Mtx skyView = view;
		skyView.r[0].w = skyView.r[1].w = skyView.r[2].w = 0.0f;
		setModelView(skyView);
		// (in 3D the sky is farther than anything: all of the far depth, whatever its dome's radius)
		setStereoCurve(-1000.0f);
		// (white fog and sky domes, then the stars, sun and moons, then clouds: the domes take the hour's weather
		// colours, the fog's at the horizon and the sky's overhead, as OpenMW draws its sky; an older cell's single
		// baked gradient a tint)
		bool layered = here.sky.size() >= 2 && here.sky[1].tex < 0 && (here.sky[1].flags & BATCH_BLEND);
		bool bodiesDrawn = false;
		for (size_t k = 0; k < here.sky.size(); k++)
		{
			if (layered && k >= 2 && !bodiesDrawn)
			{
				drawSkyBodies(view, secondEye, false);
				bodiesDrawn = true;
			}
			// The fog's dome is opaque in the fog's colour, the colour the screen was cleared to outdoors (engine.cpp):
			// it would paint the screen over in the colour it already has
			if (layered && k == 0 && w.game.hasWeather)
				continue;
			if (layered && k < 2)
			{
				float c[3];
				for (int i = 0; i < 3; i++)
					c[i] = w.weatherColour(k == 0 ? 1 : 0, i);
				setTint(c);
			}
			else if (layered && g_gpu.daylight)
			{
				// Clouds: the fog's colour lifted by 0.13 (OpenMW's SkyManager::setWeather, their emission), so a sunset's
				// clouds stand out warm against its sky rather than taking the sky's own colour
				float c[3];
				for (int i = 0; i < 3; i++)
					c[i] = w.weatherColour(1, i) + 0.13f;
				setTint(c);
				// in the weather's own picture (an overcast sky's grey blanket, an ash storm's brown); while it changes,
				// now's fading out as the next one's fades in (OpenMW's two cloud layers)
				if (g_cloudTex[0])
				{
					const CellBatch& cb = here.sky[k];
					bool changing = g_cloudTex[1] && g_cloudBlend > 0.01f;
					if (changing)
					{
						CellBatch fade = cb;
						fade.flags |= BATCH_BLEND;
						rendererAlphaScale(1.0f - g_cloudBlend);
						drawBatchTex(g_cloudTex[0], fade, DRAW_SKY, time);
						rendererAlphaScale(g_cloudBlend);
						drawBatchTex(g_cloudTex[1], fade, DRAW_SKY, time);
						rendererAlphaScale(1.0f);
						draws += 2;
					}
					else
					{
						drawBatchTex(g_cloudTex[0], cb, DRAW_SKY, time);
						draws++;
					}
					continue;
				}
			}
			else
				setTint(g_gpu.daylight ? g_gpu.skyTint : kOne);
			drawBatch(here, here.sky[k], DRAW_SKY, time);
			draws++;
		}
		if (!bodiesDrawn)
			drawSkyBodies(view, secondEye, false);
		drawSkyBodies(view, secondEye, true);
	}
	drawProfAdd(DRAWP_SKY, drawSince);
	setStereoCurve(g_gpu.stereoCurve[1]);
	setModelView(view);
	setTint(g_gpu.daylight ? g_gpu.land : kOne);
	setLight(&view);
	if (fog)
		enableFog(here, fogStart, fogEnd);

	// Which static batches of each loaded cell can be seen this frame. The second eye (a few
	// units to the side, inside the frustum's margin) reuses the first eye's lists.
	Frustum fr = makeFrustum(cam, fog ? fogEnd : 0.0f);
	static std::vector<std::vector<u8>> visible;
	int shown = 0, total = 0;
	// The last frame's lists while the eye has hardly moved (4 units, a degree: well inside the frustum's 15% margin),
	// for a few frames, with the same cells and the same reach: standing or turning slowly, the culling is skipped
	static float listAt[5] = { 1e30f }, listReach[4];
	static int listAge = 0;
	static size_t listSig = 0;
	size_t sig = w.loaded.size();
	for (LoadedCell* l : w.loaded)
		sig = sig * 31 + (size_t)l->cell.batches.data() + l->cell.batches.size() + l->cell.places.size();
	float reach[4] = { fog ? fogEnd : 0.0f, fogScale, g_profile.detailScale, g_detailScale };
	float dyaw = fabsf(remainderf(cam.yaw - listAt[3], 6.2831853f));
	bool reuse = !secondEye && listAge < 6 && sig == listSig && memcmp(reach, listReach, sizeof(reach)) == 0
		&& dyaw < 0.0175f && fabsf(cam.pitch - listAt[4]) < 0.0175f && fabsf(cam.pos[0] - listAt[0])
		+ fabsf(cam.pos[1] - listAt[1]) + fabsf(cam.pos[2] - listAt[2]) < 4.0f && visible.size() == w.loaded.size();
	if (!secondEye)
	{
		listAge = reuse ? listAge + 1 : 0;
		if (!reuse)
		{
			memcpy(listAt, cam.pos, sizeof(cam.pos));
			listAt[3] = cam.yaw, listAt[4] = cam.pitch;
			memcpy(listReach, reach, sizeof(reach));
			listSig = sig;
		}
	}
	if (!secondEye && !reuse)
		visible.resize(w.loaded.size());
	for (size_t k = 0; k < w.loaded.size() && !secondEye && !reuse; k++)
	{
		Cell& cell = w.loaded[k]->cell;
		// (the batches, then the shared models' placements)
		const size_t nb = cell.batches.size();
		visible[k].resize(nb + cell.places.size());
		// A whole cell out of the frustum (most of the nine are behind the camera or beside it): none of its batches tested
		if (cell.batchBoundsOf != cell.batches.size())
		{
			for (int j = 0; j < 3; j++)
				cell.batchMin[j] = 1e30f, cell.batchMax[j] = -1e30f;
			for (const CellBatch& b : cell.batches)
				for (int j = 0; j < 3; j++)
					cell.batchMin[j] = fminf(cell.batchMin[j], b.bmin[j]), cell.batchMax[j] = fmaxf(cell.batchMax[j], b.bmax[j]);
			for (const SharedPlace& p : cell.places)
				for (int j = 0; j < 3; j++)
					cell.batchMin[j] = fminf(cell.batchMin[j], p.bmin[j]), cell.batchMax[j] = fmaxf(cell.batchMax[j], p.bmax[j]);
			cell.batchBoundsOf = cell.batches.size();
		}
		if (!cell.batches.empty() && !boxVisible(fr, cell.batchMin, cell.batchMax))
		{
			std::fill(visible[k].begin(), visible[k].end(), 0);
			total += visible[k].size();
			continue;
		}
		for (size_t i = 0; i < cell.batches.size(); i++)
		{
			const CellBatch& b = cell.batches[i];
			// Clutter's own cutoff shrinks with the view; terrain blend layers only near the player
			// (far chunks show their main texture, mostly under fog anyway)
			float maxDist = b.maxDist * fogScale * g_profile.detailScale * g_detailScale;
			if (b.flags & BATCH_DECAL)
				maxDist = kTerrainLayerDistance * fogScale;
			visible[k][i] = boxVisible(fr, b.bmin, b.bmax, maxDist);
			shown += visible[k][i];
		}
		for (size_t i = 0; i < cell.places.size(); i++)
		{
			const SharedPlace& p = cell.places[i];
			float maxDist = p.maxDist * fogScale * g_profile.detailScale * g_detailScale;
			visible[k][nb + i] = boxVisible(fr, p.bmin, p.bmax, maxDist);
			shown += visible[k][nb + i];
		}
		total += visible[k].size();
	}
	if (!secondEye && !reuse)
	{
		g_drawnBatches = shown;
		g_culledBatches = total - shown;
	}
	drawProfAdd(DRAWP_CULL, drawSince);

	// Opaque batches of all loaded cells sorted by texture and state, so the state cache above
	// skips most changes (order doesn't matter for them); terrain layers keep their order
	// (a shared model's placement: its transform and colours, PlaceDraw; -1 for the cell's own batches)
	struct OpaqueDraw { C3D_Tex* tex; u32 state; const CellBatch* b; size_t cell; int place; };
	struct PlaceDraw { C3D_Mtx mv; const SharedPlace* p; size_t cell; };
	static std::vector<OpaqueDraw> opaque;
	static std::vector<PlaceDraw> placeDraws;       // the visible placements (the second eye's too, and the blended pass's)
	if (!secondEye)
	{
		opaque.clear();
		placeDraws.clear();
	}
	for (size_t k = 0; k < w.loaded.size() && !secondEye; k++)
	{
		Cell& cell = w.loaded[k]->cell;
		for (size_t i = 0; i < cell.batches.size(); i++)
			if (visible[k][i] && batchPass(cell.batches[i]) == 0)
				opaque.push_back({ batchTexture(cell, cell.batches[i]),
					(u32)cell.batches[i].flags | ((u32)cell.batches[i].alphaRef << 8), &cell.batches[i], k, -1 });
		const size_t nb = cell.batches.size();
		for (size_t i = 0; i < cell.places.size(); i++)
		{
			// (a disabled reference's placement: not drawn, as a baked object's triangles go degenerate, hideRanges)
			const int ref = cell.places[i].ref;
			if (!visible[k][nb + i] || !cell.models[cell.places[i].model].colors
				|| (ref >= 0 && ref < (int)w.refs.size() && !w.refs[ref].visible()))
				continue;
			PlaceDraw pd;
			sharedPlaceView(view, cell.places[i], &pd.mv);
			pd.p = &cell.places[i];
			pd.cell = k;
			placeDraws.push_back(pd);
			const SharedModel& sm = cell.models[cell.places[i].model];
			for (u32 j = sm.firstBatch; j < sm.firstBatch + sm.numBatches; j++)
				if (batchPass(cell.shared[j]) == 0)
					opaque.push_back({ batchTexture(cell, cell.shared[j]),
						(u32)cell.shared[j].flags | ((u32)cell.shared[j].alphaRef << 8), &cell.shared[j], k, (int)placeDraws.size() - 1 });
		}
	}
	// (then by cell: a cell's batches share one position grid, so the transform goes up once for a run of them; a shared
	// model's batch then runs through its placements with the same layout)
	if (!secondEye)
		std::sort(opaque.begin(), opaque.end(), [](const OpaqueDraw& a, const OpaqueDraw& b) {
			return a.tex != b.tex ? a.tex < b.tex : a.state != b.state ? a.state < b.state : a.cell != b.cell ? a.cell < b.cell
				: a.b != b.b ? a.b < b.b : a.place < b.place;
		});
	if (!secondEye)
		g_sharedDrawn = (int)placeDraws.size();
	if (g_renderParts & RENDER_OPAQUE)
	{
		bool atView = true;
		for (auto& d : opaque)
		{
			if (d.place < 0)
			{
				if (!atView)
					setModelView(view);
				atView = true;
				drawBatchTex(d.tex, *d.b, DRAW_WORLD, time);
				continue;
			}
			const PlaceDraw& pd = placeDraws[d.place];
			setModelView(pd.mv);
			atView = false;
			CellBatch b = *d.b;
			b.colors = placeColors(w.loaded[pd.cell]->cell, *pd.p, *d.b);
			drawBatchTex(d.tex, b, DRAW_WORLD, time);
		}
		if (!atView)
			setModelView(view);
	}
	draws += opaque.size();
	drawProfAdd(DRAWP_OPAQUE, drawSince);

	// Distant land past the loaded cells (drawn after them: what they cover fails the depth test)
	if (fog && !(here.flags & CELL_INTERIOR) && (g_renderParts & RENDER_OPAQUE))
	{
		int gx = (int)floorf(cam.pos[0] / 8192.0f), gy = (int)floorf(cam.pos[1] / 8192.0f);
		if (!secondEye)
			distantUpdate(gx, gy, (int)ceilf(horizonEnd / 8192.0f) + 1);
		// Past the detailed view (an Old 3DS's closes in to 0.3 of it): the fog from where the near fog starts to the horizon.
		// It started past the near fog's end, by as much as the horizon is farther: just behind the near view, where the
		// detailed objects are lost in the fog, the distant land's stood out dark and clear
		bool horizon = horizonEnd > fogEnd + 1.0f;
		Frustum far = horizon ? makeFrustum(cam, horizonEnd) : fr;
		if (horizon)
			setFogVec(fogStart, horizonEnd);
		g_gpu.lastHorizonFog[0] = horizon ? fogStart : 0.0f;
		g_gpu.lastHorizonFog[1] = horizon ? horizonEnd : 0.0f;
		for (auto& d : distantCells())
		{
			int ci = w.gridCell(d.gx, d.gy);
			if (ci >= 0 && w.cells[ci].live)
				continue;
			if (boxVisible(far, d.batch.bmin, d.batch.bmax, horizonEnd + 500.0f))
			{
				drawBatchTex(nullptr, d.batch, DRAW_WORLD, time);
				draws++;
			}
			if (d.statics.numIndices && boxVisible(far, d.statics.bmin, d.statics.bmax, horizonEnd + 500.0f))
			{
				drawBatchTex(nullptr, d.statics, DRAW_WORLD, time);
				draws++;
			}
		}
		if (horizon)
			setFogVec(fogStart, fogEnd);
	}
	drawProfAdd(DRAWP_DISTANT, drawSince);
	for (size_t k = 0; k < w.loaded.size() && (g_renderParts & RENDER_LAYERS); k++)
		draws += drawBatches(w.loaded[k]->cell, w.loaded[k]->cell.batches, 1, &visible[k], time);

	// Enchanted things lying about shimmer: their own triangles in the cell's batches (Ref::ranges; the cell lists them
	// when it loads, LoadedCell::glowRefs)
	for (size_t k = 0; k < w.loaded.size() && g_gpu.caustCount && (g_renderParts & RENDER_OPAQUE); k++)
	{
		Cell& cell = w.loaded[k]->cell;
		for (auto& g : w.loaded[k]->glowRefs)
		{
			const Ref& r = w.refs[g.first];
			if (!r.visible())
				continue;
			for (auto& rg : r.ranges)
				if (rg.batch >= 0 && rg.batch < (int)cell.batches.size() && visible[k][rg.batch] && rg.count >= 3)
				{
					CellBatch b = cell.batches[rg.batch];
					b.indices += rg.first;
					b.numIndices = rg.count;
					drawShimmer(b, g.second);
					draws++;
				}
		}
	}

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
	drawProfAdd(DRAWP_LAYERS, drawSince);     // (the terrain layers, the shimmers and the doors)
	u64 actorStart = svcGetSystemTick();
	// (an Old 3DS draws people and creatures to 4000 outdoors: each is a re-skin and a dozen draws)
	float maxDist = fog && !(here.flags & CELL_INTERIOR)
		? fminf(fogEnd, g_profile.actorViewLimit > 0.0f ? g_profile.actorViewLimit : kActorViewDistance) : 4000.0f;
	float detailDist = kActorDetailDistance * g_profile.actorDetailScale * g_actorDetailScale;
	// (one pass: the blended meshes go to the blend queue, drawn back to front with the world's)
	if (g_renderParts & RENDER_ACTORS)
	{
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
				// Re-skinned for each new pose: World::update poses those with skins on the re-skin cadence (often up close,
				// less often the smaller they are on screen). A pose held for a side-by-side shot (the test driver's POSE)
				// every time: the native build draws only the frames it saves
				if (!secondEye && (a.poseSerial != a.skinSerial || !a.deformed || a.frozen))
				{
					actorDeform(l->actors, a);
					a.deformed = true;
				}
				// Far away, small pieces (rings, belts, hair strands: a few pixels) aren't worth a draw
				bool far = dist2 > detailDist * detailDist;
				// Skinned meshes are in actor space and share the placement; rigid ones ride their bone
				C3D_Mtx placeMv;
				{
					C3D_Mtx place;
					for (int r = 0; r < 3; r++)
						place.r[r] = FVec4_New(a.place[r * 4], a.place[r * 4 + 1], a.place[r * 4 + 2], a.place[r * 4 + 3]);
					place.r[3] = FVec4_New(0, 0, 0, 1);
					Mtx_Multiply(&placeMv, &view, &place);
				}
				// (see-through under Invisibility or Chameleon: every mesh blended, in the back-to-front queue)
				const bool ghost = a.alpha < 0.999f;
				float tint[3];
				actorTint(a, tint);
				setTint(tint);
				for (auto& m : a.meshes)
				{
					if (((m.flags & ACTOR_MESH_WEAPON) && !a.showWeapon) || !actorMeshShown(a, m))
						continue;
					if (far && m.numIndices < kActorDetailTris * 3)
						continue;
					if (m.gpuSkin && ((m.flags & BATCH_BLEND) || ghost))
					{
						BlendDraw bd = { d[0] * d[0] + d[1] * d[1] + d[2] * d[2], &l->cell, nullptr, &m, placeMv, 0, &a, a.alpha };
						memcpy(bd.tint, tint, sizeof(bd.tint));
						g_gpu.blendQueue.push_back(bd);
						continue;
					}
					if (m.gpuSkin)
					{
						SkinDraw sd = { &l->cell, &a, &m, placeMv };
						memcpy(sd.tint, tint, sizeof(sd.tint));
						g_gpu.skinQueue.push_back(sd);
						continue;
					}
					if ((m.flags & BATCH_BLEND) || ghost)
					{
						BlendDraw bd = { d[0] * d[0] + d[1] * d[1] + d[2] * d[2], &l->cell, nullptr, &m, placeMv, 0, nullptr, a.alpha };
						memcpy(bd.tint, tint, sizeof(bd.tint));
						if (m.bone >= 0 && m.kind != ACTOR_SKINNED)
						{
							C3D_Mtx model;
							actorMeshMatrix(a, m, &model);
							Mtx_Multiply(&bd.mv, &view, &model);
						}
						const Ref& ar = w.refs[a.ref];
						if ((m.flags & ACTOR_MESH_WEAPON) && g_gpu.caustCount && ar.actor >= 0)
							bd.shimmer = w.game.enchantGlow(w.game.actors[ar.actor].weapon);
						g_gpu.blendQueue.push_back(bd);
						continue;
					}
					if (m.bone >= 0 && m.kind != ACTOR_SKINNED)
					{
						C3D_Mtx model, mv;
						actorMeshMatrix(a, m, &model);
						Mtx_Multiply(&mv, &view, &model);
						setModelView(mv);
					}
					else
						setModelView(placeMv);
					CellBatch b = { m.tex, actorBatchFlags(m), m.alphaRef, m.numVerts, m.numIndices, m.verts, m.indices, {}, {}, 0.0f };
					bool flat = m.glow && !g_gpu.caustCount;
					if (flat)
						glowStage(m.glow);         // (enchanted armour and clothing: flat only without the caustic frames)
					drawBatch(l->cell, b, DRAW_WORLD, time);
					if (flat)
						glowStage(0);
					else if (m.glow)
					{
						// (shimmering as a weapon does: the caustic frames added over it, not a colour over all of it)
						drawShimmer(b, m.glow);
						draws++;
					}
					draws++;
					// their weapon, enchanted, shimmers
					const Ref& ar = w.refs[a.ref];
					if ((m.flags & ACTOR_MESH_WEAPON) && g_gpu.caustCount && ar.actor >= 0)
						if (u32 glow = w.game.enchantGlow(w.game.actors[ar.actor].weapon))
						{
							drawShimmer(b, glow);
							draws++;
						}
				}
			}
		draws += drawSkinQueue(time);
		setTint(worldTint());
	}

	drawSince = svcGetSystemTick();
	g_profActorsMs += (drawSince - actorStart) * 1000.0f / SYSCLOCK_ARM11;

	for (size_t k = 0; k < w.loaded.size() && (g_renderParts & RENDER_BLENDED); k++)
	{
		std::vector<CellBatch>& batches = w.loaded[k]->cell.batches;
		for (size_t i = 0; i < batches.size(); i++)
			if (batchPass(batches[i]) == 2 && visible[k][i])
			{
				const CellBatch& b = batches[i];
				float c[3];
				for (int j = 0; j < 3; j++)
					c[j] = (b.bmin[j] + b.bmax[j]) * 0.5f - cam.pos[j];
				BlendDraw bd = { c[0] * c[0] + c[1] * c[1] + c[2] * c[2], &w.loaded[k]->cell, &b, nullptr, view, 0 };
				memcpy(bd.tint, worldTint(), sizeof(bd.tint));
				g_gpu.blendQueue.push_back(bd);
			}
	}
	for (const PlaceDraw& pd : placeDraws)
	{
		if (!(g_renderParts & RENDER_BLENDED) || pd.cell >= w.loaded.size())
			break;
		Cell& cell = w.loaded[pd.cell]->cell;
		const SharedModel& sm = cell.models[pd.p->model];
		float c[3];
		for (int j = 0; j < 3; j++)
			c[j] = (pd.p->bmin[j] + pd.p->bmax[j]) * 0.5f - cam.pos[j];
		for (u32 j = sm.firstBatch; j < sm.firstBatch + sm.numBatches; j++)
			if (batchPass(cell.shared[j]) == 2)
			{
				BlendDraw bd = { c[0] * c[0] + c[1] * c[1] + c[2] * c[2], &cell, &cell.shared[j], nullptr, pd.mv, 0 };
				memcpy(bd.tint, worldTint(), sizeof(bd.tint));
				bd.colors = placeColors(cell, *pd.p, cell.shared[j]);
				g_gpu.blendQueue.push_back(bd);
			}
	}
	draws += drawBlendQueue(time);
	drawProfAdd(DRAWP_BLEND, drawSince);
	setModelView(view);
	disableFog();
	setUvOffset(0.0f, 0.0f);
	if (!secondEye)
		g_worldDraws = draws;
	return draws;
}

void rendererDrawMesh(const ActorMesh& m, const C3D_Mtx* model, C3D_Tex* tex)
{
	C3D_Mtx mv;
	if (model)
		Mtx_Multiply(&mv, &g_gpu.view, model);
	else
		mv = g_gpu.view;
	setModelView(mv);
	CellBatch b = { -1, m.flags, m.alphaRef, m.numVerts, m.numIndices, m.verts, m.indices, {}, {}, 0.0f };
	if (g_gpu.meshAlpha < 0.999f)
	{
		b.flags |= BATCH_BLEND;
		rendererAlphaScale(g_gpu.meshAlpha);
		drawBatchTex(tex, b, DRAW_WORLD, g_gpu.time);
		rendererAlphaScale(1.0f);
		return;
	}
	drawBatchTex(tex, b, DRAW_WORLD, g_gpu.time);
}

void rendererLoadCaustics(const char* dataDir)
{
	if (g_gpu.caustCount)
		return;
	for (int i = 0; i < 32; i++)
	{
		char path[256];
		snprintf(path, sizeof(path), "%s/art/magicitem/caust%02d.t3x", dataDir, i);
		if (!texImportFile(&g_gpu.caust[i], path))
			break;
		texToVramLater(&g_gpu.caust[i]);
		C3D_TexSetFilter(&g_gpu.caust[i], GPU_LINEAR, GPU_LINEAR);
		g_gpu.caustCount = i + 1;
	}
	logf("renderer: %d enchantment shimmer frames", g_gpu.caustCount);
}

int rendererCausticFrames()
{
	return g_gpu.caustCount;
}

// The second pass over an enchanted batch, under the model view already set: the current frame x the colour
// (0xRRGGBB), added on the same surface
static void shimmerStage(u32 rgb)
{
	// the glow stage: texture x colour, replacing the lit texture, torch and fog before it (alpha 1: added whole)
	C3D_TexEnv* env = C3D_GetTexEnv(kTevGlow);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_CONSTANT);
	C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
	C3D_TexEnvSrc(env, C3D_Alpha, GPU_CONSTANT);
	C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
	C3D_TexEnvColor(env, 0xFF000000 | ((rgb & 255) << 16) | (rgb & 0xFF00) | ((rgb >> 16) & 255));
}

static void drawShimmer(CellBatch b, u32 rgb)
{
	b.flags = (b.flags & BATCH_TWO_SIDED) | BATCH_BLEND | BATCH_ADDITIVE | BATCH_DECAL;
	b.alphaRef = 0;
	b.uvScale[0] *= kCaustTiles;
	b.uvScale[1] *= kCaustTiles;
	shimmerStage(rgb);
	drawBatchTex(&g_gpu.caust[(int)(g_gpu.time * kCaustFps) % g_gpu.caustCount], b, DRAW_WORLD, g_gpu.time);
	C3D_TexEnvInit(C3D_GetTexEnv(kTevGlow));
}

void rendererDrawMeshGlow(const ActorMesh& m, const C3D_Mtx* model, C3D_Tex* tex, u32 glow)
{
	if (!glow)
		rendererDrawMesh(m, model, tex);
	else if (g_gpu.caustCount)
	{
		rendererDrawMesh(m, model, tex);
		drawShimmer({ -1, m.flags, m.alphaRef, m.numVerts, m.numIndices, m.verts, m.indices, {}, {}, 0.0f }, glow);
	}
	else
	{
		// without the shimmer's frames in the data: the colour added over the lit texture (a free combiner
		// stage), pulsing
		float k = 0.30f + 0.12f * sinf(osGetTime() * 0.004f);
		u32 r = (u32)(((glow >> 16) & 255) * k), g = (u32)(((glow >> 8) & 255) * k), b = (u32)((glow & 255) * k);
		C3D_TexEnv* env = C3D_GetTexEnv(kTevGlow);
		C3D_TexEnvInit(env);
		C3D_TexEnvSrc(env, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT);
		C3D_TexEnvFunc(env, C3D_RGB, GPU_ADD);
		C3D_TexEnvSrc(env, C3D_Alpha, GPU_PREVIOUS);
		C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
		C3D_TexEnvColor(env, 0xFF000000 | (b << 16) | (g << 8) | r);
		rendererDrawMesh(m, model, tex);
		C3D_TexEnvInit(C3D_GetTexEnv(kTevGlow));
	}
}

void rendererDrawActor(Actor& a, const std::vector<C3D_Tex*>& textures, bool viewModel)
{
	// The view model's depth is squeezed in front of the world, so arms never sink into walls
	if (viewModel)
		C3D_DepthMap(true, -0.1f, 0.9f);
	// In 3D the arms (a few dozen units away) get their own focus near them: with the world's they
	// would leap far out of the screen. And their own view always, the world's wider one or not (a werewolf's)
	const bool ownProjection = viewModel && (g_gpu.eyeShift != 0.0f || g_worldFovY != kFovY);
	if (ownProjection)
	{
		C3D_Mtx projection;
		if (g_gpu.eyeShift != 0.0f)
			Mtx_PerspStereoTilt(&projection, C3D_AngleFromDegrees(kFovY), C3D_AspectRatioTop, kNear, g_gpu.farPlane,
				g_gpu.eyeShift * 0.5f, kViewModelFocus, false);
		else
			Mtx_PerspTilt(&projection, C3D_AngleFromDegrees(kFovY), C3D_AspectRatioTop, kNear, g_gpu.farPlane, false);
		C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_gpu.uProjection, &projection);
	}
	g_gpu.meshAlpha = a.alpha;
	for (int pass = 0; pass < 2; pass++)
		for (auto& m : a.meshes)
		{
			if (((m.flags & BATCH_BLEND) != 0) != (pass == 1) || !actorMeshShown(a, m))
				continue;
			C3D_Tex* tex = m.tex >= 0 && m.tex < (int)textures.size() ? textures[m.tex] : nullptr;
			C3D_Mtx model;
			actorMeshMatrix(a, m, &model);
			rendererDrawMeshGlow(m, &model, tex, g_gpu.meshAlpha < 0.999f ? 0 : m.glow);     // (an enchanted item shimmers)
		}
	g_gpu.meshAlpha = 1.0f;
	if (viewModel)
	{
		C3D_DepthMap(true, -1.0f, 0.0f);
		if (ownProjection)
			C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_gpu.uProjection, &g_gpu.projection);
	}
	setModelView(g_gpu.view);
}

void rendererViewAxes(float right[3], float up[3], float back[3])
{
	// the view matrix's rotation rows are the camera axes in world space
	right[0] = g_gpu.view.r[0].x; right[1] = g_gpu.view.r[0].y; right[2] = g_gpu.view.r[0].z;
	up[0] = g_gpu.view.r[1].x; up[1] = g_gpu.view.r[1].y; up[2] = g_gpu.view.r[1].z;
	back[0] = g_gpu.view.r[2].x; back[1] = g_gpu.view.r[2].y; back[2] = g_gpu.view.r[2].z;
}

void rendererSpriteFrame()
{
	g_gpu.spritesAskedLast = g_gpu.spritesAsked;
	g_gpu.spritesDrawnLast = g_gpu.spritesDrawn;
	g_gpu.spritesWantedLast = g_gpu.spritesWanted;
	g_gpu.spritesAsked = g_gpu.spritesDrawn = g_gpu.spritesWanted = 0;
}

void rendererSpritesWanted(float n)
{
	g_gpu.spritesWanted += n;
}

// The base share of its particles each visual draws this frame: all of them while every visual's together fit the
// budget, else budget / (last frame's particles, each counted by its visual's nearness weight); a visual keeps that
// times its own weight. So ten visuals all show (the nearest the fullest) rather than the first one drawn taking the
// whole budget
float rendererSpriteShare()
{
	float budget = (float)g_profile.spriteBudget;
	return g_gpu.spritesWantedLast > budget ? budget / g_gpu.spritesWantedLast : 1.0f;
}

float rendererSpritesWantedLast()
{
	return g_gpu.spritesWantedLast;
}

void rendererSpriteCounts(int& asked, int& drawn)
{
	asked = g_gpu.spritesAskedLast;
	drawn = g_gpu.spritesDrawnLast;
}

void rendererDrawSprites(const float* pos, const float* half, int n, C3D_Tex* tex, u32 rgba, const u32* colors, bool additive)
{
	g_gpu.spritesAsked += std::max(0, n);
	n = std::min(n, g_gpu.spritesLeft);
	g_gpu.spritesLeft -= std::max(0, n);
	g_gpu.spritesDrawn += std::max(0, n);
	static const int kMaxPerDraw = 100;                // quads one draw takes
	static const int kRing = 2 * g_profile.spriteBudgetMax;   // all a frame's, both eyes, at the most the budget gets
	static u8* ring = nullptr;
	static u16* indices = nullptr;
	if (!ring)
	{
		ring = (u8*)lockedLinearAlloc(kRing * 4 * 24);
		indices = (u16*)lockedLinearAlloc(kMaxPerDraw * 6 * 2);
		if (!ring || !indices)
			return;
		for (int q = 0; q < kMaxPerDraw; q++)
		{
			static const u16 idx[6] = { 0, 1, 2, 0, 2, 3 };
			for (int k = 0; k < 6; k++)
				indices[q * 6 + k] = (u16)(q * 4 + idx[k]);
		}
		GSPGPU_FlushDataCache(indices, kMaxPerDraw * 6 * 2);
	}
	n = std::min(n, kMaxPerDraw);
	if (!ring || n <= 0)
		return;
	// The GPU reads every eye's sprites after the frame is built: a ring that wrapped within the frame wrote the right
	// eye's over the left eye's, and the left eye showed other particles' quads (the frame starts the ring at 0, and two
	// eyes' budgets fill it at most)
	if (g_gpu.spriteSlot + n > kRing)
	{
		monitorOnce("sprite ring", "the sprite ring is full (%d of %d quads this frame): sprites dropped", g_gpu.spriteSlot, kRing);
		return;
	}
	u8* v = ring + g_gpu.spriteSlot * 4 * 24;
	g_gpu.spriteSlot += n;
	float right[3] = { g_gpu.view.r[0].x, g_gpu.view.r[0].y, g_gpu.view.r[0].z };
	float up[3] = { g_gpu.view.r[1].x, g_gpu.view.r[1].y, g_gpu.view.r[1].z };
	static const float corner[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
	for (int q = 0; q < n; q++)
		for (int i = 0; i < 4; i++)
		{
			float* p = (float*)(v + (q * 4 + i) * 24);
			for (int k = 0; k < 3; k++)
				p[k] = pos[q * 3 + k] + (right[k] * corner[i][0] + up[k] * corner[i][1]) * half[q];
			p[3] = corner[i][0] * 0.5f + 0.5f;
			p[4] = corner[i][1] * 0.5f + 0.5f;
			memcpy(v + (q * 4 + i) * 24 + 20, colors ? &colors[q] : &rgba, 4);
		}
	GSPGPU_FlushDataCache(v, n * 4 * 24);
	ActorMesh m = {};
	m.flags = BATCH_BLEND | (additive ? BATCH_ADDITIVE : 0) | BATCH_TWO_SIDED | BATCH_CLAMP;
	m.numVerts = n * 4;
	m.numIndices = n * 6;
	m.verts = v;
	m.indices = indices;
	rendererDrawMesh(m, nullptr, tex);
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
	if (!ring || g_gpu.glowCount >= kMaxGlows)
		return;
	void* verts = ring + (g_gpu.glowCount++) * 4 * 24;
	// Billboard: the view matrix's rotation rows are the camera axes in world space
	float right[3] = { g_gpu.view.r[0].x, g_gpu.view.r[0].y, g_gpu.view.r[0].z };
	float up[3] = { g_gpu.view.r[1].x, g_gpu.view.r[1].y, g_gpu.view.r[1].z };
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
