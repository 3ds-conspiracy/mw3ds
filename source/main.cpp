// The console build's entry: the game is engine.cpp, shared with the PC test build (platform/platform.hpp)
#include <3ds.h>

#include "platform/platform.hpp"

// A scripted run's end changes nothing on the console: the game goes on (into the run's QUIT, when it has one)
void platformRunOver()
{
}

bool platformFailFast()
{
	return false;
}

// gfxInit shows its screen buffers, in VRAM, before anything is drawn: what was left there came up as static. VRAM is
// filled with black first by the GPU (the CPU may not write it), and given back for gfxInit to take
void platformClearVram()
{
	gspInit();
	void* blocks[16];
	int n = 0;
	while (n < 16)
	{
		void* b = vramAlloc(512 * 1024);
		if (!b)
			break;
		blocks[n++] = b;
		GX_MemoryFill((u32*)b, 0, (u32*)((u8*)b + 512 * 1024), GX_FILL_TRIGGER | GX_FILL_32BIT_DEPTH, nullptr, 0, nullptr, 0);
		gspWaitForPSC0();
	}
	while (n > 0)
		vramFree(blocks[--n]);
	gspExit();
}

int main()
{
	return appMain();
}
