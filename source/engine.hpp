#pragma once

// The game as OpenMW's OMW::Engine: start-up, the frame loop and the exit, shared by the console build and the PC test
// build (appMain, platform/platform.hpp, runs one)
#include <cstddef>
#include <vector>

#include <citro3d.h>

#include "testdrive/autoinput.hpp"
#include "testdrive/testdrive.hpp"

struct Session;

struct Engine
{
	int go();                                // start-up, frames until the player quits, the exit
	bool prepare();
	bool retryFromTitle(bool& loadedOut);
	bool frame();
	void shutdown();

	// What the frame loop keeps from one frame to the next
	bool isNew3ds = false;
	bool romfs = false;
	C3D_RenderTarget* top = nullptr;
	C3D_RenderTarget* topRight = nullptr;      // the right eye (drawn only in stereo 3D)
	C3D_RenderTarget* bottom = nullptr;
	bool stereoOn = false;
	// The top screen's depth buffer: 24 bits (and a stencil byte nothing uses) in 2D; 16 bits while 3D is on, half the
	// depth traffic for the two eyes (a trial: 16 bits of depth may show far surfaces fighting on the console). Both
	// pairs of targets are made at the start (making one mid-game crashed the emulator); the screen's output is
	// pointed at the pair in use (top, topRight)
	bool topDepth16 = false;
	C3D_RenderTarget* topPair[2][2] = {};      // [depth16][eye]
	void createTopTargets();
	void useTopDepth(bool depth16);
	const char* dataDir = nullptr;
	Session* session = nullptr;
	bool loaded = false;                       // the session started (else the frame shows "Load failed")
	bool testRun = false;                      // a scripted run or an autocam: no title screen
	// Scripted runs (testdrive/steprunner.cpp)
	ScriptedSteps autoinput;
	size_t inputIndex = 0;
	float inputTime = 0.0f;
	TestDriver driver;
	bool chainMode = false;
	bool stepBlocking = false;                 // this step waits for the driver (a walk, a kill, an activation)
	bool stepShot = false;
	int stepShots = 0;
	bool autoShot = false;
	std::vector<CamPose> autocam;
	size_t poseIndex = 0;
	int poseFrames = 0;
	// The frame clock and the second's statistics
	u64 lastTick = 0, statTick = 0;
	int frames = 0;
	float cmdPeak = 0.0f;                      // most of the GPU command buffer a frame used this second
	float t = 0.0f;
	// The touch screen between frames: taps and drags
	bool touchWasDown = false, dragged = false;
	int lastTouchX = 0, lastTouchY = 0, pressX = 0, pressY = 0;
};
