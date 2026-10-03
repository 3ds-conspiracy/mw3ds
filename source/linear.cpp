#include "linear.h"

#include <vector>

static RecursiveLock s_lock;
static bool s_ready = false;
static std::vector<void*> s_frees;          // waiting for the GPU (linearRetire)
static std::vector<C3D_Tex> s_texDeletes;

void linearLockInit()
{
	RecursiveLock_Init(&s_lock);
	s_ready = true;
}

LinearGuard::LinearGuard()
{
	if (s_ready)
		RecursiveLock_Lock(&s_lock);
}

LinearGuard::~LinearGuard()
{
	if (s_ready)
		RecursiveLock_Unlock(&s_lock);
}

void* lockedLinearAlloc(size_t size)
{
	LinearGuard g;
	void* p = linearAlloc(size);
	// Full, but memory is waiting for the GPU: on the main thread between frames, wait for it (an
	// empty frame: C3D_FrameBegin waits for the GPU, and refuses inside a frame)
	if (!p && !threadGetCurrent() && (!s_frees.empty() || !s_texDeletes.empty()) && C3D_FrameBegin(0))
	{
		linearRetire();
		C3D_FrameEnd(0);
		p = linearAlloc(size);
	}
	return p;
}

void lockedLinearFree(void* p)
{
	if (!p)
		return;
	LinearGuard g;
	s_frees.push_back(p);
}

void deferredTexDelete(C3D_Tex* tex)
{
	LinearGuard g;
	s_texDeletes.push_back(*tex);
	tex->data = nullptr;
}

void linearRetire()
{
	LinearGuard g;
	for (void* p : s_frees)
		linearFree(p);
	s_frees.clear();
	for (auto& t : s_texDeletes)
		C3D_TexDelete(&t);
	s_texDeletes.clear();
}
