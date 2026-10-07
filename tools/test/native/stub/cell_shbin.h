// Host stand-in for the assembled vertex shader: native_gpu.cpp runs cell.v.pica as C++ instead
#pragma once
#include "3ds/types.h"
static const u8 cell_shbin[4] = { 0 };
static const u32 cell_shbin_size = sizeof(cell_shbin);
