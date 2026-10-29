#include "platform/cellarena.hpp"

#include <3ds.h>
#include <cstdlib>
#include <cstring>

#include "components/debug/debuglog.hpp"

// A block: [size of the block before, while that one is free][own size | kUsed | kPrevUsed][payload ...]. A free one
// keeps its list links in the payload, and its size again in the next block's first word, so a block coming back merges
// with free neighbours on both sides. A block in use costs 8 bytes over what was asked.
struct Arena::Block
{
	size_t prevSize;
	size_t head;
	Block* next;
	Block* prev;
};

namespace
{
const size_t kAlign = 8, kHead = 2 * sizeof(size_t), kMin = sizeof(Arena::Block), kUsed = 1, kPrevUsed = 2;

inline size_t sizeOf(const Arena::Block* b) { return b->head & ~(size_t)7; }
inline Arena::Block* after(Arena::Block* b) { return (Arena::Block*)((char*)b + sizeOf(b)); }

size_t blockSize(size_t bytes)
{
	size_t n = (bytes + kHead + kAlign - 1) & ~(kAlign - 1);
	return n < kMin ? kMin : n;
}
}

int Arena::binOf(size_t size)
{
	int b = 0;
	size >>= 5;
	while (size > 1 && b < kBins - 1)
	{
		size >>= 1;
		b++;
	}
	return b;
}

void Arena::unlink(Block* b)
{
	if (b->prev)
		b->prev->next = b->next;
	else
		bins_[binOf(sizeOf(b))] = b->next;
	if (b->next)
		b->next->prev = b->prev;
}

void Arena::insertFree(Block* b, size_t size)
{
	b->head = size | kPrevUsed;          // (the block before is in use: free neighbours were merged first)
	Block* n = (Block*)((char*)b + size);
	n->prevSize = size;
	n->head &= ~kPrevUsed;
	int bin = binOf(size);
	b->prev = nullptr;
	b->next = bins_[bin];
	if (b->next)
		b->next->prev = b;
	bins_[bin] = b;
}

void Arena::init(size_t bytes, const char* what)
{
	if (base_ || bytes < 4096)
		return;
	base_ = (char*)malloc(bytes);
	if (!base_)
	{
		logf("arena: no %u KB for %s: its blocks come from the heap", (unsigned)(bytes / 1024), what);
		return;
	}
	size_ = bytes;
	memset(bins_, 0, sizeof(bins_));
	// the whole span one free block, ended by a block that is always in use (never merged into)
	size_t span = (bytes - kHead) & ~(kAlign - 1);
	Block* end = (Block*)(base_ + span);
	end->head = kUsed;
	insertFree((Block*)base_, span);
	logf("arena: %u KB for %s", (unsigned)(bytes / 1024), what);
}

void* Arena::alloc(size_t bytes)
{
	if (!base_)
		return nullptr;
	size_t need = blockSize(bytes);
	LightLock_Lock(&lock_);
	// the smallest block that fits among the first few of its size class, else the first of a bigger class
	Block* best = nullptr;
	for (int bin = binOf(need); bin < kBins && !best; bin++)
	{
		int looked = 0;
		for (Block* f = bins_[bin]; f && looked < 32; f = f->next, looked++)
			if (sizeOf(f) >= need && (!best || sizeOf(f) < sizeOf(best)))
				best = f;
	}
	void* p = nullptr;
	if (best)
	{
		unlink(best);
		size_t size = sizeOf(best);
		if (size - need >= kMin)
		{
			insertFree((Block*)((char*)best + need), size - need);
			size = need;
		}
		best->head = size | kUsed | (best->head & kPrevUsed);
		after(best)->head |= kPrevUsed;
		used_ += size;
		p = (char*)best + kHead;
	}
	LightLock_Unlock(&lock_);
	return p;
}

bool Arena::release(void* p)
{
	char* c = (char*)p;
	if (!base_ || c < base_ || c >= base_ + size_)
		return false;
	Block* b = (Block*)(c - kHead);
	LightLock_Lock(&lock_);
	size_t size = sizeOf(b);
	used_ -= size;
	Block* n = after(b);
	if (!(n->head & kUsed))
	{
		unlink(n);
		size += sizeOf(n);
	}
	if (!(b->head & kPrevUsed))
	{
		Block* before = (Block*)((char*)b - b->prevSize);
		unlink(before);
		size += sizeOf(before);
		b = before;
	}
	insertFree(b, size);
	LightLock_Unlock(&lock_);
	return true;
}

size_t Arena::largestLocked() const
{
	for (int bin = kBins - 1; bin >= 0; bin--)
		if (bins_[bin])
		{
			size_t n = 0;
			for (Block* f = bins_[bin]; f; f = f->next)
				if (sizeOf(f) > n)
					n = sizeOf(f);
			return n > kHead ? n - kHead : 0;
		}
	return 0;
}

bool Arena::fits(size_t bytes)
{
	if (!base_)
		return false;
	LightLock_Lock(&lock_);
	bool fit = largestLocked() >= bytes;
	LightLock_Unlock(&lock_);
	return fit;
}

size_t Arena::largest()
{
	if (!base_)
		return 0;
	LightLock_Lock(&lock_);
	size_t n = largestLocked();
	LightLock_Unlock(&lock_);
	return n;
}

Arena g_cellArena, g_scriptArena;

// The threads with a scope open (the main one and the streaming one; a slot is its own thread's to change, so others
// read it without the lock: they only look for their own)
namespace
{
struct ScopeSlot { Thread thread; int depth, off; };
ScopeSlot s_slots[4];
volatile int s_open = 0;
LightLock s_slotLock = 1;

ScopeSlot* ownSlot()
{
	Thread me = threadGetCurrent();
	for (ScopeSlot& s : s_slots)
		if (s.depth > 0 && s.thread == me)
			return &s;
	return nullptr;
}
}

CellScope::CellScope()
{
	if (ScopeSlot* s = ownSlot())
	{
		s->depth++;
		return;
	}
	LightLock_Lock(&s_slotLock);
	for (ScopeSlot& s : s_slots)
		if (s.depth == 0)
		{
			s.thread = threadGetCurrent();
			s.off = 0;
			s.depth = 1;
			s_open++;
			break;
		}
	LightLock_Unlock(&s_slotLock);
}

CellScope::~CellScope()
{
	ScopeSlot* s = ownSlot();
	if (!s || --s->depth > 0)
		return;
	LightLock_Lock(&s_slotLock);
	s_open--;
	LightLock_Unlock(&s_slotLock);
}

CellScopeOff::CellScopeOff()
{
	if (ScopeSlot* s = s_open ? ownSlot() : nullptr)
		s->off++;
}

CellScopeOff::~CellScopeOff()
{
	if (ScopeSlot* s = s_open ? ownSlot() : nullptr)
		s->off--;
}

bool cellScopeOpen()
{
	if (!s_open)
		return false;
	ScopeSlot* s = ownSlot();
	return s && s->off == 0;
}

void* cellScopeAlloc(size_t bytes)
{
	return cellScopeOpen() ? g_cellArena.alloc(bytes) : nullptr;
}

void* arenaOverflowAlloc(size_t bytes)
{
	if (void* p = g_cellArena.alloc(bytes))
		return p;
	return g_scriptArena.alloc(bytes);
}

bool arenaRelease(void* p)
{
	return g_cellArena.release(p) || g_scriptArena.release(p);
}

void cellArenaInit(size_t bytes) { g_cellArena.init(bytes, "the cells"); }
void* cellArenaAlloc(size_t bytes) { return g_cellArena.alloc(bytes); }
bool cellArenaRelease(void* p) { return g_cellArena.release(p); }
bool cellArenaFits(size_t bytes) { return g_cellArena.fits(bytes); }
size_t cellArenaUsed() { return g_cellArena.used(); }
size_t cellArenaSpare() { return g_cellArena.spare() + g_scriptArena.spare(); }
size_t cellArenaLargest() { return g_cellArena.largest(); }
