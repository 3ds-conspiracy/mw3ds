// The memory checker's heap (build.py --mem: build\native-mem): malloc, free, realloc and calloc replaced by an arena
// as big as the console's C++ heap (source/platform/heaps.cpp: 39 MB on an Old 3DS, NATIVE_OLD3DS=1; else the New 3DS's
// 92 MB), so a run that needs more than the console has runs out where the console would, and fragmentation builds up
// as a long session's does. The C++ library's operator new comes here too (it calls malloc).
//
// Binned best fit with boundary tags and coalescing, after the dlmalloc the 3DS's newlib uses: not the same holes to
// the byte, the same kind of heap. mallinfo() reports as newlib's does: uordblks the bytes in use, fordblks the free
// bytes in holes (the untouched top of the arena left out, as newlib's arena only grows as far as it was used).
// Out of heap: the reserve first (platform/memstat.cpp), as the console's operator new does, then the console's crash
// line and the run ends (the console freezes there; a test should end).
#include <windows.h>

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "components/debug/debuglog.hpp"
#include "platform/memstat.hpp"

namespace
{
// A chunk, as dlmalloc lays one out on a 32-bit console: [prev size][size | flags][payload ...]. The prev size word is
// only meaningful while the chunk before is free (its last word otherwise belongs to that chunk's payload), so a chunk
// in use costs one word over what was asked; a free one keeps its links in the payload and its size again in the next
// chunk's prev size word. Sizes are multiples of 8, at least 16 (dlmalloc's minimum on 32 bits)
struct Chunk
{
	size_t prevSize;
	size_t head;                 // size | kPrevUsed | kUsed
	Chunk* next;                 // (free: the bin's list)
	Chunk* prev;
};
const size_t kAlign = 8, kWord = sizeof(size_t), kMin = 16, kPrevUsed = 1, kUsed = 2, kBins = 40;

char* s_base = nullptr;
char* s_end = nullptr;            // the sentinel chunk (always in use) sits here
char* s_top = nullptr;            // the highest address ever handed out (newlib's arena top)
size_t s_used = 0, s_free = 0;
Chunk* s_bins[kBins];
CRITICAL_SECTION s_lock;
bool s_ready = false;

// the runtime's own (for what it allocated before us, or inside the C library)
typedef void* (*MallocFn)(size_t);
typedef void (*FreeFn)(void*);
typedef void* (*ReallocFn)(void*, size_t);
MallocFn s_crtMalloc;
FreeFn s_crtFree;
ReallocFn s_crtRealloc;

inline size_t sizeOf(const Chunk* c) { return c->head & ~(size_t)7; }
inline Chunk* after(Chunk* c) { return (Chunk*)((char*)c + sizeOf(c)); }

int binOf(size_t size)
{
	int b = 0;
	size >>= 4;
	while (size > 1 && b < (int)kBins - 1)
	{
		size >>= 1;
		b++;
	}
	return b;
}

void unlink(Chunk* f)
{
	if (f->prev)
		f->prev->next = f->next;
	else
		s_bins[binOf(sizeOf(f))] = f->next;
	if (f->next)
		f->next->prev = f->prev;
}

// a free chunk of `size` at c (the chunk before it in use: free neighbours are merged before this)
void insert(Chunk* c, size_t size)
{
	c->head = size | kPrevUsed;
	Chunk* n = (Chunk*)((char*)c + size);
	n->prevSize = size;
	n->head &= ~kPrevUsed;
	int bin = binOf(size);
	c->prev = nullptr;
	c->next = s_bins[bin];
	if (c->next)
		c->next->prev = c;
	s_bins[bin] = c;
}

void init()
{
	if (s_ready)
		return;
	InitializeCriticalSection(&s_lock);
	HMODULE crt = GetModuleHandleA("msvcrt.dll");
	if (!crt)
		crt = LoadLibraryA("msvcrt.dll");
	s_crtMalloc = (MallocFn)GetProcAddress(crt, "malloc");
	s_crtFree = (FreeFn)GetProcAddress(crt, "free");
	s_crtRealloc = (ReallocFn)GetProcAddress(crt, "realloc");
	const char* o = getenv("NATIVE_OLD3DS");
	size_t heap = o && o[0] == '1' ? (size_t)39 << 20 : (size_t)92380 * 1024;
	s_base = (char*)VirtualAlloc(nullptr, heap, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	if (!s_base)
	{
		fprintf(stderr, "native_heap: no %u KB for the arena\n", (unsigned)(heap / 1024));
		ExitProcess(3);
	}
	size_t span = (heap - 4 * kWord) & ~(kAlign - 1);
	s_end = s_base + span;
	Chunk* sentinel = (Chunk*)s_end;
	sentinel->head = 0 | kUsed;          // never merged into, never handed out
	s_top = s_base;
	memset(s_bins, 0, sizeof(s_bins));
	insert((Chunk*)s_base, span);
	s_free = span;
	s_ready = true;
}

inline bool inArena(void* p) { return s_ready && (char*)p >= s_base && (char*)p < s_end; }

void* take(size_t n)
{
	size_t need = (n + kWord + kAlign - 1) & ~(kAlign - 1);
	if (need < kMin)
		need = kMin;
	Chunk* best = nullptr;
	// the smallest chunk that fits: in its own bin the best of the first few, else the first of a bigger bin's
	for (int bin = binOf(need); bin < (int)kBins && !best; bin++)
	{
		int looked = 0;
		for (Chunk* f = s_bins[bin]; f && looked < 64; f = f->next, looked++)
			if (sizeOf(f) >= need && (!best || sizeOf(f) < sizeOf(best)))
				best = f;
	}
	if (!best)
		return nullptr;
	unlink(best);
	size_t size = sizeOf(best);
	s_free -= size;
	if (size - need >= kMin)
	{
		Chunk* rest = (Chunk*)((char*)best + need);
		insert(rest, size - need);
		s_free += size - need;
		size = need;
	}
	best->head = size | kUsed | (best->head & kPrevUsed);
	after(best)->head |= kPrevUsed;
	s_used += size;
	if ((char*)best + size > s_top)
		s_top = (char*)best + size;
	return (char*)best + 2 * kWord;
}

void give(void* p)
{
	Chunk* c = (Chunk*)((char*)p - 2 * kWord);
	size_t size = sizeOf(c);
	s_used -= size;
	s_free += size;
	Chunk* n = after(c);
	if (!(n->head & kUsed))
	{
		unlink(n);
		size += sizeOf(n);
	}
	if (!(c->head & kPrevUsed))
	{
		Chunk* b = (Chunk*)((char*)c - c->prevSize);
		unlink(b);
		size += sizeOf(b);
		c = b;
	}
	insert(c, size);
}

// Out of heap in operator new (the C++ library calls the new handler): the console's operator new
// (components/debug/debuglog.cpp): the reserve first, then the crash line; the run ends instead of freezing
void outOfHeap()
{
	if (memoryReserveSpend())
	{
		logf("memory: out of heap, the reserve spent (main at %s)", g_mainAt);
		return;
	}
	logf("crash: out of memory (new), main at %s, %u KB used of the heap", g_mainAt, (unsigned)(s_used / 1024));
	fflush(nullptr);
	ExitProcess(3);
}

struct InstallHandler
{
	InstallHandler() { std::set_new_handler(outOfHeap); }
} s_installHandler;
}

// (inside a nativeOutsideEnter / Leave pair, this thread allocates outside the arena: the native renderer's own buffers,
// which the console keeps in VRAM and linear memory, not the C++ heap)
static thread_local int tl_outside = 0;

extern "C" {
void nativeOutsideEnter() { tl_outside++; }
void nativeOutsideLeave() { tl_outside--; }

void* malloc(size_t n)
{
	init();
	if (tl_outside > 0)
		return s_crtMalloc(n ? n : 1);
	EnterCriticalSection(&s_lock);
	void* p = take(n ? n : 1);
	LeaveCriticalSection(&s_lock);
	return p;
}

void free(void* p)
{
	if (!p)
		return;
	if (!inArena(p))
	{
		if (s_crtFree)
			s_crtFree(p);
		return;
	}
	EnterCriticalSection(&s_lock);
	give(p);
	LeaveCriticalSection(&s_lock);
}

void* calloc(size_t a, size_t b)
{
	size_t n = a * b;
	void* p = malloc(n);
	if (p)
		memset(p, 0, n);
	return p;
}

void* realloc(void* p, size_t n)
{
	if (!p)
		return malloc(n);
	if (!inArena(p))
		return s_crtRealloc ? s_crtRealloc(p, n) : nullptr;
	if (!n)
	{
		free(p);
		return nullptr;
	}
	size_t have = sizeOf((Chunk*)((char*)p - 2 * kWord)) - kWord;
	if (have >= n)
		return p;
	void* q = malloc(n);
	if (q)
	{
		memcpy(q, p, have);
		free(p);
	}
	return q;
}

// Memory the console doesn't take from the C++ heap (the linear heap the GPU sees: native_stubs.cpp linearAlloc): from
// the C runtime, outside the arena (free sends anything outside the arena back there)
void* nativeOutsideAlloc(size_t n)
{
	init();
	return s_crtMalloc(n ? n : 1);
}

// newlib's mallinfo, as the game asks it (native_compat.h maps mallinfo() here in this build)
NativeMallinfo nativeMallinfo()
{
	init();
	EnterCriticalSection(&s_lock);
	size_t untouched = (size_t)(s_end - s_top);
	NativeMallinfo m = { (int)s_used, (int)(s_free > untouched ? s_free - untouched : 0) };
	LeaveCriticalSection(&s_lock);
	return m;
}
}

// A compressed file opened for reading (components/files/zfile.cpp, built as zopenWhole here): the console inflates it
// as it is read, holding zlib's state and ~60 KB; this build inflates the whole file into one block, which the console
// never asks for, so that block (and the stream over it, freed at fclose) comes from outside the arena
FILE* zopenWhole(const char* path);
FILE* zopen(const char* path)
{
	nativeOutsideEnter();
	FILE* f = zopenWhole(path);
	nativeOutsideLeave();
	return f;
}
