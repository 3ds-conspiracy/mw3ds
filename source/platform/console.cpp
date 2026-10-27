#include "platform/console.hpp"

ConsoleProfile g_profile;
bool g_sysCoreThreads = false;

void consoleProfileInit(bool new3ds, size_t heapBytes)
{
	ConsoleProfile& p = g_profile;
	if (new3ds)
	{
		p.slowCpu = false;
		p.frameCap = 60;
		p.capSteps = false;
		p.viewScaleMin = 0.6f;
		p.viewScaleMax = 1.0f;
		p.keepViewIndoors = false;
		p.viewStartsShort = false;
		p.workCap = 45.0f;
		p.actorViewLimit = 0.0f;
		p.actorDetailScale = 1.0f;
		p.detailScale = 1.0f;
		p.skinEvery[0] = 2, p.skinEvery[1] = 3, p.skinEvery[2] = 4;
		// People are posed by time, as on an Old 3DS, a step above it (user, 2026-10-27): 30 a second for the three most in
		// view, 15 for the rest near and the middle, 10 far. Counted in frames with the steps shortened below 50 fps, a
		// Balmora street at 30 fps was posed 1.8 x as often and ran slower (perf-new3ds-pose-budget)
		p.poseHz[0] = 30.0f, p.poseHz[1] = 15.0f, p.poseHz[2] = 10.0f;
		p.poseFull = 3;
		p.rgb565 = false;
		p.cmdBufScale = 8;           // room for both eyes of a busy town
		p.goldenEyeAtStart = false;
	}
	else
	{
		// A CPU a third as fast: a steady 20 fps outdoors (an even share of the screen's 60 Hz), 30 indoors (20 when a
		// room is too heavy), a shorter view outdoors, people drawn and animated less far
		p.slowCpu = true;
		p.frameCap = 30;
		p.capSteps = true;
		p.viewScaleMin = 0.3f;       // (2150 units: as far as it closes in to hold 20 fps)
		p.viewScaleMax = 0.8f;
		p.keepViewIndoors = true;
		p.viewStartsShort = true;    // (the view it can hold is found from below: not a slow start, then closing in)
		p.workCap = 70.0f;
		p.actorViewLimit = 4000.0f;
		p.actorDetailScale = 0.67f;
		p.detailScale = 0.8f;        // (the size tiers' 2500 / 5000 / 10000 as 2000 / 4000 / 8000: trial-detail-distance)
		p.skinEvery[0] = 2, p.skinEvery[1] = 4, p.skinEvery[2] = 6;
		// People are posed by time, not frames: counted in frames, a crowd that dropped the cap to 20 fps was posed
		// 10, 5 and 3 times a second and looked choppy. Near, up to 20 a second for the two most in view (the rest
		// near take the middle's 10), far 6.7; all the posing of a busy Balmora was under a millisecond of the frame
		p.poseHz[0] = 20.0f, p.poseHz[1] = 10.0f, p.poseHz[2] = 20.0f / 3.0f;
		p.poseFull = 2;
		p.rgb565 = false;            // (16-bit banded the sky and the fog; the GPU and its memory are the New 3DS's)
		p.cmdBufScale = 2;           // 1.5 MB of linear memory back: a busy town peaked at 18% of 1 MB (a second eye draws only
		                             // while it fits: Session::stereoTarget)
		p.goldenEyeAtStart = true;
	}
	p.smallHeap = heapBytes < (64u << 20);
	p.refBudget = p.smallHeap ? 5000 : 20000;
	// (sounds are DSP-ADPCM, a little over a quarter of the PCM16 the caps were once set for: half the bytes holds
	// nearly twice the sounds)
	p.soundCacheBytes = p.smallHeap ? 1u << 20 : 3u << 20;
	p.cleanAtDoors = p.smallHeap;
	p.streamWaitsForBlock = p.smallHeap;
	p.spriteBudget = p.slowCpu ? 200 : 400;
	p.spriteBudgetMin = p.slowCpu ? 100 : 200;
	p.spriteBudgetMax = p.slowCpu ? 400 : 600;
	// (an Old 3DS has the streaks alone: it has the least to spare in a busy town, in 3D most of all)
	p.rainExtras = !p.slowCpu;
	// (snow as ash's flakes on a New 3DS, half on an Old one: spec/performance-tradeoffs.md)
	p.snowFlakes = p.slowCpu ? 120 : 240;
	// (an Old 3DS draws from Project Atlas's sheets: a sheet's smaller copies blend its pictures into each other, and the
	// far walkways and walls showed dark patches of the trim packed under them, flickering in and out)
	p.textureMips = !p.slowCpu;
}
