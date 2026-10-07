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

bool linearReclaim()
{
	// Memory waiting for the GPU: on the main thread between frames, wait for it (an empty frame:
	// C3D_FrameBegin waits for the GPU, and refuses inside a frame)
	LinearGuard g;
	if (threadGetCurrent() || (s_frees.empty() && s_texDeletes.empty()) || !C3D_FrameBegin(0))
		return false;
	linearRetire();
	C3D_FrameEnd(0);
	return true;
}

void* lockedLinearAlloc(size_t size)
{
	LinearGuard g;
	void* p = linearAlloc(size);
	if (!p && linearReclaim())
		p = linearAlloc(size);
	return p;
}

// ---- libctru's linear heap, for every caller: ours, citro3d's, citro2d's, ndsp's (-Wl,--wrap in the Makefile)
//
// Its pool counts as not set up once its list of free blocks is empty, and the next allocation sets it up again:
// the whole heap free, every allocation forgotten. What is handed out after that lands on memory still in use: the
// UI's font and frames, the music's buffers, textures (noise on the screen and in the sound until a restart,
// textures in coloured stripes). In a crowded place an emulator run went from 37 KB free to 32 MB free in a second.
// So the last kLinearReserve bytes are never handed out (an allocation that doesn't fit fails, as when memory is
// full: a texture or sound left out, tried again later), and the heap is used under the lock, the libraries' calls
// too (it has no lock of its own, and the streaming thread uses it)
static const size_t kLinearReserve = 64 * 1024;
static bool s_heapStarted = false;

extern "C"
{
void* __real_linearAlloc(size_t size);
void* __real_linearMemAlign(size_t size, size_t alignment);
void __real_linearFree(void* mem);

static bool heapFits(size_t size, size_t alignment)
{
	// (before the first allocation the pool isn't set up and reports nothing free)
	return !s_heapStarted || linearSpaceFree() >= size + alignment + kLinearReserve;
}

void* __wrap_linearAlloc(size_t size)
{
	LinearGuard g;
	if (!heapFits(size, 0x80))
		return nullptr;
	void* p = __real_linearAlloc(size);
	s_heapStarted = true;
	return p;
}

void* __wrap_linearMemAlign(size_t size, size_t alignment)
{
	LinearGuard g;
	if (!heapFits(size, alignment))
		return nullptr;
	void* p = __real_linearMemAlign(size, alignment);
	s_heapStarted = true;
	return p;
}

void __wrap_linearFree(void* mem)
{
	LinearGuard g;
	__real_linearFree(mem);
}
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
