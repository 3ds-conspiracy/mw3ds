#pragma once

#include <cstddef>
#include <cstdlib>

// The C++ heap, watched and kept from crashing (the Old 3DS has ~40 MB of it):
// - the biggest single block that can still be had (a heap with room in total but only in small holes is
//   fragmented: a cell file that needs 3 MB at once fails there), and the most ever used
// - a reserve set aside at start: running out of memory in `new` spends it and raises g_memoryLow instead of
//   stopping the game; the main loop then gives memory back (far cells, map tiles, view distance) and takes the
//   reserve again once it can
size_t heapLargestBlock();
size_t heapHighWater();             // most bytes ever in use (sampled by heapNote)
void heapNote(size_t used);         // the bytes in use now (each second's profile line)
void memoryReserveTake();           // sets the reserve aside (again), if the heap can spare it now
bool memoryReserveSpend();          // out of memory: frees the reserve; false if it was already spent
bool memoryReserveHeld();
extern volatile bool g_memoryLow;   // the reserve was spent: give memory back

// A big vector resize that can FAIL instead of waiting forever. On the streaming thread, `new` that
// runs out sleeps and retries (components/debug/debuglog.cpp outOfMemory): a cell load wanting one block the fragmented
// heap cannot give never returned — the long-route freeze. Probing with malloc keeps the failure
// visible; the caller returns false and the cell waits for room. (Another thread can still take the
// probed room before the resize lands — rare; that resize then retries under outOfMemory as before.)
// A failed probe also sets g_heapProbeFailed, so a reader that treats "false" as truncated data (the cell's collision) can tell
// a memory failure, which fails the load, from a short file. The loader clears it before each cell
extern volatile bool g_heapProbeFailed;
template <class V> bool tryResize(V& v, size_t n)
{
	if (n <= v.capacity())
	{
		v.resize(n);
		return true;
	}
	void* probe = malloc(n * sizeof(typename V::value_type) + 64);
	if (!probe)
	{
		g_heapProbeFailed = true;
		return false;
	}
	free(probe);
	v.resize(n);
	return true;
}

// The same for a reserve (room for n, size unchanged)
template <class V> bool tryReserve(V& v, size_t n)
{
	if (n <= v.capacity())
		return true;
	void* probe = malloc(n * sizeof(typename V::value_type) + 64);
	if (!probe)
	{
		g_heapProbeFailed = true;
		return false;
	}
	free(probe);
	v.reserve(n);
	return true;
}
