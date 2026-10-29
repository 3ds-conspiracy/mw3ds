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

void watchStart();

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
	watchStart();
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

// The console's operator new and delete (components/debug/debuglog.cpp): a cell's read takes its blocks from the cells'
// arena (platform/cellarena.hpp CellScope), and a block goes back to the arena it came from
void* operator new(size_t n)
{
	if (n == 0)
		n = 1;
	if (void* p = cellScopeAlloc(n))
		return p;
	for (;;)
	{
		if (void* p = malloc(n))
			return p;
		if (void* p = arenaOverflowAlloc(n))
			return p;
		std::new_handler h = std::get_new_handler();
		if (!h)
			abort();
		h();
	}
}

void* operator new[](size_t n)
{
	return operator new(n);
}

void operator delete(void* p) noexcept
{
	if (p && !arenaRelease(p))
		free(p);
}

void operator delete[](void* p) noexcept
{
	operator delete(p);
}

void operator delete(void* p, size_t) noexcept
{
	operator delete(p);
}

void operator delete[](void* p, size_t) noexcept
{
	operator delete(p);
}

static size_t s_holesKb[4];

bool heapHoles(size_t kb[4])
{
	char line[256];
	if (!heapMapLine(line, sizeof(line)))
		return false;
	memcpy(kb, s_holesKb, sizeof(s_holesKb));
	return true;
}

// The heap's map in a line (platform/memstat.hpp): the free space by the size of its holes, and the blocks in use
// pinned between two free ones of 16 KB or more (what keeps the free space from merging back into one piece)
bool heapMapLine(char* out, size_t n)
{
	if (!s_ready)
		return false;
	EnterCriticalSection(&s_lock);
	size_t freeBy[4] = {}, pinBy[4] = {}, pinBytes = 0;
	int holes = 0;
	Chunk* prevFree = nullptr;
	Chunk* prevUsed = nullptr;
	bool usedAfterBigFree = false;
	for (Chunk* c = (Chunk*)s_base; (char*)c < s_top && (char*)c < s_end; c = after(c))
	{
		size_t sz = sizeOf(c);
		if (!sz)
			break;
		if (c->head & kUsed)
		{
			// a run of blocks in use right after a big hole: pinned if another big hole follows it
			if (prevFree && sizeOf(prevFree) >= 16 * 1024)
				usedAfterBigFree = true, prevUsed = c, pinBytes = 0;
			if (usedAfterBigFree)
				pinBytes += sz;
			prevFree = nullptr;
		}
		else
		{
			holes++;
			freeBy[sz >= (1536u << 10) ? 3 : sz >= (256u << 10) ? 2 : sz >= (16u << 10) ? 1 : 0] += sz;
			if (usedAfterBigFree && sz >= 16 * 1024 && prevUsed)
				pinBy[pinBytes >= (256u << 10) ? 3 : pinBytes >= (16u << 10) ? 2 : pinBytes >= 1024 ? 1 : 0]++;
			usedAfterBigFree = false;
			prevFree = c;
		}
	}
	LeaveCriticalSection(&s_lock);
	s_holesKb[0] = freeBy[0] >> 10;
	s_holesKb[1] = freeBy[1] >> 10;
	s_holesKb[2] = freeBy[2] >> 10;
	s_holesKb[3] = freeBy[3] >> 10;
	snprintf(out, n, "%d holes; free in holes <16K %u KB, 16-256K %u KB, 256K-1.5M %u KB, >1.5M %u KB; pins between big holes "
		"<1K %u, 1-16K %u, 16-256K %u, >256K %u", holes, (unsigned)(freeBy[0] >> 10), (unsigned)(freeBy[1] >> 10),
		(unsigned)(freeBy[2] >> 10), (unsigned)(freeBy[3] >> 10), (unsigned)pinBy[0], (unsigned)pinBy[1], (unsigned)pinBy[2],
		(unsigned)pinBy[3]);
	return true;
}

// (inside a nativeOutsideEnter / Leave pair, this thread allocates outside the arena: the native renderer's own buffers,
// which the console keeps in VRAM and linear memory, not the C++ heap)
static thread_local int tl_outside = 0;

// NATIVE_GUARD=1: each block from outside the arena (linear memory, the inflated files, the renderer's buffers) ends
// against a page that faults when touched, so an overrun stops at the write that makes it (the crash handler names it)
// instead of corrupting the C runtime's heap, which only fails later at some free (0xc0000374)
namespace
{
const uint32_t kGuardMagic = 0x44524147;
struct GuardHead { uint32_t magic; uint32_t size; };
int s_guard = -1;

bool guardOn()
{
	if (s_guard < 0)
	{
		const char* g = getenv("NATIVE_GUARD");
		s_guard = g && g[0] == '1';
	}
	return s_guard == 1;
}

void* guardAlloc(size_t n)
{
	const size_t page = 4096, body = (n + kAlign - 1) & ~(kAlign - 1);
	size_t pages = (body + sizeof(GuardHead) + page - 1) / page;
	char* base = (char*)VirtualAlloc(nullptr, (pages + 1) * page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	if (!base)
		return nullptr;
	DWORD old;
	VirtualProtect(base + pages * page, page, PAGE_NOACCESS, &old);
	GuardHead* h = (GuardHead*)base;
	h->magic = kGuardMagic;
	h->size = (uint32_t)n;
	return base + pages * page - body;
}

// a guarded block's head, or null for a block of the C runtime's
GuardHead* guardHead(void* p)
{
	if (s_guard != 1)
		return nullptr;
	MEMORY_BASIC_INFORMATION mi;
	if (!VirtualQuery(p, &mi, sizeof(mi)) || mi.State != MEM_COMMIT || mi.Type != MEM_PRIVATE)
		return nullptr;
	GuardHead* h = (GuardHead*)mi.AllocationBase;
	return h->magic == kGuardMagic ? h : nullptr;
}

// NATIVE_WATCH=<hex address>: a hardware watch (the CPU's debug registers, on the thread that first allocates: the main
// one) on the 4 bytes there; each write logs the value written and the code that wrote it, to find a write through a
// stale pointer (the heap's layout is the same from run to run, so an address seen once is the same the next time)
uintptr_t s_watch = 0;

LONG CALLBACK watchHandler(EXCEPTION_POINTERS* e)
{
	CONTEXT* c = e->ContextRecord;
	char* image = (char*)GetModuleHandleA(nullptr);
	if (e->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT && s_watch && !(c->Dr7 & 1))
	{
		c->Dr0 = s_watch;
		c->Dr7 = 1 | (1u << 16) | (3u << 18);      // local enable, on writes, 4 bytes
		c->Eip += 1;                              // past the int3
		return EXCEPTION_CONTINUE_EXECUTION;
	}
	if (e->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP && (c->Dr6 & 1))
	{
		void* frames[6];
		USHORT n = CaptureStackBackTrace(1, 6, frames, nullptr);
		char line[200];
		int k = snprintf(line, sizeof(line), "watch: %08x = %08x at +0x%x", (unsigned)s_watch, *(unsigned*)s_watch,
			(unsigned)((char*)c->Eip - image));
		for (USHORT i = 0; i < n && k < (int)sizeof(line) - 12; i++)
			k += snprintf(line + k, sizeof(line) - k, " +0x%x", (unsigned)((char*)frames[i] - image));
		logf("%s", line);
		c->Dr6 = 0;
		return EXCEPTION_CONTINUE_EXECUTION;
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

void watchStart()
{
	const char* wv = getenv("NATIVE_WATCH");
	if (!wv || !wv[0])
		return;
	s_watch = (uintptr_t)strtoul(wv, nullptr, 16);
	AddVectoredExceptionHandler(1, watchHandler);
	__asm__ volatile("int3");
}

void* outsideAlloc(size_t n) { return guardOn() ? guardAlloc(n) : s_crtMalloc(n); }

void outsideFree(void* p)
{
	if (GuardHead* h = guardHead(p))
	{
		VirtualFree(h, 0, MEM_RELEASE);
		return;
	}
	// (guarded: a pointer the C runtime's heap doesn't hold, freed twice or never its, faults here, at the free that
	// does it, rather than corrupting that heap for a later free to find)
	if (s_guard == 1)
	{
		typedef HANDLE (*HeapFn)();
		static HANDLE crtHeap = nullptr;
		if (!crtHeap)
			if (HeapFn f = (HeapFn)GetProcAddress(GetModuleHandleA("msvcrt.dll"), "_get_heap_handle"))
				crtHeap = f();
		if (crtHeap && !HeapValidate(crtHeap, 0, p))
		{
			logf("crash: free of %p, not a block of the C runtime's heap", p);
			void* frames[24];
			USHORT n = CaptureStackBackTrace(0, 24, frames, nullptr);
			char* image = (char*)GetModuleHandleA(nullptr);
			for (USHORT i = 0; i < n; i++)
				logf("crash:   +0x%x", (unsigned)((char*)frames[i] - image));
			fflush(nullptr);
			*(volatile int*)0 = 0;
		}
	}
	if (s_crtFree)
		s_crtFree(p);
}
}

extern "C" {
void nativeOutsideEnter() { tl_outside++; }
void nativeOutsideLeave() { tl_outside--; }

void* malloc(size_t n)
{
	init();
	if (tl_outside > 0)
		return outsideAlloc(n ? n : 1);
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
		outsideFree(p);
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
	{
		if (GuardHead* h = guardHead(p))
		{
			void* q = guardAlloc(n);
			if (q)
			{
				memcpy(q, p, h->size < n ? h->size : n);
				VirtualFree(h, 0, MEM_RELEASE);
			}
			return q;
		}
		return s_crtRealloc ? s_crtRealloc(p, n) : nullptr;
	}
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
	return outsideAlloc(n ? n : 1);
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
