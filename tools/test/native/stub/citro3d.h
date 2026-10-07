// Host stand-in for citro3d. The matrix math is real (viewmodel.cpp reads it, and the projection is what the
// software GPU in native_gpu.cpp draws with). Every GPU call goes to native_gpu.cpp: with NATIVE_DRAW=1 it draws
// both screens in software (for SHOT); without it the calls return at once and the run stays headless.
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
inline C3D_FVec Mtx_MultiplyFVec4(const C3D_Mtx* m, C3D_FVec v)
{
	float in[4] = { v.x, v.y, v.z, v.w }, out[4];
	for (int i = 0; i < 4; i++)
	{
		out[i] = 0;
		for (int k = 0; k < 4; k++)
			out[i] += *mtxp((C3D_Mtx*)m, i, k) * in[k];
	}
	return FVec4_New(out[0], out[1], out[2], out[3]);
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
// Projections. citro3d's "Tilt" versions turn the picture a quarter for the 3DS's sideways framebuffers; the
// software GPU draws upright, so these are the plain ones. Depth as the PICA wants it: near -> -1, far -> 0
// (C3D_DepthMap(true, -1, 0) then makes nearer bigger, for GPU_GREATER).
inline void Mtx_PerspStereoTilt(C3D_Mtx* m, float fovy, float aspect, float n, float f, float iod, float screen, bool)
{
	float k = 1.0f / tanf(fovy * 0.5f);
	Mtx_Zeros(m);
	m->r[0].x = k / aspect;
	m->r[1].y = k;
	// Each eye iod to the side, looking parallel; the two pictures agree at depth `screen`
	if (iod != 0.0f && screen > 0.0f)
	{
		m->r[0].w = -iod * k / aspect;
		m->r[0].z = -iod * k / aspect / screen;
	}
	m->r[2].z = -n / (f - n);
	m->r[2].w = -n * f / (f - n);
	m->r[3].z = -1.0f;
}
inline void Mtx_PerspTilt(C3D_Mtx* m, float fovy, float aspect, float n, float f, bool lh)
{
	Mtx_PerspStereoTilt(m, fovy, aspect, n, f, 0.0f, 0.0f, lh);
}
inline void Mtx_Ortho(C3D_Mtx* m, float l, float r, float b, float t, float n, float f, bool)
{
	Mtx_Zeros(m);
	m->r[0].x = 2.0f / (r - l);
	m->r[0].w = -(r + l) / (r - l);
	m->r[1].y = 2.0f / (t - b);
	m->r[1].w = -(t + b) / (t - b);
	m->r[2].z = -1.0f / (f - n);
	m->r[2].w = -f / (f - n);
	m->r[3].w = 1.0f;
}
inline void Mtx_LookAt(C3D_Mtx* m, C3D_FVec eye, C3D_FVec target, C3D_FVec up, bool)
{
	float fw[3] = { target.x - eye.x, target.y - eye.y, target.z - eye.z };
	float u[3] = { up.x, up.y, up.z };
	auto norm = [](float* v) { float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (l > 0) { v[0] /= l; v[1] /= l; v[2] /= l; } };
	norm(fw);
	float s[3] = { fw[1] * u[2] - fw[2] * u[1], fw[2] * u[0] - fw[0] * u[2], fw[0] * u[1] - fw[1] * u[0] };
	norm(s);
	float uu[3] = { s[1] * fw[2] - s[2] * fw[1], s[2] * fw[0] - s[0] * fw[2], s[0] * fw[1] - s[1] * fw[0] };
	float e[3] = { eye.x, eye.y, eye.z };
	Mtx_Identity(m);
	m->r[0] = FVec4_New(s[0], s[1], s[2], -(s[0] * e[0] + s[1] * e[1] + s[2] * e[2]));
	m->r[1] = FVec4_New(uu[0], uu[1], uu[2], -(uu[0] * e[0] + uu[1] * e[1] + uu[2] * e[2]));
	m->r[2] = FVec4_New(-fw[0], -fw[1], -fw[2], fw[0] * e[0] + fw[1] * e[1] + fw[2] * e[2]);
	m->r[3] = FVec4_New(0, 0, 0, 1);
}
#define C3D_AngleFromDegrees(d) ((float)(d) * 3.14159265358979f / 180.0f)
#define C3D_AspectRatioTop (400.0f / 240.0f)

// Texture formats: the PICA's numbers (they are what a .t3x header holds)
enum GPU_TEXCOLOR : int {
	GPU_RGBA8 = 0, GPU_RGB8, GPU_RGBA5551, GPU_RGB565, GPU_RGBA4, GPU_LA8, GPU_HILO8, GPU_L8, GPU_A8, GPU_LA4, GPU_L4,
	GPU_A4, GPU_ETC1, GPU_ETC1A4,
};
// Combiner channel masks
enum { C3D_RGB = 1, C3D_Alpha = 2, C3D_Both = 3 };

// Every other GPU enumerant the game names is just a number here
enum GpuEnum : int {
	GPU_VERTEX_SHADER = 100, GPU_LINEAR, GPU_WRITE_COLOR, GPU_SCISSOR_NORMAL, GPU_SCISSOR_DISABLE, GPU_CLAMP_TO_EDGE,
	GPU_BLEND_ADD, GPU_ALWAYS, GPU_SRC_ALPHA, GPU_PREVIOUS, GPU_REPLACE, GPU_RB_DEPTH24_STENCIL8, GPU_ONE_MINUS_SRC_ALPHA,
	GPU_ONE, GPU_NEAREST, GPU_RB_RGBA8, GPU_ZERO, GPU_TEVOP_RGB_SRC_COLOR, GPU_SHORT,
	GPU_PRIMARY_COLOR, GPU_GREATER, GPU_FLOAT, GPU_CULL_NONE, GPU_CONSTANT, GPU_WRITE_ALL, GPU_UNSIGNED_BYTE,
	GPU_TRIANGLES, GPU_TEXTURE1, GPU_TEXTURE0, GPU_TEXFACE_2D, GPU_TEVOP_RGB_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA,
	GPU_REPEAT, GPU_MODULATE, GPU_INTERPOLATE, GPU_GEQUAL, GPU_CULL_BACK_CCW, GPU_ADD, GPU_LESS, GPU_LEQUAL, GPU_EQUAL,
	GPU_NEVER, GPU_NOTEQUAL, GPU_BYTE,
	GFX_TOP, GFX_BOTTOM, GFX_LEFT, GFX_RIGHT, C3D_UNSIGNED_SHORT, C3D_UNSIGNED_BYTE, C3D_CLEAR_ALL, C3D_CLEAR_COLOR,
	C3D_CLEAR_DEPTH, C3D_FRAME_SYNCDRAW,
	C3D_DEFAULT_CMDBUF_SIZE = 0x40000,
};
typedef int GPU_TEXTURE_FILTER_PARAM;
typedef int GPU_TEXTURE_WRAP_PARAM;
typedef int GPU_BLENDFACTOR;

struct C3D_Tex
{
	void* data = nullptr;
	u16 width = 0, height = 0;
	u32 size = 0;                 // bytes of level 0
	u8 maxLevel = 0;
	int fmt = 0;
	int wrapS = GPU_REPEAT, wrapT = GPU_REPEAT, filter = GPU_NEAREST;
	void* native = nullptr;       // the software GPU's decoded copy (native_gpu.cpp)
};
struct C3D_RenderTarget;
struct C3D_TexEnv
{
	int srcRgb[3], srcAlpha[3], opRgb[3], opAlpha[3], funcRgb, funcAlpha;
	u32 color;
};
struct C3D_AttrInfo { int count; int format[8]; int elements[8]; };
struct C3D_BufInfo { const void* data; int stride; };
struct Tex3DS_SubTexture { u16 width, height; float left, top, right, bottom; };
typedef void* Tex3DS_Texture;
struct C2D_Image { C3D_Tex* tex; const Tex3DS_SubTexture* subtex; };

// Shader programs: only renderer.cpp's (cell.v.pica, run in C++ by native_gpu.cpp)
struct DVLE_s { int unused; };
struct DVLB_s { DVLE_s DVLE[1]; };
struct shaderInstance_s { int unused; };
struct shaderProgram_s { shaderInstance_s* vertexShader; };
DVLB_s* DVLB_ParseFile(u32* data, u32 size);
void DVLB_Free(DVLB_s* dvlb);
int shaderProgramInit(shaderProgram_s* p);
int shaderProgramFree(shaderProgram_s* p);
int shaderProgramSetVsh(shaderProgram_s* p, DVLE_s* dvle);
s8 shaderInstanceGetUniformLocation(shaderInstance_s* si, const char* name);

bool C3D_Init(size_t);
void C3D_Fini();
bool C3D_FrameBegin(u8 flags);
void C3D_FrameEnd(u8 flags);
void C3D_FrameDrawOn(C3D_RenderTarget* t);
inline float C3D_GetCmdBufUsage() { return 0; }
inline float C3D_GetProcessingTime() { return 0; }
inline float C3D_GetDrawingTime() { return 0; }
C3D_RenderTarget* C3D_RenderTargetCreate(int width, int height, int colorFmt, int depthFmt);
C3D_RenderTarget* C3D_RenderTargetCreateFromTex(C3D_Tex* tex, int face, int level, int depthFmt);
void C3D_RenderTargetSetOutput(C3D_RenderTarget* t, int screen, int side, u32 flags);
void C3D_RenderTargetClear(C3D_RenderTarget* t, int bits, u32 clearColor, u32 clearDepth);
void C3D_RenderTargetDelete(C3D_RenderTarget* t);
void C3D_DepthTest(bool enable, int function, int writemask);
void C3D_DepthMap(bool bIsZBuffer, float zScale, float zOffset);
void C3D_CullFace(int mode);
void C3D_AlphaTest(bool enable, int function, int ref);
void C3D_AlphaBlend(int colorEq, int alphaEq, int srcClr, int dstClr, int srcAlpha, int dstAlpha);
void C3D_SetScissor(int mode, u32 left, u32 top, u32 right, u32 bottom);
void C3D_TexSetFilter(C3D_Tex* t, int magFilter, int minFilter);
inline void C3D_TexSetFilterMipmap(C3D_Tex*, int) {}
void C3D_TexSetWrap(C3D_Tex* t, int wrapS, int wrapT);
void C3D_TexBind(int unit, C3D_Tex* t);
void C3D_TexFlush(C3D_Tex* t);
void C3D_TexDelete(C3D_Tex* t);
bool C3D_TexInit(C3D_Tex* t, u16 w, u16 h, int fmt);
bool C3D_TexInitVRAM(C3D_Tex* t, u16 w, u16 h, int fmt);
inline u32 C3D_TexCalcTotalSize(u32 s, int) { return s; }
C3D_TexEnv* C3D_GetTexEnv(int id);
void C3D_TexEnvInit(C3D_TexEnv* env);
void C3D_TexEnvSrc(C3D_TexEnv* env, int mode, int s1, int s2 = GPU_PRIMARY_COLOR, int s3 = GPU_PRIMARY_COLOR);
void C3D_TexEnvOpRgb(C3D_TexEnv* env, int o1, int o2 = GPU_TEVOP_RGB_SRC_COLOR, int o3 = GPU_TEVOP_RGB_SRC_COLOR);
void C3D_TexEnvFunc(C3D_TexEnv* env, int mode, int func);
void C3D_TexEnvColor(C3D_TexEnv* env, u32 color);
void C3D_FVUnifSet(int type, int id, float x, float y, float z, float w);
void C3D_FVUnifMtx4x4(int type, int id, const C3D_Mtx* m);
C3D_AttrInfo* C3D_GetAttrInfo();
void AttrInfo_Init(C3D_AttrInfo* info);
int AttrInfo_AddLoader(C3D_AttrInfo* info, int regId, int format, int count);
C3D_BufInfo* C3D_GetBufInfo();
void BufInfo_Init(C3D_BufInfo* info);
int BufInfo_Add(C3D_BufInfo* info, const void* data, ptrdiff_t stride, int attribCount, u64 permutation);
void C3D_DrawElements(int primitive, int count, int type, const void* indices);
void C3D_BindProgram(shaderProgram_s* p);
Tex3DS_Texture Tex3DS_TextureImport(const void* data, size_t size, C3D_Tex* tex, void* texcube, bool vram);
Tex3DS_Texture Tex3DS_TextureImportStdio(FILE* f, C3D_Tex* tex, void* texcube, bool vram);
void Tex3DS_TextureFree(Tex3DS_Texture t);
