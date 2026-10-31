#pragma once

#include <3ds.h>
#include <citro3d.h>
#include <string>
#include <vector>

#include "mwworld/player.hpp"
#include "mwrender/renderingmanager.hpp"

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
float readStickTurn();
void goldenEyeInput(GoldenEyeState& ge, PlayerInput& in, float padX, float padY, u32 held, u32 down, u32 up, float dt);
// The gyroscope let go
void gyroStop();
// The look speed (Options) as a factor
float lookScale();
PlayerInput invertLook(PlayerInput in);
// The buttons, the pads and the gyroscope as the player's input this frame, in the layout chosen in Options
PlayerInput readInput(u32 held, u32 down, u32 up, float dt);
// The stylus layout's buttons (device and scripted runs alike)
void stylusInput(PlayerInput& in, u32 held, u32 down);
// The stylus layout: the HUD's glass buttons over the minimap, as the console's lie: round A B X Y at its right, half
// way down (A right, B below, X above, Y left), the shoulder buttons L and R as boxes in its top corners. The discs' centres and
// radius, the boxes' x, y, width, height, and which button a point is on (-1: none)
enum { STYLUS_PAD_A, STYLUS_PAD_B, STYLUS_PAD_X, STYLUS_PAD_Y, STYLUS_PAD_L, STYLUS_PAD_R, STYLUS_PAD_COUNT };
static const float kStylusPad[4][2] = { { 299, 90 }, { 282, 107 }, { 282, 73 }, { 265, 90 } };   // (centred on the map's height)
static const float kStylusPadRadius = 11.0f;
static const float kStylusShoulder[2][4] = { { 189, 29, 38, 15 }, { 273, 29, 38, 15 } };   // (3 pixels in from the map's corners)
int stylusPadAt(int x, int y);
// Stylus layouts: a slide on the bottom screen in play (inPlay) turns the view and is taken from the screen's input
struct UiInput;
void stylusLook(UiInput& ui, PlayerInput& in, bool inPlay);
