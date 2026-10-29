#include "platform/memstat.hpp"

#include <cstdlib>
#include <malloc.h>

volatile bool g_memoryLow = false;
volatile bool g_heapProbeFailed = false;
static size_t s_highWater = 0;
// Enough for a save to be written and a few cell objects to be read while far cells go. One block when the heap has
// one, else in pieces: a heap cut into holes of a few hundred KB never gave a whole one back, and with the reserve
// spent for good the next `new` the main thread couldn't place stopped the game
static const size_t kReserve = 768 * 1024, kPiece = 64 * 1024;
static void* s_reserve[kReserve / kPiece];
static int s_pieces = 0;
static bool s_big = false;         // s_reserve[0] is the whole reserve in one block

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

// (the console and the plain PC build: no map. The memory checker's heap has its own)
__attribute__((weak)) bool heapMapLine(char*, size_t)
{
	return false;
}

__attribute__((weak)) bool heapHoles(size_t*)
{
	return false;
}

size_t heapInUse()
{
	size_t used = mallinfo().uordblks, spare = cellArenaSpare();
	return used > spare ? used - spare : 0;
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

static bool reserveFull()
{
	return s_big || s_pieces == (int)(kReserve / kPiece);
}

void memoryReserveTake()
{
	if (!s_pieces && (s_reserve[0] = malloc(kReserve)))
	{
		s_pieces = 1;
		s_big = true;
	}
	if (!s_big)
		while (s_pieces < (int)(kReserve / kPiece) && (s_reserve[s_pieces] = malloc(kPiece)))
			s_pieces++;
	if (reserveFull())
		g_memoryLow = false;
}

bool memoryReserveSpend()
{
	if (!s_pieces)
		return false;
	while (s_pieces)
		free(s_reserve[--s_pieces]);
	s_big = false;
	g_memoryLow = true;
	return true;
}

// (held: all of it; a part taken back still frees what it has when `new` runs out)
bool memoryReserveHeld()
{
	return reserveFull();
}
