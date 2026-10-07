// Host stand-in for citro2d: drawn by the software GPU in native_gpu.cpp (NATIVE_DRAW=1), else nothing
#pragma once
#include "citro3d.h"
struct C2D_TextBuf_s { int unused; };
typedef C2D_TextBuf_s* C2D_TextBuf;
struct C2D_Text { const char* s; };
struct C2D_Tint { u32 color; float blend; };
struct C2D_ImageTint { C2D_Tint corners[4]; };      // top left, top right, bottom left, bottom right
#define C2D_Color32(r, g, b, a) ((u32)(((a) << 24) | ((b) << 16) | ((g) << 8) | (r)))
#define C2D_AlphaBlend 0
#define C2D_WithColor 0
inline bool C2D_Init(size_t) { return true; }
inline void C2D_Fini() {}
void C2D_Prepare();
inline void C2D_Flush() {}
void C2D_SceneBegin(C3D_RenderTarget* t);
void C2D_TargetClear(C3D_RenderTarget* t, u32 color);
C3D_RenderTarget* C2D_CreateScreenTarget(int screen, int side);
void C2D_ViewReset();
void C2D_ViewTranslate(float x, float y);
bool C2D_DrawRectSolid(float x, float y, float z, float w, float h, u32 clr);
bool C2D_DrawTriangle(float x0, float y0, u32 clr0, float x1, float y1, u32 clr1, float x2, float y2, u32 clr2, float depth);
bool C2D_DrawImageAt(C2D_Image img, float x, float y, float depth, const C2D_ImageTint* tint, float scaleX, float scaleY);
bool C2D_DrawImageAtRotated(C2D_Image img, float x, float y, float depth, float angle, const C2D_ImageTint* tint,
	float scaleX, float scaleY);
inline void C2D_PlainImageTint(C2D_ImageTint* t, u32 color, float blend)
{
	for (auto& c : t->corners)
		c = { color, blend };
}
inline C2D_TextBuf C2D_TextBufNew(size_t) { static C2D_TextBuf_s b; return &b; }
inline void C2D_TextBufDelete(C2D_TextBuf) {}
inline void C2D_TextBufClear(C2D_TextBuf) {}
inline const char* C2D_TextParse(C2D_Text* t, C2D_TextBuf, const char* s) { t->s = s; return s; }
inline void C2D_TextOptimize(const C2D_Text*) {}
// The system font (only before the theme's Morrowind font is loaded): a plain 8 x 8 stand-in
void C2D_DrawText(const C2D_Text* t, u32 flags, float x, float y, float z, float sx, float sy, ...);
