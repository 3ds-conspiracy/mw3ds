// The native test build's software GPU: what citro3d and citro2d draw on the 3DS, drawn on the PC, so SHOT saves
// both screens as the emulator does (the same 400 x 480 BMP). On with NATIVE_DRAW=1 (run-test.ps1 -Native sets it
// for cases with SHOT); without it every call returns at once and the textures are never read, so the plain native
// runs stay headless and fast.
//
// What it covers: .t3x textures (none / LZ10 / LZ11 / Huffman / RLE, the PICA's tiled formats and ETC1 / ETC1A4),
// render targets (the two screens, the local map texture), renderer.cpp's vertex shader (cell.v.pica) in C++,
// clipping, the texture combiners renderer.cpp sets, alpha test, depth test, blending, culling, scissor, and
// citro2d's rectangles, triangles and images. Not covered: mipmaps (level 0 only), the system font (glyphs are
// grey blocks; the game draws its own Morrowind font once the theme is loaded), stereo (left eye only).
#include <3ds.h>
#include <citro3d.h>
#include <citro2d.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "log.h"
#include "renderer.h"
#include "screenshot.h"

// NATIVE_DRAW=1: textures are read and the frames a SHOT saves are drawn (the shot is the frame drawn right after
// the SHOT step starts, one frame later than the 3DS build's, which saves the frame before); NATIVE_DRAW=all draws
// every frame (slow: for watching a whole run)
static int drawMode()
{
	static int mode = -1;
	if (mode < 0)
	{
		const char* e = getenv("NATIVE_DRAW");
		mode = !e || !*e || strcmp(e, "0") == 0 ? 0 : strcmp(e, "all") == 0 ? 2 : 1;
	}
	return mode;
}
static bool drawOn() { return drawMode() != 0; }
static bool s_drawThisFrame = false;
static std::vector<std::string> s_pendingShots;
static bool drawFrame() { return drawMode() == 2 || (drawMode() == 1 && s_drawThisFrame); }

// ---- render targets: RGBA (0xAABBGGRR, as C2D_Color32), rows top to bottom, and a depth buffer

struct C3D_RenderTarget
{
	int w = 0, h = 0;
	std::vector<u32> color;
	std::vector<float> depth;
	int screen = -1, side = -1;
	C3D_Tex* tex = nullptr;
};

static std::vector<C3D_RenderTarget*> s_targets;
static C3D_RenderTarget* s_target = nullptr;

static C3D_RenderTarget* newTarget(int w, int h)
{
	auto* t = new C3D_RenderTarget();
	t->w = w;
	t->h = h;
	t->color.assign(w * h, 0xFF000000);
	t->depth.assign(w * h, 0.0f);
	s_targets.push_back(t);
	return t;
}

// ---- textures: decoded on first use into RGBA rows, top row first (v = 1 at the top, as the subtextures say)

struct TexImage
{
	int w = 0, h = 0;
	bool valid = false;
	std::vector<u32> px;
	C3D_RenderTarget* target = nullptr;     // a render target's texture reads its colour buffer
};

static int texBits(int fmt)
{
	switch (fmt)
	{
	case GPU_RGBA8: return 32;
	case GPU_RGB8: return 24;
	case GPU_RGBA5551: case GPU_RGB565: case GPU_RGBA4: case GPU_LA8: case GPU_HILO8: return 16;
	case GPU_L8: case GPU_A8: case GPU_LA4: case GPU_ETC1A4: return 8;
	default: return 4;      // L4, A4, ETC1
	}
}

static inline u32 rgba(u32 r, u32 g, u32 b, u32 a) { return r | (g << 8) | (b << 16) | (a << 24); }

static void etc1Block(u64 v, u64 alpha, bool hasAlpha, u32 out[4][4])
{
	static const int kMod[8][4] = { { 2, 8, -2, -8 }, { 5, 17, -5, -17 }, { 9, 29, -9, -29 }, { 13, 42, -13, -42 },
		{ 18, 60, -18, -60 }, { 24, 80, -24, -80 }, { 33, 106, -33, -106 }, { 47, 183, -47, -183 } };
	bool diff = (v >> 33) & 1, flip = (v >> 32) & 1;
	int t1 = (v >> 37) & 7, t2 = (v >> 34) & 7;
	int c1[3], c2[3];
	for (int k = 0; k < 3; k++)
	{
		int shift = 59 - k * 8;
		if (diff)
		{
			int a = (v >> shift) & 31;
			int d = (v >> (shift - 3)) & 7;
			if (d >= 4)
				d -= 8;
			int b = std::clamp(a + d, 0, 31);
			c1[k] = (a << 3) | (a >> 2);
			c2[k] = (b << 3) | (b >> 2);
		}
		else
		{
			int a = (v >> (shift + 1)) & 15, b = (v >> (shift - 3)) & 15;
			c1[k] = a * 17;
			c2[k] = b * 17;
		}
	}
	for (int x = 0; x < 4; x++)
		for (int y = 0; y < 4; y++)
		{
			int i = x * 4 + y;
			bool second = flip ? y >= 2 : x >= 2;
			int idx = (((v >> (16 + i)) & 1) << 1) | ((v >> i) & 1);
			int m = kMod[second ? t2 : t1][idx];
			const int* c = second ? c2 : c1;
			u32 a = hasAlpha ? ((alpha >> (i * 4)) & 15) * 17 : 255;
			out[y][x] = rgba(std::clamp(c[0] + m, 0, 255), std::clamp(c[1] + m, 0, 255), std::clamp(c[2] + m, 0, 255), a);
		}
}

// Level 0 of a PICA texture: 8 x 8 tiles in rows, texels in Morton order inside a tile (ETC: four 4 x 4 blocks).
// The first row in memory is the top of the picture (v = 1).
static void decodeTex(const C3D_Tex* t, TexImage& img)
{
	int w = t->width, h = t->height;
	img.w = w;
	img.h = h;
	img.px.assign(w * h, 0xFFFF00FF);
	const u8* d = (const u8*)t->data;
	if (!d || w < 8 || h < 8)
		return;
	auto put = [&](int x, int row, u32 c) { img.px[row * w + x] = c; };
	int fmt = t->fmt;
	if (fmt == GPU_ETC1 || fmt == GPU_ETC1A4)
	{
		bool hasAlpha = fmt == GPU_ETC1A4;
		for (int ty = 0; ty < h / 8; ty++)
			for (int tx = 0; tx < w / 8; tx++)
				for (int b = 0; b < 4; b++)
				{
					u64 alpha = 0, v;
					if (hasAlpha)
					{
						memcpy(&alpha, d, 8);
						d += 8;
					}
					memcpy(&v, d, 8);
					d += 8;
					u32 block[4][4];
					etc1Block(v, alpha, hasAlpha, block);
					int bx = tx * 8 + (b & 1) * 4, by = ty * 8 + (b >> 1) * 4;
					for (int y = 0; y < 4; y++)
						for (int x = 0; x < 4; x++)
							put(bx + x, by + y, block[y][x]);
				}
		return;
	}
	int bits = texBits(fmt);
	for (int ty = 0; ty < h / 8; ty++)
		for (int tx = 0; tx < w / 8; tx++)
		{
			const u8* tile = d + (size_t)(ty * (w / 8) + tx) * 64 * bits / 8;
			for (int i = 0; i < 64; i++)
			{
				int x = (i & 1) | ((i >> 1) & 2) | ((i >> 2) & 4);
				int y = ((i >> 1) & 1) | ((i >> 2) & 2) | ((i >> 3) & 4);
				const u8* p = tile + i * bits / 8;
				u32 c = 0xFFFFFFFF;
				u16 s = bits == 16 ? (u16)(p[0] | (p[1] << 8)) : 0;
				switch (fmt)
				{
				case GPU_RGBA8: c = rgba(p[3], p[2], p[1], p[0]); break;
				case GPU_RGB8: c = rgba(p[2], p[1], p[0], 255); break;
				case GPU_RGBA5551: c = rgba(((s >> 11) & 31) * 255 / 31, ((s >> 6) & 31) * 255 / 31, ((s >> 1) & 31) * 255 / 31, (s & 1) * 255); break;
				case GPU_RGB565: c = rgba(((s >> 11) & 31) * 255 / 31, ((s >> 5) & 63) * 255 / 63, (s & 31) * 255 / 31, 255); break;
				case GPU_RGBA4: c = rgba(((s >> 12) & 15) * 17, ((s >> 8) & 15) * 17, ((s >> 4) & 15) * 17, (s & 15) * 17); break;
				case GPU_LA8: c = rgba(p[1], p[1], p[1], p[0]); break;
				case GPU_HILO8: c = rgba(p[1], p[0], 0, 255); break;
				case GPU_L8: c = rgba(p[0], p[0], p[0], 255); break;
				case GPU_A8: c = rgba(0, 0, 0, p[0]); break;
				case GPU_LA4: c = rgba((p[0] >> 4) * 17, (p[0] >> 4) * 17, (p[0] >> 4) * 17, (p[0] & 15) * 17); break;
				case GPU_L4: case GPU_A4:
				{
					u8 n = (tile[i / 2] >> ((i & 1) * 4)) & 15;
					c = fmt == GPU_L4 ? rgba(n * 17, n * 17, n * 17, 255) : rgba(0, 0, 0, n * 17);
					break;
				}
				}
				put(tx * 8 + x, ty * 8 + y, c);
			}
		}
}

static TexImage* texImage(C3D_Tex* t)
{
	if (!t)
		return nullptr;
	if (!t->native)
		t->native = new TexImage();
	TexImage* img = (TexImage*)t->native;
	if (!img->target && !img->valid)
	{
		decodeTex(t, *img);
		img->valid = true;
	}
	return img;
}

static void freeTexImage(C3D_Tex* t)
{
	delete (TexImage*)t->native;
	t->native = nullptr;
}

struct V4 { float r, g, b, a; };

static inline V4 unpack(u32 c)
{
	return { (c & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, ((c >> 16) & 255) / 255.0f, (c >> 24) / 255.0f };
}

static inline u32 texel(const TexImage* img, const C3D_Tex* t, int x, int y)
{
	int w = img->target ? img->target->w : img->w, h = img->target ? img->target->h : img->h;
	if (t->wrapS == GPU_REPEAT)
		x = ((x % w) + w) % w;
	else
		x = std::clamp(x, 0, w - 1);
	if (t->wrapT == GPU_REPEAT)
		y = ((y % h) + h) % h;
	else
		y = std::clamp(y, 0, h - 1);
	return img->target ? img->target->color[y * w + x] : img->px[y * w + x];
}

static V4 sample(C3D_Tex* t, float u, float v)
{
	TexImage* img = texImage(t);
	if (!img)
		return { 1, 1, 1, 1 };
	int w = img->target ? img->target->w : img->w, h = img->target ? img->target->h : img->h;
	if (w <= 0 || h <= 0)
		return { 1, 1, 1, 1 };
	float fx = u * w - 0.5f, fy = (1.0f - v) * h - 0.5f;
	if (t->filter != GPU_LINEAR)
		return unpack(texel(img, t, (int)floorf(fx + 0.5f), (int)floorf(fy + 0.5f)));
	int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
	float ax = fx - x0, ay = fy - y0;
	V4 a = unpack(texel(img, t, x0, y0)), b = unpack(texel(img, t, x0 + 1, y0));
	V4 c = unpack(texel(img, t, x0, y0 + 1)), d = unpack(texel(img, t, x0 + 1, y0 + 1));
	auto lerp = [](float p, float q, float k) { return p + (q - p) * k; };
	return { lerp(lerp(a.r, b.r, ax), lerp(c.r, d.r, ax), ay), lerp(lerp(a.g, b.g, ax), lerp(c.g, d.g, ax), ay),
		lerp(lerp(a.b, b.b, ax), lerp(c.b, d.b, ax), ay), lerp(lerp(a.a, b.a, ax), lerp(c.a, d.a, ax), ay) };
}

// ---- .t3x: header, subtextures, then the texture data behind a GBA-style compression header

static std::vector<u8> decompress(const u8* d, size_t n)
{
	std::vector<u8> out;
	if (n < 4)
		return out;
	u8 type = d[0];
	size_t size = d[1] | (d[2] << 8) | (d[3] << 16), p = 4;
	if (size == 0 && n >= 8)
	{
		size = d[4] | (d[5] << 8) | (d[6] << 16) | ((size_t)d[7] << 24);
		p = 8;
	}
	out.reserve(size);
	auto back = [&](size_t disp, size_t len) {
		for (size_t i = 0; i < len && out.size() < size; i++)
			out.push_back(disp <= out.size() ? out[out.size() - disp] : 0);
	};
	if (type == 0x00)
		out.assign(d + p, d + std::min(n, p + size));
	else if (type == 0x10 || type == 0x11)
	{
		while (out.size() < size && p < n)
		{
			u8 flags = d[p++];
			for (int bit = 7; bit >= 0 && out.size() < size && p < n; bit--)
			{
				if (!(flags & (1 << bit)))
				{
					out.push_back(d[p++]);
					continue;
				}
				size_t len, disp;
				u8 b0 = d[p];
				if (type == 0x10)
				{
					len = (b0 >> 4) + 3;
					disp = (((b0 & 15) << 8) | d[p + 1]) + 1;
					p += 2;
				}
				else if ((b0 >> 4) == 0)
				{
					len = (((b0 & 15) << 4) | (d[p + 1] >> 4)) + 0x11;
					disp = (((d[p + 1] & 15) << 8) | d[p + 2]) + 1;
					p += 3;
				}
				else if ((b0 >> 4) == 1)
				{
					len = (((b0 & 15) << 12) | (d[p + 1] << 4) | (d[p + 2] >> 4)) + 0x111;
					disp = (((d[p + 2] & 15) << 8) | d[p + 3]) + 1;
					p += 4;
				}
				else
				{
					len = (b0 >> 4) + 1;
					disp = (((b0 & 15) << 8) | d[p + 1]) + 1;
					p += 2;
				}
				back(disp, len);
			}
		}
	}
	else if ((type & 0xF0) == 0x20)
	{
		// Huffman: a tree of 8-bit nodes (offset to the children in bits 0-5, "child is data" in bits 7 / 6),
		// then 32-bit words read from the top bit
		int symBits = type & 15;
		size_t treeAt = p, root = p + 1, stream = p + (d[p] + 1) * 2;
		size_t node = root;
		bool low = true;
		u8 pending = 0;
		while (out.size() < size && stream + 4 <= n)
		{
			u32 word = d[stream] | (d[stream + 1] << 8) | (d[stream + 2] << 16) | ((u32)d[stream + 3] << 24);
			stream += 4;
			for (int bit = 31; bit >= 0 && out.size() < size; bit--)
			{
				int b = (word >> bit) & 1;
				u8 nd = d[node];
				size_t child = (node & ~(size_t)1) + (nd & 0x3F) * 2 + 2 + b;
				bool leaf = b ? (nd & 0x40) : (nd & 0x80);
				if (!leaf)
				{
					node = child;
					continue;
				}
				u8 sym = d[child];
				if (symBits == 8)
					out.push_back(sym);
				else if (low)
				{
					pending = sym & 15;
					low = false;
				}
				else
				{
					out.push_back(pending | ((sym & 15) << 4));
					low = true;
				}
				node = root;
			}
		}
		(void)treeAt;
	}
	else if (type == 0x30)
	{
		while (out.size() < size && p < n)
		{
			u8 f = d[p++];
			if (f & 0x80)
			{
				size_t len = (f & 0x7F) + 3;
				u8 v = d[p++];
				for (size_t i = 0; i < len && out.size() < size; i++)
					out.push_back(v);
			}
			else
				for (size_t i = 0, len = (f & 0x7F) + 1; i < len && out.size() < size && p < n; i++)
					out.push_back(d[p++]);
		}
	}
	else
		logf("native gpu: unknown texture compression 0x%02x", type);
	out.resize(size);
	return out;
}

struct T3x { std::vector<Tex3DS_SubTexture> subs; };

static Tex3DS_Texture importT3x(const u8* d, size_t n, C3D_Tex* tex)
{
	if (n < 5)
		return nullptr;
	int count = d[0] | (d[1] << 8);
	int wl = d[2] & 7, hl = (d[2] >> 3) & 7, fmt = d[3], mips = d[4];
	size_t p = 5;
	auto* t = new T3x();
	for (int i = 0; i < count && p + 12 <= n; i++, p += 12)
	{
		u16 v[6];
		for (int k = 0; k < 6; k++)
			v[k] = d[p + k * 2] | (d[p + k * 2 + 1] << 8);
		t->subs.push_back({ v[0], v[1], v[2] / 1024.0f, v[3] / 1024.0f, v[4] / 1024.0f, v[5] / 1024.0f });
	}
	std::vector<u8> data = decompress(d + p, n - p);
	C3D_TexInit(tex, 1 << (wl + 3), 1 << (hl + 3), fmt);
	tex->maxLevel = mips;
	memcpy(tex->data, data.data(), std::min<size_t>(data.size(), tex->size));
	return (Tex3DS_Texture)t;
}

Tex3DS_Texture Tex3DS_TextureImport(const void* data, size_t size, C3D_Tex* tex, void*, bool)
{
	if (!drawOn())
		return nullptr;
	return importT3x((const u8*)data, size, tex);
}

Tex3DS_Texture Tex3DS_TextureImportStdio(FILE* f, C3D_Tex* tex, void*, bool)
{
	if (!drawOn())
	{
		tex->width = tex->height = 8;       // every texture "loads", nothing is read
		return (Tex3DS_Texture)1;
	}
	std::vector<u8> buf;
	u8 chunk[65536];
	size_t got;
	while ((got = fread(chunk, 1, sizeof(chunk), f)) > 0)
		buf.insert(buf.end(), chunk, chunk + got);
	return importT3x(buf.data(), buf.size(), tex);
}

void Tex3DS_TextureFree(Tex3DS_Texture t)
{
	if (t && t != (Tex3DS_Texture)1)
		delete (T3x*)t;
}

bool C3D_TexInit(C3D_Tex* t, u16 w, u16 h, int fmt)
{
	t->width = w;
	t->height = h;
	t->fmt = fmt;
	t->size = (u32)w * h * texBits(fmt) / 8;
	t->maxLevel = 0;
	t->wrapS = t->wrapT = GPU_CLAMP_TO_EDGE;
	t->filter = GPU_NEAREST;
	t->data = calloc(1, t->size ? t->size : 1);
	t->native = nullptr;
	return t->data != nullptr;
}

bool C3D_TexInitVRAM(C3D_Tex* t, u16 w, u16 h, int fmt) { return C3D_TexInit(t, w, h, fmt); }

void C3D_TexDelete(C3D_Tex* t)
{
	free(t->data);
	t->data = nullptr;
	freeTexImage(t);
}

void C3D_TexFlush(C3D_Tex* t)
{
	if (t->native)
		((TexImage*)t->native)->valid = false;
}

void C3D_TexSetFilter(C3D_Tex* t, int mag, int) { t->filter = mag; }
void C3D_TexSetWrap(C3D_Tex* t, int s, int tt) { t->wrapS = s; t->wrapT = tt; }

// ---- GPU state

static bool s_depthOn = false, s_depthWrite = true;
static int s_depthFunc = GPU_GREATER;
static float s_zScale = -1.0f, s_zOffset = 0.0f;
static int s_cull = GPU_CULL_NONE;
static bool s_alphaTestOn = false;
static int s_alphaFunc = GPU_ALWAYS, s_alphaRef = 0;
static int s_blend[4] = { GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA };
static bool s_scissorOn = false;
static u32 s_scissor[4];
static C3D_Tex* s_units[3];
static C3D_TexEnv s_env[6];
static float s_unif[96][4];
static C3D_AttrInfo s_attr;
static C3D_BufInfo s_buf;
static float s_view2d[2];

bool C3D_Init(size_t)
{
	for (auto& e : s_env)
		C3D_TexEnvInit(&e);
	return true;
}
void C3D_Fini() {}
static bool writeShot(const char* path);
bool C3D_FrameBegin(u8)
{
	s_drawThisFrame = false;
	return true;
}
void C3D_FrameEnd(u8)
{
	for (auto& path : s_pendingShots)
		if (!writeShot(path.c_str()))
			logf("native gpu: could not write %s", path.c_str());
	s_pendingShots.clear();
	s_drawThisFrame = false;
}
void C3D_FrameDrawOn(C3D_RenderTarget* t) { s_target = t; }

C3D_RenderTarget* C3D_RenderTargetCreate(int width, int height, int, int)
{
	return newTarget(height, width);         // made sideways, as the screens are
}

C3D_RenderTarget* C3D_RenderTargetCreateFromTex(C3D_Tex* tex, int, int, int)
{
	C3D_RenderTarget* t = newTarget(tex->width, tex->height);
	t->tex = tex;
	if (!tex->native)
		tex->native = new TexImage();
	((TexImage*)tex->native)->target = t;
	return t;
}

void C3D_RenderTargetSetOutput(C3D_RenderTarget* t, int screen, int side, u32)
{
	t->screen = screen;
	t->side = side;
}

C3D_RenderTarget* C2D_CreateScreenTarget(int screen, int side)
{
	C3D_RenderTarget* t = newTarget(screen == GFX_TOP ? 400 : 320, 240);
	C3D_RenderTargetSetOutput(t, screen, side, 0);
	return t;
}

void C3D_RenderTargetDelete(C3D_RenderTarget* t)
{
	s_targets.erase(std::remove(s_targets.begin(), s_targets.end(), t), s_targets.end());
	if (t->tex && t->tex->native)
		((TexImage*)t->tex->native)->target = nullptr;
	if (s_target == t)
		s_target = nullptr;
	delete t;
}

static void clearTarget(C3D_RenderTarget* t, bool color, bool depth, u32 c, float z)
{
	if (!drawFrame() || !t)
		return;
	if (color)
		std::fill(t->color.begin(), t->color.end(), c);
	if (depth)
		std::fill(t->depth.begin(), t->depth.end(), z);
}

void C3D_RenderTargetClear(C3D_RenderTarget* t, int bits, u32 c, u32 depth)
{
	// citro3d's clear colour is 0xRRGGBBAA
	clearTarget(t, bits != C3D_CLEAR_DEPTH, bits != C3D_CLEAR_COLOR, rgba(c >> 24, (c >> 16) & 255, (c >> 8) & 255, c & 255),
		(depth & 0xFFFFFF) / 16777215.0f);
}

void C2D_TargetClear(C3D_RenderTarget* t, u32 color) { clearTarget(t, true, true, color, 0.0f); }
void C2D_SceneBegin(C3D_RenderTarget* t) { s_target = t; }
void C2D_Prepare() {}
void C2D_ViewReset() { s_view2d[0] = s_view2d[1] = 0.0f; }
void C2D_ViewTranslate(float x, float y) { s_view2d[0] += x; s_view2d[1] += y; }

void C3D_DepthTest(bool enable, int function, int writemask)
{
	s_depthOn = enable;
	s_depthFunc = function;
	s_depthWrite = writemask == GPU_WRITE_ALL;
}
void C3D_DepthMap(bool, float zScale, float zOffset) { s_zScale = zScale; s_zOffset = zOffset; }
void C3D_CullFace(int mode) { s_cull = mode; }
void C3D_AlphaTest(bool enable, int function, int ref) { s_alphaTestOn = enable; s_alphaFunc = function; s_alphaRef = ref; }
void C3D_AlphaBlend(int, int, int srcClr, int dstClr, int srcAlpha, int dstAlpha)
{
	s_blend[0] = srcClr;
	s_blend[1] = dstClr;
	s_blend[2] = srcAlpha;
	s_blend[3] = dstAlpha;
}
void C3D_SetScissor(int mode, u32 left, u32 top, u32 right, u32 bottom)
{
	s_scissorOn = mode == GPU_SCISSOR_NORMAL;
	s_scissor[0] = left;
	s_scissor[1] = top;
	s_scissor[2] = right;
	s_scissor[3] = bottom;
}
void C3D_TexBind(int unit, C3D_Tex* t)
{
	if (unit >= 0 && unit < 3)
		s_units[unit] = t;
}

C3D_TexEnv* C3D_GetTexEnv(int id) { return &s_env[std::clamp(id, 0, 5)]; }
void C3D_TexEnvInit(C3D_TexEnv* env)
{
	for (int i = 0; i < 3; i++)
	{
		env->srcRgb[i] = env->srcAlpha[i] = i == 0 ? GPU_PREVIOUS : GPU_PRIMARY_COLOR;
		env->opRgb[i] = GPU_TEVOP_RGB_SRC_COLOR;
		env->opAlpha[i] = GPU_TEVOP_A_SRC_ALPHA;
	}
	env->funcRgb = env->funcAlpha = GPU_REPLACE;
	env->color = 0xFFFFFFFF;
}
void C3D_TexEnvSrc(C3D_TexEnv* env, int mode, int s1, int s2, int s3)
{
	int s[3] = { s1, s2, s3 };
	for (int i = 0; i < 3; i++)
	{
		if (mode & C3D_RGB)
			env->srcRgb[i] = s[i];
		if (mode & C3D_Alpha)
			env->srcAlpha[i] = s[i];
	}
}
void C3D_TexEnvOpRgb(C3D_TexEnv* env, int o1, int o2, int o3)
{
	env->opRgb[0] = o1;
	env->opRgb[1] = o2;
	env->opRgb[2] = o3;
}
void C3D_TexEnvFunc(C3D_TexEnv* env, int mode, int func)
{
	if (mode & C3D_RGB)
		env->funcRgb = func;
	if (mode & C3D_Alpha)
		env->funcAlpha = func;
}
void C3D_TexEnvColor(C3D_TexEnv* env, u32 color) { env->color = color; }

// ---- renderer.cpp's shader: uniform slots by name, as cell.v.pica declares them
enum { U_PROJECTION = 0, U_MODELVIEW = 4, U_UVOFFSET = 8, U_FOGVEC, U_POSOFFSET, U_POSSCALE, U_UVSCALE, U_TINT, U_LIGHTPOS,
	U_LIGHTCOLOR };

DVLB_s* DVLB_ParseFile(u32*, u32) { return new DVLB_s(); }
void DVLB_Free(DVLB_s* d) { delete d; }
int shaderProgramInit(shaderProgram_s* p) { static shaderInstance_s si; p->vertexShader = &si; return 0; }
int shaderProgramFree(shaderProgram_s*) { return 0; }
int shaderProgramSetVsh(shaderProgram_s*, DVLE_s*) { return 0; }
s8 shaderInstanceGetUniformLocation(shaderInstance_s*, const char* name)
{
	static const struct { const char* name; int slot; } kSlots[] = { { "projection", U_PROJECTION },
		{ "modelView", U_MODELVIEW }, { "uvOffset", U_UVOFFSET }, { "fogVec", U_FOGVEC }, { "posOffset", U_POSOFFSET },
		{ "posScale", U_POSSCALE }, { "uvScale", U_UVSCALE }, { "tint", U_TINT }, { "lightPos", U_LIGHTPOS },
		{ "lightColor", U_LIGHTCOLOR } };
	for (auto& s : kSlots)
		if (strcmp(s.name, name) == 0)
			return s.slot;
	return -1;
}
void C3D_BindProgram(shaderProgram_s*) {}

void C3D_FVUnifSet(int, int id, float x, float y, float z, float w)
{
	if (id < 0 || id >= 96)
		return;
	s_unif[id][0] = x;
	s_unif[id][1] = y;
	s_unif[id][2] = z;
	s_unif[id][3] = w;
}
void C3D_FVUnifMtx4x4(int, int id, const C3D_Mtx* m)
{
	for (int r = 0; r < 4; r++)
		C3D_FVUnifSet(GPU_VERTEX_SHADER, id + r, m->r[r].x, m->r[r].y, m->r[r].z, m->r[r].w);
}

C3D_AttrInfo* C3D_GetAttrInfo() { return &s_attr; }
void AttrInfo_Init(C3D_AttrInfo* info) { info->count = 0; }
int AttrInfo_AddLoader(C3D_AttrInfo* info, int, int format, int count)
{
	if (info->count >= 8)
		return -1;
	info->format[info->count] = format;
	info->elements[info->count] = count;
	return info->count++;
}
C3D_BufInfo* C3D_GetBufInfo() { return &s_buf; }
void BufInfo_Init(C3D_BufInfo* info) { info->data = nullptr; info->stride = 0; }
int BufInfo_Add(C3D_BufInfo* info, const void* data, ptrdiff_t stride, int, u64)
{
	info->data = data;
	info->stride = (int)stride;
	return 0;
}

// ---- rasterizer: one triangle in target pixels; attributes interpolated (perspective-correct when asked)

static const int kAttrs = 8;
struct SV { float x, y, z, iw; float a[kAttrs]; };

static void scissorRect(const C3D_RenderTarget* t, int& x0, int& y0, int& x1, int& y1)
{
	x0 = 0;
	y0 = 0;
	x1 = t->w;
	y1 = t->h;
	if (!s_scissorOn)
		return;
	// The game passes the framebuffer's sideways coordinates: left / right run along the 240 rows from the
	// bottom, top / bottom along the width from the right
	x0 = std::max(x0, t->w - (int)s_scissor[3]);
	x1 = std::min(x1, t->w - (int)s_scissor[1]);
	y0 = std::max(y0, 240 - (int)s_scissor[2]);
	y1 = std::min(y1, 240 - (int)s_scissor[0]);
}

template <class Frag>
static void rasterTri(const C3D_RenderTarget* t, const SV& a0, const SV& a1, const SV& a2, int na, bool persp, Frag frag)
{
	float area = (a1.x - a0.x) * (a2.y - a0.y) - (a2.x - a0.x) * (a1.y - a0.y);
	if (fabsf(area) < 1e-9f)
		return;
	// One winding (positive area), so a pixel on an edge shared by two triangles goes to exactly one of them:
	// the one for which that edge is a left or top edge (a blended panel's diagonal is not drawn twice)
	const SV& v0 = a0;
	const SV& v1 = area > 0 ? a1 : a2;
	const SV& v2 = area > 0 ? a2 : a1;
	area = fabsf(area);
	auto owns = [](const SV& a, const SV& b) { return b.y < a.y || (b.y == a.y && b.x > a.x); };
	bool own0 = owns(v1, v2), own1 = owns(v2, v0), own2 = owns(v0, v1);
	int sx0, sy0, sx1, sy1;
	scissorRect(t, sx0, sy0, sx1, sy1);
	int x0 = std::max(sx0, (int)floorf(std::min({ v0.x, v1.x, v2.x })));
	int x1 = std::min(sx1 - 1, (int)ceilf(std::max({ v0.x, v1.x, v2.x })));
	int y0 = std::max(sy0, (int)floorf(std::min({ v0.y, v1.y, v2.y })));
	int y1 = std::min(sy1 - 1, (int)ceilf(std::max({ v0.y, v1.y, v2.y })));
	if (x0 > x1 || y0 > y1)
		return;
	float inv = 1.0f / area;
	// Edge functions (areas opposite each vertex) at each pixel centre, evaluated directly so ties are exact
	auto edge = [](const SV& a, const SV& b, float px, float py) { return (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x); };
	float att[kAttrs];
	for (int y = y0; y <= y1; y++)
	{
		float py = y + 0.5f;
		for (int x = x0; x <= x1; x++)
		{
			float px = x + 0.5f;
			float w0 = edge(v1, v2, px, py), w1 = edge(v2, v0, px, py), w2 = edge(v0, v1, px, py);
			if (w0 < 0 || w1 < 0 || w2 < 0 || (w0 == 0 && !own0) || (w1 == 0 && !own1) || (w2 == 0 && !own2))
				continue;
			float b0 = w0 * inv, b1 = w1 * inv, b2 = w2 * inv;
			float z = b0 * v0.z + b1 * v1.z + b2 * v2.z;
			if (persp)
			{
				float iw = b0 * v0.iw + b1 * v1.iw + b2 * v2.iw;
				float k = iw != 0.0f ? 1.0f / iw : 0.0f;
				for (int i = 0; i < na; i++)
					att[i] = (b0 * v0.a[i] + b1 * v1.a[i] + b2 * v2.a[i]) * k;
			}
			else
				for (int i = 0; i < na; i++)
					att[i] = b0 * v0.a[i] + b1 * v1.a[i] + b2 * v2.a[i];
			frag(x, y, z, att);
		}
	}
}

static inline float factor(int f, float srcA)
{
	switch (f)
	{
	case GPU_ZERO: return 0.0f;
	case GPU_SRC_ALPHA: return srcA;
	case GPU_ONE_MINUS_SRC_ALPHA: return 1.0f - srcA;
	default: return 1.0f;
	}
}

static inline u32 blendPixel(u32 dstPx, V4 s, const int* bl)
{
	V4 d = unpack(dstPx);
	float fs = factor(bl[0], s.a), fd = factor(bl[1], s.a), fsa = factor(bl[2], s.a), fda = factor(bl[3], s.a);
	auto q = [](float v) { return (u32)(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
	return rgba(q(s.r * fs + d.r * fd), q(s.g * fs + d.g * fd), q(s.b * fs + d.b * fd), q(s.a * fsa + d.a * fda));
}

static inline bool compare(int func, float a, float b)
{
	switch (func)
	{
	case GPU_NEVER: return false;
	case GPU_EQUAL: return a == b;
	case GPU_NOTEQUAL: return a != b;
	case GPU_LESS: return a < b;
	case GPU_LEQUAL: return a <= b;
	case GPU_GREATER: return a > b;
	case GPU_GEQUAL: return a >= b;
	default: return true;
	}
}

// ---- 3D draws (renderer.cpp)

static inline float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

// cell.v.pica: position from the packed or float input, model-view, projection; texture coordinate; fog; baked
// colour times the tint, plus the carried light
struct ClipV { float p[4]; float a[kAttrs]; };     // a: r g b a u v fog

static void readAttr(const u8* v, int fmt, int n, float out[4])
{
	out[0] = out[1] = out[2] = 0.0f;
	out[3] = 1.0f;
	for (int i = 0; i < n && i < 4; i++)
	{
		if (fmt == GPU_FLOAT)
		{
			float f;
			memcpy(&f, v + i * 4, 4);
			out[i] = f;
		}
		else if (fmt == GPU_SHORT)
		{
			s16 s;
			memcpy(&s, v + i * 2, 2);
			out[i] = s;
		}
		else if (fmt == GPU_BYTE)
			out[i] = (s8)v[i];
		else
			out[i] = v[i];
	}
}

static int attrSize(int fmt) { return fmt == GPU_FLOAT ? 4 : fmt == GPU_SHORT ? 2 : 1; }

static ClipV vertexShader(const u8* v)
{
	float in[3][4] = {};
	int off = 0;
	for (int i = 0; i < s_attr.count && i < 3; i++)
	{
		readAttr(v + off, s_attr.format[i], s_attr.elements[i], in[i]);
		off += attrSize(s_attr.format[i]) * s_attr.elements[i];
	}
	const float (*u)[4] = s_unif;
	float r0[4] = { u[U_POSOFFSET][0] + in[0][0] * u[U_POSSCALE][0], u[U_POSOFFSET][1] + in[0][1] * u[U_POSSCALE][1],
		u[U_POSOFFSET][2] + in[0][2] * u[U_POSSCALE][2], 1.0f };
	float r1[4];
	for (int i = 0; i < 4; i++)
		r1[i] = u[U_MODELVIEW + i][0] * r0[0] + u[U_MODELVIEW + i][1] * r0[1] + u[U_MODELVIEW + i][2] * r0[2]
			+ u[U_MODELVIEW + i][3] * r0[3];
	ClipV o;
	for (int i = 0; i < 4; i++)
		o.p[i] = u[U_PROJECTION + i][0] * r1[0] + u[U_PROJECTION + i][1] * r1[1] + u[U_PROJECTION + i][2] * r1[2]
			+ u[U_PROJECTION + i][3] * r1[3];
	o.a[4] = u[U_UVOFFSET][0] + in[1][0] * u[U_UVSCALE][0];
	o.a[5] = u[U_UVOFFSET][1] + in[1][1] * u[U_UVSCALE][1];
	o.a[6] = u[U_FOGVEC][0] * r1[0] + u[U_FOGVEC][1] * r1[1] + u[U_FOGVEC][2] * r1[2] + u[U_FOGVEC][3] * r1[3];
	float d[3] = { r1[0] - u[U_LIGHTPOS][0], r1[1] - u[U_LIGHTPOS][1], r1[2] - u[U_LIGHTPOS][2] };
	float fall = std::max(0.0f, 1.0f - (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) * u[U_LIGHTPOS][3]);
	for (int k = 0; k < 4; k++)
	{
		float c = in[2][k] / 255.0f * u[U_TINT][k];
		if (k < 3)
			c += u[U_LIGHTCOLOR][k] * fall;
		o.a[k] = clamp01(c);
	}
	o.a[7] = 0.0f;
	return o;
}

// Clip a polygon to the PICA's depth range: -w <= z <= 0
static int clipPoly(ClipV* in, int n, ClipV* out)
{
	ClipV tmp[16];
	ClipV* src = in;
	ClipV* dst = tmp;
	for (int plane = 0; plane < 2; plane++)
	{
		int m = 0;
		auto dist = [plane](const ClipV& v) { return plane == 0 ? v.p[2] + v.p[3] : -v.p[2]; };
		for (int i = 0; i < n; i++)
		{
			const ClipV& a = src[i];
			const ClipV& b = src[(i + 1) % n];
			float da = dist(a), db = dist(b);
			if (da >= 0)
				dst[m++] = a;
			if ((da >= 0) != (db >= 0))
			{
				float k = da / (da - db);
				ClipV c;
				for (int j = 0; j < 4; j++)
					c.p[j] = a.p[j] + (b.p[j] - a.p[j]) * k;
				for (int j = 0; j < kAttrs; j++)
					c.a[j] = a.a[j] + (b.a[j] - a.a[j]) * k;
				dst[m++] = c;
			}
		}
		n = m;
		src = dst;
		dst = plane == 0 ? out : tmp;
	}
	if (src != out)
		memcpy(out, src, n * sizeof(ClipV));
	return n;
}

static V4 tevSource(int s, const V4& tex0, const V4& tex1, const V4& prim, const V4& cons, const V4& prev)
{
	switch (s)
	{
	case GPU_TEXTURE0: return tex0;
	case GPU_TEXTURE1: return tex1;
	case GPU_PRIMARY_COLOR: return prim;
	case GPU_CONSTANT: return cons;
	default: return prev;
	}
}

static inline float tevFunc(int f, float a, float b, float c)
{
	switch (f)
	{
	case GPU_MODULATE: return a * b;
	case GPU_ADD: return std::min(1.0f, a + b);
	case GPU_INTERPOLATE: return a * c + b * (1.0f - c);
	default: return a;
	}
}

static bool usesSource(int s)
{
	for (auto& e : s_env)
		for (int i = 0; i < 3; i++)
			if (e.srcRgb[i] == s || e.srcAlpha[i] == s)
				return true;
	return false;
}

void C3D_DrawElements(int, int count, int type, const void* indices)
{
	C3D_RenderTarget* t = s_target;
	if (!drawFrame() || !t || !s_buf.data || count < 3)
		return;
	const u8* verts = (const u8*)s_buf.data;
	auto index = [&](int i) { return type == C3D_UNSIGNED_BYTE ? ((const u8*)indices)[i] : ((const u16*)indices)[i]; };
	int maxIndex = 0;
	for (int i = 0; i < count; i++)
		maxIndex = std::max(maxIndex, (int)index(i));
	static std::vector<ClipV> xf;
	xf.resize(maxIndex + 1);
	for (int i = 0; i <= maxIndex; i++)
		xf[i] = vertexShader(verts + (size_t)i * s_buf.stride);

	bool need0 = usesSource(GPU_TEXTURE0) && s_units[0], need1 = usesSource(GPU_TEXTURE1) && s_units[1];
	V4 consts[6];
	for (int i = 0; i < 6; i++)
		consts[i] = unpack(s_env[i].color);
	auto frag = [&](int x, int y, float z, const float* a) {
		size_t at = (size_t)y * t->w + x;
		if (s_depthOn && !compare(s_depthFunc, z, t->depth[at]))
			return;
		V4 prim = { a[0], a[1], a[2], a[3] };
		V4 tex0 = need0 ? sample(s_units[0], a[4], a[5]) : V4{ 1, 1, 1, 1 };
		V4 tex1 = need1 ? sample(s_units[1], a[6], 0.5f) : V4{ 1, 1, 1, 1 };
		V4 prev = { 0, 0, 0, 0 };
		for (int st = 0; st < 6; st++)
		{
			const C3D_TexEnv& e = s_env[st];
			float rgb[3][3], al[3];
			for (int i = 0; i < 3; i++)
			{
				V4 s = tevSource(e.srcRgb[i], tex0, tex1, prim, consts[st], prev);
				if (e.opRgb[i] == GPU_TEVOP_RGB_SRC_ALPHA)
					rgb[i][0] = rgb[i][1] = rgb[i][2] = s.a;
				else
				{
					rgb[i][0] = s.r;
					rgb[i][1] = s.g;
					rgb[i][2] = s.b;
				}
				al[i] = tevSource(e.srcAlpha[i], tex0, tex1, prim, consts[st], prev).a;
			}
			V4 out;
			out.r = tevFunc(e.funcRgb, rgb[0][0], rgb[1][0], rgb[2][0]);
			out.g = tevFunc(e.funcRgb, rgb[0][1], rgb[1][1], rgb[2][1]);
			out.b = tevFunc(e.funcRgb, rgb[0][2], rgb[1][2], rgb[2][2]);
			out.a = tevFunc(e.funcAlpha, al[0], al[1], al[2]);
			prev = out;
		}
		if (s_alphaTestOn && !compare(s_alphaFunc, (float)(int)(prev.a * 255.0f + 0.5f), (float)s_alphaRef))
			return;
		t->color[at] = blendPixel(t->color[at], prev, s_blend);
		if (s_depthOn && s_depthWrite)
			t->depth[at] = z;
	};

	for (int i = 0; i + 2 < count; i += 3)
	{
		ClipV poly[3] = { xf[index(i)], xf[index(i + 1)], xf[index(i + 2)] };
		ClipV clipped[16];
		int n;
		bool inside = true;
		for (auto& v : poly)
			if (v.p[2] + v.p[3] < 0 || v.p[2] > 0)
				inside = false;
		if (inside)
		{
			memcpy(clipped, poly, sizeof(poly));
			n = 3;
		}
		else
			n = clipPoly(poly, 3, clipped);
		if (n < 3)
			continue;
		SV sv[16];
		for (int k = 0; k < n; k++)
		{
			const ClipV& c = clipped[k];
			float iw = c.p[3] != 0.0f ? 1.0f / c.p[3] : 0.0f;
			sv[k].x = (c.p[0] * iw + 1.0f) * 0.5f * t->w;
			sv[k].y = (1.0f - c.p[1] * iw) * 0.5f * t->h;
			sv[k].z = s_zScale * c.p[2] * iw + s_zOffset;
			sv[k].iw = iw;
			for (int j = 0; j < kAttrs; j++)
				sv[k].a[j] = c.a[j] * iw;
		}
		// Winding on the screen (y down): the PICA's front faces are counter-clockwise in its clip space
		float area = (sv[1].x - sv[0].x) * (sv[2].y - sv[0].y) - (sv[2].x - sv[0].x) * (sv[1].y - sv[0].y);
		if (s_cull == GPU_CULL_BACK_CCW && area > 0)
			continue;
		for (int k = 1; k + 1 < n; k++)
			rasterTri(t, sv[0], sv[k], sv[k + 1], 7, true, frag);
	}
}

// ---- citro2d: screen pixels, alpha blended, no depth; images tinted as citro2d does (rgb towards the tint by
// its blend, alpha multiplied)

static void draw2D(SV v[4], int count, C3D_Tex* tex)
{
	C3D_RenderTarget* t = s_target;
	if (!t)
		return;
	for (int i = 0; i < count; i++)
	{
		v[i].x += s_view2d[0];
		v[i].y += s_view2d[1];
	}
	static const int kStdBlend[4] = { GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA };
	auto frag = [&](int x, int y, float, const float* a) {
		V4 c = { a[0], a[1], a[2], a[3] };
		if (tex)
		{
			V4 s = sample(tex, a[4], a[5]);
			float k = a[6];
			c = { s.r + (c.r - s.r) * k, s.g + (c.g - s.g) * k, s.b + (c.b - s.b) * k, s.a * c.a };
		}
		size_t at = (size_t)y * t->w + x;
		t->color[at] = blendPixel(t->color[at], c, kStdBlend);
	};
	rasterTri(t, v[0], v[1], v[2], 7, false, frag);
	if (count == 4)
		rasterTri(t, v[2], v[1], v[3], 7, false, frag);
}

static SV sv2(float x, float y, u32 color, float u = 0, float v = 0, float blend = 0)
{
	V4 c = unpack(color);
	return { x, y, 0.0f, 1.0f, { c.r, c.g, c.b, c.a, u, v, blend, 0 } };
}

bool C2D_DrawRectSolid(float x, float y, float, float w, float h, u32 clr)
{
	if (!drawFrame())
		return true;
	SV v[4] = { sv2(x, y, clr), sv2(x + w, y, clr), sv2(x, y + h, clr), sv2(x + w, y + h, clr) };
	draw2D(v, 4, nullptr);
	return true;
}

bool C2D_DrawTriangle(float x0, float y0, u32 c0, float x1, float y1, u32 c1, float x2, float y2, u32 c2, float)
{
	if (!drawFrame())
		return true;
	SV v[4] = { sv2(x0, y0, c0), sv2(x1, y1, c1), sv2(x2, y2, c2) };
	draw2D(v, 3, nullptr);
	return true;
}

// Corners top left, top right, bottom left, bottom right, around (cx, cy) by angle
static bool drawImage(C2D_Image img, float cx, float cy, float hw, float hh, float angle, const C2D_ImageTint* tint)
{
	if (!drawFrame() || !img.tex || !img.subtex)
		return true;
	const Tex3DS_SubTexture* s = img.subtex;
	float c = cosf(angle), sn = sinf(angle);
	static const float kCorner[4][2] = { { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } };
	float uv[4][2] = { { s->left, s->top }, { s->right, s->top }, { s->left, s->bottom }, { s->right, s->bottom } };
	SV v[4];
	for (int i = 0; i < 4; i++)
	{
		float dx = kCorner[i][0] * hw, dy = kCorner[i][1] * hh;
		u32 color = tint ? tint->corners[i].color : 0xFFFFFFFF;
		float blend = tint ? tint->corners[i].blend : 0.0f;
		v[i] = sv2(cx + dx * c - dy * sn, cy + dx * sn + dy * c, color, uv[i][0], uv[i][1], blend);
	}
	draw2D(v, 4, img.tex);
	return true;
}

bool C2D_DrawImageAt(C2D_Image img, float x, float y, float, const C2D_ImageTint* tint, float sx, float sy)
{
	if (!img.subtex)
		return true;
	float w = img.subtex->width * sx, h = img.subtex->height * sy;
	return drawImage(img, x + w / 2, y + h / 2, w / 2, h / 2, 0.0f, tint);
}

bool C2D_DrawImageAtRotated(C2D_Image img, float x, float y, float, float angle, const C2D_ImageTint* tint, float sx, float sy)
{
	if (!img.subtex)
		return true;
	return drawImage(img, x, y, img.subtex->width * sx / 2, img.subtex->height * sy / 2, angle, tint);
}

void C2D_DrawText(const C2D_Text* t, u32, float x, float y, float, float sx, float sy, ...)
{
	if (!drawFrame() || !t || !t->s)
		return;
	va_list args;
	va_start(args, sy);
	u32 color = va_arg(args, u32);
	va_end(args);
	// No system font on the PC: each glyph a block of its advance (13, native_compat.h) under the line feed (30)
	float pen = x;
	for (const char* p = t->s; *p; p++)
	{
		if ((u8)*p >= 0x80 && ((u8)*p & 0xC0) == 0x80)
			continue;
		if (*p != ' ')
			C2D_DrawRectSolid(pen + 2 * sx, y + 8 * sy, 0.5f, 9 * sx, 16 * sy, (color & 0x00FFFFFF) | 0x90000000);
		pen += 13 * sx;
	}
}

// ---- SHOT: both screens as one 400 x 480 BMP, the top screen above the bottom (as screenshot.cpp writes)

bool screenshotSave(const char* path)
{
	if (!drawOn())
		return false;
	if (drawMode() == 2)
		return writeShot(path);
	// Called as a frame begins: draw this frame and save it when it ends
	s_pendingShots.push_back(path);
	s_drawThisFrame = true;
	return true;
}

static bool writeShot(const char* path)
{
	const C3D_RenderTarget *top = nullptr, *bottom = nullptr;
	for (auto* t : s_targets)
	{
		if (t->screen == GFX_TOP && t->side == GFX_LEFT)
			top = t;
		if (t->screen == GFX_BOTTOM)
			bottom = t;
	}
	const int kW = 400, kH = 480, kRow = kW * 3;
	std::vector<u8> img(kRow * kH, 0);
	auto blit = [&](const C3D_RenderTarget* t, int xOff, int yOff) {
		if (!t)
			return;
		for (int y = 0; y < t->h && y + yOff < kH; y++)
			for (int x = 0; x < t->w && x + xOff < kW; x++)
			{
				u32 c = t->color[y * t->w + x];
				u8* p = &img[(kH - 1 - (y + yOff)) * kRow + (x + xOff) * 3];
				p[0] = (c >> 16) & 255;
				p[1] = (c >> 8) & 255;
				p[2] = c & 255;
			}
	};
	blit(top, 0, 0);
	blit(bottom, 40, 240);
	u8 header[54] = {};
	auto put32 = [&](int at, u32 v) { for (int i = 0; i < 4; i++) header[at + i] = (v >> (i * 8)) & 255; };
	header[0] = 'B';
	header[1] = 'M';
	put32(2, sizeof(header) + img.size());
	put32(10, sizeof(header));
	put32(14, 40);
	put32(18, kW);
	put32(22, kH);
	header[26] = 1;
	header[28] = 24;
	put32(34, img.size());
	FILE* f = fopen(path, "wb");
	bool ok = f && fwrite(header, sizeof(header), 1, f) == 1 && fwrite(img.data(), img.size(), 1, f) == 1;
	if (f)
		fclose(f);
	return ok;
}

// ---- renderer.cpp, built with its drawing entry points renamed (build.py): they draw only when drawing is on,
// so a plain native run does what it did before the software GPU (no world drawn, no actors re-skinned)

int rendererDrawWorld_sw(World& w, const RenderCamera& cam, float eyeShift, bool secondEye, float fogScale);
void rendererDrawMesh_sw(const ActorMesh& m, const C3D_Mtx* model, C3D_Tex* tex);
void rendererDrawActor_sw(Actor& a, const std::vector<C3D_Tex*>& textures, bool viewModel);
void rendererDrawGlow_sw(const float pos[3], float size, u32 rgba);
bool rendererDrawLocalMap_sw(World& w, float minX, float minY, float size, float topZ, float depth);
C3D_Tex* rendererLocalMap_sw();

int rendererDrawWorld(World& w, const RenderCamera& cam, float eyeShift, bool secondEye, float fogScale)
{
	return drawFrame() ? rendererDrawWorld_sw(w, cam, eyeShift, secondEye, fogScale) : 0;
}
void rendererDrawMesh(const ActorMesh& m, const C3D_Mtx* model, C3D_Tex* tex)
{
	if (drawFrame())
		rendererDrawMesh_sw(m, model, tex);
}
void rendererDrawActor(Actor& a, const std::vector<C3D_Tex*>& textures, bool viewModel)
{
	if (drawFrame())
		rendererDrawActor_sw(a, textures, viewModel);
}
void rendererDrawGlow(const float pos[3], float size, u32 rgba)
{
	if (drawFrame())
		rendererDrawGlow_sw(pos, size, rgba);
}
bool rendererDrawLocalMap(World& w, float minX, float minY, float size, float topZ, float depth)
{
	return drawFrame() && rendererDrawLocalMap_sw(w, minX, minY, size, topZ, depth);
}
C3D_Tex* rendererLocalMap() { return drawFrame() ? rendererLocalMap_sw() : nullptr; }
