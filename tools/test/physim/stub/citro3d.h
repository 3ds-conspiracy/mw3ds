#pragma once
struct C3D_Tex { int unused; };
struct C3D_Mtx { float rows[4][3]; };    // mwrender headers: declared, never run in the harness
struct C3D_RenderTarget;                 // mwgui/widgets.hpp draw calls, never run here
// mwrender/renderhelpers.hpp's inline frame helpers: declared, never run in the harness
struct C3D_TexEnv { int unused; };
#define GPU_VERTEX_SHADER 0
inline void C3D_FVUnifSet(int, int, float, float, float, float) {}
inline C3D_TexEnv* C3D_GetTexEnv(int) { static C3D_TexEnv t; return &t; }
inline void C3D_TexEnvInit(C3D_TexEnv*) {}
