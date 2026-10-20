#pragma once

// The game is the same code in the console build and the PC test build (tools/test/native): source/engine.cpp. Each build's
// entry file defines these few things and calls appMain, nothing else (tools/test/native/build.py refuses an entry file
// that grows), so what the two run can't drift apart:
//   source/main.cpp                    the console
//   tools/test/native/main_native.cpp  the PC test build

// Start-up, the title screen and the frame loop until the game is left (engine.cpp)
int appMain();

// A scripted run is over: its last step taken and the game saved, or (platformFailFast) its first failure. The PC test
// build ends the process there; the console carries on
void platformRunOver();

// MW3DS_FAILFAST=1 on the PC test build: a run stops at its first failure. Never on the console
bool platformFailFast();

// Before gfxInit: the console's VRAM cleared to black (gfxInit puts the screens' buffers there and lights the screens on
// them at once, before the game's first frame). Nothing on the PC test build
void platformClearVram();
