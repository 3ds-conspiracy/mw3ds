#pragma once

#include <vector>

#include "cell.h"

// Distant land (tools/convert/distant.py): a coarse, pre-lit height grid of the whole island, drawn for the
// grid cells past the loaded 3 x 3 so the view reaches further. One untextured batch per cell,
// built when the camera comes near and freed when it leaves.
struct DistantCell
{
	int gx, gy;
	CellBatch batch;
	CellBatch statics;            // its big objects, simplified (numIndices 0: none)
};

bool distantLoad(const char* dataDir);        // data/distant.bin; false (nothing drawn) without it
void distantFree();
// Keep the cells within `radius` of grid cell (gx, gy) built, free the rest
void distantUpdate(int gx, int gy, int radius);
const std::vector<DistantCell>& distantCells();
