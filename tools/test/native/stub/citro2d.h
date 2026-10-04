// Host stand-in for citro2d: drawing calls that do nothing
#pragma once
#include "citro3d.h"
struct C2D_TextBuf_s { int unused; };
typedef C2D_TextBuf_s* C2D_TextBuf;
struct C2D_Text { int unused; };
struct C2D_ImageTint { u32 corners[4]; };
#define C2D_Color32(r, g, b, a) ((u32)(((a) << 24) | ((b) << 16) | ((g) << 8) | (r)))
#define C2D_AlphaBlend 0
#define C2D_WithColor 0
inline bool C2D_Init(size_t) { return true; }
inline void C2D_Fini() {}
inline void C2D_Prepare() {}
inline void C2D_Flush() {}
inline void C2D_SceneBegin(C3D_RenderTarget*) {}
inline void C2D_TargetClear(C3D_RenderTarget*, u32) {}
inline C3D_RenderTarget* C2D_CreateScreenTarget(int, int) { static C3D_RenderTarget t; return &t; }
inline void C2D_ViewReset() {}
inline void C2D_ViewTranslate(float, float) {}
inline bool C2D_DrawRectSolid(float, float, float, float, float, u32) { return true; }
inline bool C2D_DrawTriangle(float, float, u32, float, float, u32, float, float, u32, float) { return true; }
inline bool C2D_DrawImageAt(C2D_Image, float, float, float, const C2D_ImageTint*, float, float) { return true; }
inline bool C2D_DrawImageAtRotated(C2D_Image, float, float, float, float, const C2D_ImageTint*, float, float) { return true; }
inline void C2D_PlainImageTint(C2D_ImageTint*, u32, float) {}
inline C2D_TextBuf C2D_TextBufNew(size_t) { static C2D_TextBuf_s b; return &b; }
inline void C2D_TextBufDelete(C2D_TextBuf) {}
inline void C2D_TextBufClear(C2D_TextBuf) {}
inline const char* C2D_TextParse(C2D_Text*, C2D_TextBuf, const char* s) { return s; }
inline void C2D_TextOptimize(const C2D_Text*) {}
inline void C2D_DrawText(const C2D_Text*, u32, float, float, float, float, float, ...) {}
