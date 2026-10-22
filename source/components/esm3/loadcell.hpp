#pragma once

// Cells (OpenMW's ESM::Cell)
#include "components/misc/atom.hpp"

struct CellDef
{
	Atom name, file;
	bool interior;
	int gx = 0, gy = 0;
	float sea = 0.0f;
	bool noSleep = false;         // CELL flag: resting here is illegal (only waiting, or in a bed)
	bool sky = false;             // CELL flag 0x80: an interior that behaves as an exterior (Tribunal's Mournhold)
	Atom region;                  // exterior: its region (lowercase id)
};
