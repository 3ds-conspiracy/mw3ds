#pragma once

#include <3ds.h>
#include <citro3d.h>

// libctru's linear heap (GPU-visible memory) has no lock of its own. Outdoor cells load on a
// second thread (World::streamExterior) while the main thread frees cells and plays sounds, so
// every allocation and free of linear memory goes through these, or holds a LinearGuard around
// library calls that allocate it (texture imports, C3D_TexDelete).
void linearLockInit();
void* lockedLinearAlloc(size_t size);

// The GPU draws a frame while the CPU runs the next update: memory freed then may still be read
// by that frame's commands, and the streaming thread reading a cell into it straight away hung the
// GPU on hardware (multicoloured static, outdoors). So frees and texture deletes wait for
// linearRetire, called after every C3D_FrameBegin (which waits for the GPU to finish).
void lockedLinearFree(void* p);
void deferredTexDelete(C3D_Tex* tex);
// Main thread, between frames: what waits for the GPU is freed now (an empty frame). True if anything was.
// Before reading a cell in the update that freed others (their memory would otherwise come back only after it)
bool linearReclaim();
void linearRetire();

struct LinearGuard
{
	LinearGuard();
	~LinearGuard();
};
