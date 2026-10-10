#include "memstat.h"

#include <cstdlib>

volatile bool g_memoryLow = false;
static void* s_reserve = nullptr;
static size_t s_highWater = 0;
// Enough for a save to be written and a few cell objects to be read while far cells go
static const size_t kReserve = 768 * 1024;

// Halving search with real allocations (once a second at most: about twenty malloc / free pairs)
size_t heapLargestBlock()
{
	size_t lo = 0, hi = 128u << 20;
	while (hi - lo > 16 * 1024)
	{
		size_t mid = lo + (hi - lo) / 2;
		void* p = malloc(mid);
		asm volatile("" : : "r"(p) : "memory");     // (else the compiler drops the malloc / free pair as unused)
		if (p)
		{
			free(p);
			lo = mid;
		}
		else
			hi = mid;
	}
	return lo;
}

void heapNote(size_t used)
{
	if (used > s_highWater)
		s_highWater = used;
}

size_t heapHighWater()
{
	return s_highWater;
}

void memoryReserveTake()
{
	if (!s_reserve)
		s_reserve = malloc(kReserve);
	if (s_reserve)
		g_memoryLow = false;
}

bool memoryReserveSpend()
{
	if (!s_reserve)
		return false;
	free(s_reserve);
	s_reserve = nullptr;
	g_memoryLow = true;
	return true;
}

bool memoryReserveHeld()
{
	return s_reserve != nullptr;
}
