#pragma once

#include <3ds.h>
#include <cstddef>
#include <new>
#include <vector>

// A cell's big blocks (its collision: vertices, triangles, the grid's lists) in an arena of their own, set aside at start-up.
// In the one heap they came and went between the small blocks that live longer (a reference's lists, an actor's state,
// strings), and a cell let go of gave its memory back as holes: an Old 3DS's heap ended up with two thousand of them,
// megabytes free in pieces of 16 to 256 KB, and the next cell found no block for its collision. In the arena only cells'
// blocks come and go, so what one leaves the next one fits. When it is full a block comes from the heap as before (a
// cell never fails for the arena); where a block came from, its address says. Any thread.
// The allocator of an arena: free blocks in lists by size class (powers of two), the smallest that fits taken, merged
// with free neighbours as they come back (boundary tags), as the system's heap does; small blocks by the thousand cost
// no more than big ones
class Arena
{
public:
	struct Block;
	void init(size_t bytes, const char* what);
	void* alloc(size_t bytes);               // nullptr: no room
	bool release(void* p);                   // false: not this arena's
	bool owns(const void* p) const { return base_ && (const char*)p >= base_ && (const char*)p < base_ + size_; }
	bool fits(size_t bytes);
	size_t used() const { return used_; }
	size_t spare() const { return size_ - used_; }
	size_t size() const { return size_; }
	size_t largest();
private:
	static const int kBins = 24;
	static int binOf(size_t size);
	void unlink(Block* b);
	void insertFree(Block* b, size_t size);
	size_t largestLocked() const;
	Block* bins_[kBins] = {};
	char* base_ = nullptr;
	size_t size_ = 0, used_ = 0;
	LightLock lock_ = 1;                     // unlocked (LightLock_Init sets 1)
};
// The cells' collision; and the script trees read back from the card (mwscript/scriptstore.cpp), whose coming and
// going every few seconds cut the heap the same way
extern Arena g_cellArena, g_scriptArena;

// While a CellScope is open on a thread, every `new` there takes its block from the cells' arena too (from the heap when
// the arena is full): a cell's read builds about 600 KB of small blocks (its batch and placement lists, texture names,
// its people's meshes and poses) that all go when the cell goes. On the heap they fell between the blocks that stay
// (objects, scripts, the texture cache), and a long game on an Old 3DS had 11 MB free in 3000 holes, none of them
// 1.5 MB. What a read builds that outlives the cell (the texture cache's entries, the shared skeletons) is made inside a
// CellScopeOff. `delete` gives a block back to whichever arena holds it (components/debug/debuglog.cpp)
class CellScope
{
public:
	CellScope();
	~CellScope();
};
class CellScopeOff
{
public:
	CellScopeOff();
	~CellScopeOff();
};
bool cellScopeOpen();                        // a CellScope is open on this thread (and not set off)
void* cellScopeAlloc(size_t bytes);          // the arena's block for a `new` in a CellScope; nullptr: none open, or no room
bool arenaRelease(void* p);                  // given back to the arena that holds it; false: the heap's
// The heap full: a `new` takes its block from the arenas' spare room (the cells', then the script trees'), before the
// out-of-memory reserve. Set aside at start-up, that room is heap all the same: on an Old 3DS the 10 MB arena left the
// rest of the game 6 MB less, and the emulator's run over Solstheim spent the reserve on a script's 36 KB
void* arenaOverflowAlloc(size_t bytes);

void cellArenaInit(size_t bytes);
void* cellArenaAlloc(size_t bytes);          // nullptr: no room in the arena
bool cellArenaRelease(void* p);              // false: not the arena's
bool cellArenaFits(size_t bytes);            // a block of that many bytes would come from the arena now
size_t cellArenaUsed();
size_t cellArenaSpare();                     // what is set aside for the arenas and not in use
size_t cellArenaLargest();                   // the biggest free block in it

template <class T> struct CellAllocator
{
	typedef T value_type;
	CellAllocator() = default;
	template <class U> CellAllocator(const CellAllocator<U>&) {}
	T* allocate(size_t n)
	{
		if (void* p = cellArenaAlloc(n * sizeof(T)))
			return (T*)p;
		return (T*)::operator new(n * sizeof(T));
	}
	void deallocate(T* p, size_t)
	{
		if (!cellArenaRelease(p))
			::operator delete(p);
	}
	template <class U> bool operator==(const CellAllocator<U>&) const { return true; }
	template <class U> bool operator!=(const CellAllocator<U>&) const { return false; }
};
template <class T> using CellVec = std::vector<T, CellAllocator<T>>;
