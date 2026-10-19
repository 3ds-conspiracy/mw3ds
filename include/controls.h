#pragma once

#include <3ds.h>
#include <citro3d.h>
#include <string>
#include <vector>

#include "player.h"
#include "renderer.h"

// How far (pixels) a finger may wander between press and release and still tap
static const int kTapSlop = 12;

// GoldenEye (Options > Controls): one stick, for an Old 3DS without the C-stick and ZL / ZR, on the Xbox layout's
// buttons. The Circle Pad walks (up / down) and turns (left / right); an aim button held roots the player and the
// pad aims instead, up and down too, at the look speed (half of it was GoldenEye's aim: too slow on the console): A held,
// or ZL / ZR (Circle Pad Pro / New 3DS). R winds up the attack and leaves the pad walking (it aimed too: the player stood
// rooted while winding up a blow). A tap of A activates, when it is let go (a hold aims instead). X readies or puts away
// the weapon, L casts, Y jumps (and rises, levitating), B sneaks on / off, SELECT first / third person (as on the other
// layouts), D-pad strafes (left / right) and looks up / down; quick keys are on the touch screen. (The aim was on a
// hold of X, beside Y: a thumb on Y to jump while walking caught X and the pad looked up.) Device and scripted runs
// both come through here, each with its own GoldenEyeState; dt: the frame's seconds (A's tap or hold)
struct GoldenEyeState
{
	float aHeld = 0.0f;               // seconds A has been held
	bool sneakOn = false;
};

// A menu is up this frame (the frame loop sets it before the input is read): GoldenEye's B sneaks only outside one
extern bool g_menuForInput;

// The C-stick up / down for the menus' text panes: -1 (up) to 1 (down), nothing inside the dead zone
float readStickScroll();
void goldenEyeInput(GoldenEyeState& ge, PlayerInput& in, float padX, float padY, u32 held, u32 down, u32 up, float dt);
// The gyroscope let go
void gyroStop();
// The look speed (Options) as a factor
float lookScale();
PlayerInput invertLook(PlayerInput in);
// The buttons, the pads and the gyroscope as the player's input this frame, in the layout chosen in Options
PlayerInput readInput(u32 held, u32 down, u32 up, float dt);
