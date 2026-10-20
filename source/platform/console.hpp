#pragma once

#include <cstddef>

// What the console can do, decided once at start (consoleProfileInit, before anything reads it) from two things:
// - its CPU (APT_CheckNew3DS): a New 3DS's 804 MHz cores or an Old 3DS's 268 MHz ones decide the frame rate, the view,
//   how often distant people animate, the screen's colour format and what starts on (3D, the control layout)
// - the heap it got (the memory mode): what is kept in memory. A New 3DS can run in an Old 3DS's memory mode (a .3dsx
//   from a launcher that has one): it gets the fast CPU's settings and the small heap's budgets
// The two rows are in platform/console.cpp. Everything else reads these values and never asks which console it is on;
// source/platform/heaps.cpp, which splits the memory before any of this runs, reads the memory mode itself.
struct ConsoleProfile
{
	// ---- the CPU
	bool slowCpu;                    // an Old 3DS's (the log)
	int frameCap;                    // the frame rate it starts capped at (main hands it to C3D_FrameRate)
	bool capSteps;                   // the cap steps between 30 and 20 with the scene's work (Session::adaptView)
	float viewScaleMin, viewScaleMax;    // how far the outdoor view may close in, and its longest
	bool keepViewIndoors;            // indoors the outdoor view stays as it was (else whole again)
	bool viewStartsShort;            // the dynamic view starts at its shortest and opens out while there is room (else it
	                                 // starts at its longest and closes in when slow), and again after each loading screen
	float workCap;                   // one long frame (a cell arriving) counts as at most this, in ms
	float actorViewLimit;            // people are drawn no farther than this (0: the renderer's own distance)
	float actorDetailScale;          // of the renderer's full-detail distance for people
	float detailScale;               // of the clutter's own cutoff (CellBatch::maxDist: the converter's size tiers)
	unsigned skinEvery[3];           // a skinned pose is redone every n frames: near, middle, far
	bool rgb565;                     // 16-bit colour for the 3D screen (half the bandwidth)
	int cmdBufScale;                 // GPU command buffer, in libctru's default sizes
	bool goldenEyeAtStart;           // the GoldenEye control layout (no C-stick or ZL / ZR without a Circle Pad Pro)
	int spriteBudget;                // particle sprites drawn a frame (spell visuals, bolts' trails), shared evenly among
	                                 // them; it follows the frame's work (Session::adaptView) between these two
	int spriteBudgetMin, spriteBudgetMax;
	bool rainExtras;                 // the far rain's ring and the ripples where drops land (Session::drawPrecipitation)
	bool textureMips;                // the world's textures draw their smaller copies far off (mwrender/objects.cpp loadTexture)

	// ---- the heap
	bool smallHeap;                  // under 64 MB (an Old 3DS's memory mode: about 41 MB)
	int refBudget;                   // objects kept in memory
	unsigned soundCacheBytes;
	bool cleanAtDoors;               // a door frees what the last place left (Session::cleanForLoad, skeletons, string pool)
	bool streamWaitsForBlock;        // a cell streams in only when the heap has a block big enough for it
};

extern ConsoleProfile g_profile;
extern bool g_sysCoreThreads;        // the system core takes a thread of ours (APT_SetAppCpuTimeLimit worked): cell streaming

void consoleProfileInit(bool new3ds, size_t heapBytes);
