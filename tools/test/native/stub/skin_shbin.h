// Host stand-in for the assembled skinning shader: native_gpu.cpp runs shaders/skin.v.pica as C++ instead
#pragma once
#include "3ds/types.h"
static const u8 skin_shbin[4] = { 0 };
static const u32 skin_shbin_size = sizeof(skin_shbin);
