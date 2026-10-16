#include "console.h"

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
		p.workCap = 45.0f;
		p.actorViewLimit = 0.0f;
		p.actorDetailScale = 1.0f;
		p.skinEvery[0] = 2, p.skinEvery[1] = 3, p.skinEvery[2] = 4;
		p.rgb565 = false;
		p.cmdBufScale = 8;           // room for both eyes of a busy town
		p.goldenEyeAtStart = false;
	}
	else
	{
		// A CPU a third as fast: 30 fps (20 when the scene is too heavy, an even share of the screen's 60 Hz), a shorter
		// view outdoors, people drawn and animated less far
		p.slowCpu = true;
		p.frameCap = 30;
		p.capSteps = true;
		p.viewScaleMin = 0.45f;
		p.viewScaleMax = 0.8f;
		p.keepViewIndoors = true;
		p.workCap = 70.0f;
		p.actorViewLimit = 4000.0f;
		p.actorDetailScale = 0.67f;
		p.skinEvery[0] = 2, p.skinEvery[1] = 4, p.skinEvery[2] = 6;
		p.rgb565 = true;
		p.cmdBufScale = 4;           // 1 MB of linear memory back (a second eye draws only while it fits: Session::stereoTarget)
		p.goldenEyeAtStart = true;
	}
	p.smallHeap = heapBytes < (64u << 20);
	p.refBudget = p.smallHeap ? 5000 : 20000;
	p.soundCacheBytes = p.smallHeap ? 2u << 20 : 6u << 20;
	p.cleanAtDoors = p.smallHeap;
	p.streamWaitsForBlock = p.smallHeap;
	p.spriteBudget = p.slowCpu ? 200 : 400;
	p.spriteBudgetMin = p.slowCpu ? 100 : 200;
	p.spriteBudgetMax = p.slowCpu ? 400 : 600;
}
