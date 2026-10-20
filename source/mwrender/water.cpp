// The water surface's animated frames and their slide (OpenMW's Water)

#include <algorithm>
#include <citro3d.h>
#include <cmath>
#include <cstring>

#include "mwrender/renderhelpers.hpp"

float rendererWaterScroll()
{
	return kWaterScroll;
}

// ---- Enchanted items: Morrowind's shimmer, the 32 frames of textures\magicitem\caust played over the item and
// added in the colour of its enchantment's first effect (OpenMW's addEnchantedGlow; it maps them by the view,
// here by the item's own texture coordinates)

// The water surface's frames (art/water/waterNN.t3x, the converter's from Morrowind.ini [Water]) and their rate
int rendererWaterFrameCount()
{
	return g_gpu.waterCount;
}

void rendererLoadWaterFrames(const char* dataDir, float fps)
{
	if (fps > 0.0f)
		g_gpu.waterFps = fps;
	if (g_gpu.waterCount)
		return;
	for (int i = 0; i < 64; i++)
	{
		char path[256];
		snprintf(path, sizeof(path), "%s/art/water/water%02d.t3x", dataDir, i);
		if (!texImportFile(&g_gpu.water[i], path))
			break;
		C3D_TexSetFilter(&g_gpu.water[i], GPU_LINEAR, GPU_LINEAR);
		C3D_TexSetWrap(&g_gpu.water[i], GPU_REPEAT, GPU_REPEAT);
		g_gpu.waterCount = i + 1;
	}
	logf("renderer: %d water frames at %.0f a second", g_gpu.waterCount, g_gpu.waterFps);
}
