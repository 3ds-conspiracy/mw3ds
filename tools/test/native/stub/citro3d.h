// Host stand-in for citro3d: types and no-op drawing calls; the matrix math is real (viewmodel.cpp reads it)
#pragma once
#include "3ds.h"
#include <cmath>
#include <cstring>

typedef struct { float w, z, y, x; } C3D_FVec;   // same member order as citro3d's
inline C3D_FVec FVec4_New(float x, float y, float z, float w) { return C3D_FVec{ w, z, y, x }; }
inline C3D_FVec FVec3_New(float x, float y, float z) { return C3D_FVec{ 0, z, y, x }; }
typedef struct { C3D_FVec r[4]; } C3D_Mtx;
inline void Mtx_Zeros(C3D_Mtx* m) { memset(m, 0, sizeof(*m)); }
inline void Mtx_Identity(C3D_Mtx* m) { Mtx_Zeros(m); m->r[0].x = m->r[1].y = m->r[2].z = m->r[3].w = 1.0f; }
inline float* mtxp(C3D_Mtx* m, int r, int c) { float* f = (float*)&m->r[r]; return &f[3 - c]; }   // columns x, y, z, w = 0..3
inline void Mtx_Multiply(C3D_Mtx* out, const C3D_Mtx* a, const C3D_Mtx* b)
{
	C3D_Mtx t;
	for (int i = 0; i < 4; i++)
		for (int j = 0; j < 4; j++)
		{
			float s = 0;
			for (int k = 0; k < 4; k++)
				s += *mtxp((C3D_Mtx*)a, i, k) * *mtxp((C3D_Mtx*)b, k, j);
			*mtxp(&t, i, j) = s;
		}
	*out = t;
}
inline void Mtx_Translate(C3D_Mtx* m, float x, float y, float z, bool bRight)
{
	C3D_Mtx t; Mtx_Identity(&t);
	t.r[0].w = x; t.r[1].w = y; t.r[2].w = z;
	if (bRight) Mtx_Multiply(m, m, &t); else Mtx_Multiply(m, &t, m);
}
inline void Mtx_Scale(C3D_Mtx* m, float x, float y, float z)
{
	for (int i = 0; i < 4; i++) { *mtxp(m, i, 0) *= x; *mtxp(m, i, 1) *= y; *mtxp(m, i, 2) *= z; }
}
inline void Mtx_RotateX(C3D_Mtx* m, float a, bool bRight)
{
	C3D_Mtx r; Mtx_Identity(&r); float c = cosf(a), s = sinf(a);
	r.r[1].y = c; r.r[1].z = -s; r.r[2].y = s; r.r[2].z = c;
	if (bRight) Mtx_Multiply(m, m, &r); else Mtx_Multiply(m, &r, m);
}
inline void Mtx_RotateY(C3D_Mtx* m, float a, bool bRight)
{
	C3D_Mtx r; Mtx_Identity(&r); float c = cosf(a), s = sinf(a);
	r.r[0].x = c; r.r[0].z = s; r.r[2].x = -s; r.r[2].z = c;
	if (bRight) Mtx_Multiply(m, m, &r); else Mtx_Multiply(m, &r, m);
}
inline void Mtx_RotateZ(C3D_Mtx* m, float a, bool bRight)
{
	C3D_Mtx r; Mtx_Identity(&r); float c = cosf(a), s = sinf(a);
	r.r[0].x = c; r.r[0].y = -s; r.r[1].x = s; r.r[1].y = c;
	if (bRight) Mtx_Multiply(m, m, &r); else Mtx_Multiply(m, &r, m);
}
inline void Mtx_Ortho(C3D_Mtx* m, float, float, float, float, float, float, bool) { Mtx_Identity(m); }
inline void Mtx_PerspTilt(C3D_Mtx* m, float, float, float, float, bool) { Mtx_Identity(m); }
inline void Mtx_PerspStereoTilt(C3D_Mtx* m, float, float, float, float, float, float, bool) { Mtx_Identity(m); }
inline void Mtx_LookAt(C3D_Mtx* m, C3D_FVec, C3D_FVec, C3D_FVec, bool) { Mtx_Identity(m); }
#define C3D_AngleFromDegrees(d) ((float)(d) * 3.14159265358979f / 180.0f)
#define C3D_AspectRatioTop (400.0f / 240.0f)

typedef int GPU_TEXTURE_FILTER_PARAM;
typedef int GPU_TEXTURE_WRAP_PARAM;
struct C3D_Tex { void* data = nullptr; u16 width = 0, height = 0; u32 size = 0; u8 maxLevel = 0; };
struct C3D_RenderTarget { int unused; };
struct C3D_TexEnv { int unused; };
struct C3D_AttrInfo { int unused; };
struct C3D_BufInfo { int unused; };
struct Tex3DS_SubTexture { u16 width, height; float left, top, right, bottom; };
typedef void* Tex3DS_Texture;
struct C2D_Image { C3D_Tex* tex; const Tex3DS_SubTexture* subtex; };

// Every GPU enumerant the game names is just a number here
enum GpuEnum : int {
	GPU_VERTEX_SHADER, GPU_LINEAR, GPU_WRITE_COLOR, GPU_SCISSOR_NORMAL, GPU_SCISSOR_DISABLE, GPU_CLAMP_TO_EDGE,
	GPU_BLEND_ADD, GPU_ALWAYS, GPU_SRC_ALPHA, GPU_PREVIOUS, GPU_REPLACE, GPU_RB_DEPTH24_STENCIL8, GPU_ONE_MINUS_SRC_ALPHA,
	GPU_ONE, GPU_NEAREST, GPU_RB_RGBA8, GPU_ZERO, GPU_TEVOP_RGB_SRC_COLOR, GPU_SHORT,
	GPU_PRIMARY_COLOR, GPU_GREATER, GPU_FLOAT, GPU_CULL_NONE, GPU_CONSTANT, GPU_WRITE_ALL, GPU_UNSIGNED_BYTE,
	GPU_TRIANGLES, GPU_TEXTURE1, GPU_TEXTURE0, GPU_TEXFACE_2D, GPU_TEVOP_RGB_SRC_ALPHA, GPU_RGBA8,
	GPU_REPEAT, GPU_MODULATE, GPU_INTERPOLATE, GPU_GEQUAL, GPU_CULL_BACK_CCW, GPU_BLENDFACTOR, GPU_ADD, GPU_A8,
	GFX_TOP, GFX_BOTTOM, GFX_LEFT, GFX_RIGHT, C3D_UNSIGNED_SHORT, C3D_Both, C3D_CLEAR_ALL, C3D_FRAME_SYNCDRAW,
	C3D_DEFAULT_CMDBUF_SIZE = 0x40000,
};

inline bool C3D_Init(size_t) { return true; }
inline void C3D_Fini() {}
inline bool C3D_FrameBegin(u8) { return true; }
inline void C3D_FrameEnd(u8) {}
inline void C3D_FrameDrawOn(C3D_RenderTarget*) {}
inline float C3D_GetCmdBufUsage() { return 0; }
inline float C3D_GetProcessingTime() { return 0; }
inline float C3D_GetDrawingTime() { return 0; }
inline C3D_RenderTarget* C3D_RenderTargetCreate(int, int, int, int) { static C3D_RenderTarget t; return &t; }
inline C3D_RenderTarget* C3D_RenderTargetCreateFromTex(C3D_Tex*, int, int, int) { static C3D_RenderTarget t; return &t; }
inline void C3D_RenderTargetSetOutput(C3D_RenderTarget*, int, int, u32) {}
inline void C3D_RenderTargetClear(C3D_RenderTarget*, int, u32, u32) {}
inline void C3D_RenderTargetDelete(C3D_RenderTarget*) {}
inline void C3D_DepthTest(bool, int, int) {}
inline void C3D_DepthMap(bool, float, float) {}
inline void C3D_CullFace(int) {}
inline void C3D_AlphaTest(bool, int, int) {}
inline void C3D_AlphaBlend(int, int, int, int, int, int) {}
inline void C3D_SetScissor(int, u32, u32, u32, u32) {}
inline void C3D_TexSetFilter(C3D_Tex*, int, int) {}
inline void C3D_TexSetFilterMipmap(C3D_Tex*, int) {}
inline void C3D_TexSetWrap(C3D_Tex*, int, int) {}
inline void C3D_TexBind(int, C3D_Tex*) {}
inline void C3D_TexFlush(C3D_Tex*) {}
inline void C3D_TexDelete(C3D_Tex*) {}
inline bool C3D_TexInit(C3D_Tex* t, u16 w, u16 h, int) { t->width = w; t->height = h; return true; }
inline bool C3D_TexInitVRAM(C3D_Tex* t, u16 w, u16 h, int) { t->width = w; t->height = h; return true; }
inline u32 C3D_TexCalcTotalSize(u32 s, int) { return s; }
inline C3D_TexEnv* C3D_GetTexEnv(int) { static C3D_TexEnv e; return &e; }
inline void C3D_TexEnvInit(C3D_TexEnv*) {}
inline void C3D_TexEnvSrc(C3D_TexEnv*, int, int, int, int) {}
inline void C3D_TexEnvFunc(C3D_TexEnv*, int, int) {}
inline void C3D_TexEnvOpRgb(C3D_TexEnv*, int, int, int) {}
inline void C3D_TexEnvColor(C3D_TexEnv*, u32) {}
inline u32 C3D_RGB(u8, u8, u8) { return 0; }
inline u32 C3D_Alpha(u8) { return 0; }
inline void C3D_FVUnifSet(int, int, float, float, float, float) {}
inline void C3D_FVUnifMtx4x4(int, int, const C3D_Mtx*) {}
inline C3D_AttrInfo* C3D_GetAttrInfo() { static C3D_AttrInfo a; return &a; }
inline C3D_BufInfo* C3D_GetBufInfo() { static C3D_BufInfo b; return &b; }
inline void C3D_DrawElements(int, int, int, const void*) {}
inline void C3D_BindProgram(void*) {}
inline Tex3DS_Texture Tex3DS_TextureImport(const void*, size_t, C3D_Tex*, void*, bool) { return nullptr; }
inline Tex3DS_Texture Tex3DS_TextureImportStdio(FILE*, C3D_Tex* t, void*, bool) { t->width = t->height = 8; return (Tex3DS_Texture)1; }   // every texture 'loads'
inline void Tex3DS_TextureFree(Tex3DS_Texture) {}
