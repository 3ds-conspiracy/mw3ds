#pragma once

#include <cstddef>

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
