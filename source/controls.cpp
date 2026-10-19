// The buttons, the pads and the gyroscope as the player's input (controls.h)
#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <malloc.h>
#include <string>
#include <sys/stat.h>
#include <vector>
#include "audio.h"
#include "distant.h"
#include "linear.h"
#include "memstat.h"
#include "log.h"
#include "player.h"
#include "gamma.h"
#include "renderer.h"
#include "screenshot.h"
#include "testdrive.h"
#include "zfile.h"
#include "saves.h"
#include "session.h"
#include "ui.h"
#include "platform.h"
#include "controls.h"

// The C-stick up / down for the menus' text panes: -1 (up) to 1 (down), nothing inside the dead zone
float readStickScroll()
{
	circlePosition cs;
	hidCstickRead(&cs);
	float x, y;
	stickAxes(cs.dx, cs.dy, stickZone(), x, y);
	return -y;
}

// The C-stick left / right: -1 to 1, nothing inside the dead zone
float readStickTurn()
{
	circlePosition cs;
	hidCstickRead(&cs);
	float x, y;
	stickAxes(cs.dx, cs.dy, stickZone(), x, y);
	return x;
}

// A menu is up this frame (main sets it before the input is read): GoldenEye's B toggles sneaking only outside one. The
// B that closed a menu crouched the player as it went
bool g_menuForInput = false;
void goldenEyeInput(GoldenEyeState& ge, PlayerInput& in, float padX, float padY, u32 held, u32 down, u32 up, float dt)
{
	float& aHeld = ge.aHeld;
	bool& sneakOn = ge.sneakOn;
	const float kTap = 0.25f;           // A let go before this: activate; held longer: only aimed
	if (held & KEY_A)
		aHeld += dt;
	bool aiming = held & (KEY_A | KEY_ZL | KEY_ZR);   // (A from the press on: a quick let-go activates instead)
	in.activate = (up & KEY_A) && aHeld < kTap;
	if (!(held & KEY_A))
		aHeld = 0.0f;
	in.readyToggle = down & KEY_X;
	in.moveX = in.moveY = 0.0f;
	// (Controls > Look: the pad's turn while walking, its look while aiming and the D-pad's look up and down)
	float turn = g_geTurnSpeed / 100.0f, aim = g_lookSpeed / 100.0f;
	if (aiming)
	{
		in.lookX += padX * aim;
		in.lookY += padY * aim;
	}
	else
	{
		in.lookX += padX * turn;
		in.moveY = padY;
	}
	// D-pad: left / right strafe, up / down look up and down (the user's GoldenEye; the quick keys are the touch strip's)
	if (held & KEY_DLEFT) in.moveX -= 1.0f;
	if (held & KEY_DRIGHT) in.moveX += 1.0f;
	if (held & KEY_DUP) in.lookY += aim;
	if (held & KEY_DDOWN) in.lookY -= aim;
	if ((down & KEY_B) && !g_menuForInput)
		sneakOn = !sneakOn;
	in.attack = held & KEY_R;
	in.cast = down & KEY_L;
	in.jump = down & KEY_Y;
	in.sneak = sneakOn;
	in.togglePov = down & KEY_SELECT;
}

// Gyro aim (g_gyroAim): the console's turn rates into the input, degrees a second, scaled by the sensitivity.
// The sensor runs only while it is on; the resting offset is taken off and turns under kSoft a second softened
static bool s_gyroOn = false;

bool g_gyroCalibrate = false;      // Gyro aim set to a speed (Options): measure the gyroscope's offsets (GyroRest::calibrate)
int g_gyroCalNotice = 0;           // how the last one asked for went (1 took, -1 the console moved): Session says so

// The sensors off again: the HID module keeps them running for the console after the game has gone otherwise
void gyroStop()
{
	if (!s_gyroOn)
		return;
	HIDUSER_DisableGyroscope();
	HIDUSER_DisableAccelerometer();
	s_gyroOn = false;
}

static void gyroInput(PlayerInput& in)
{
	static float toDps = 0.0f;
	if ((g_gyroAim > 0) != s_gyroOn)
	{
		if (g_gyroAim > 0)
		{
			s_gyroOn = true;
			HIDUSER_EnableGyroscope();
			HIDUSER_EnableAccelerometer();
			if (R_FAILED(HIDUSER_GetGyroscopeRawToDpsCoefficient(&toDps)))
				toDps = 0.0f;
		}
		else
			gyroStop();
	}
	if (!s_gyroOn)
		return;
	const float kSoft = 1.0f;
	angularRate r;
	hidGyroRead(&r);
	float ax = gyroRawToDps(r.x, toDps), ay = gyroRawToDps(r.y, toDps), az = gyroRawToDps(r.z, toDps);
	// The sensor's resting offset taken off (GyroRest)
	static GyroRest rest;
	static u64 last = 0, logged = 0;
	static bool firstCal = true, asked = false;  // (once as the sensor starts: kept only if the console lay still)
	u64 now = osGetTime();
	float rate[3] = { ax, ay, az };
	if (g_gyroCalibrate || firstCal)
	{
		asked = g_gyroCalibrate;
		g_gyroCalibrate = firstCal = false;
		rest.calibrate();
	}
	// Gravity from the accelerometer, steadied over a few frames (GyroRest learns the up / down offset by it)
	static float grav[3] = { 0.0f, -512.0f, 0.0f };
	accelVector acc;
	hidAccelRead(&acc);
	if (acc.x || acc.y || acc.z)
	{
		const float a[3] = { (float)acc.x, (float)acc.y, (float)acc.z };
		for (int k = 0; k < 3; k++)
			grav[k] += (a[k] - grav[k]) * 0.1f;
	}
	float step = last ? fminf(0.1f, (now - last) / 1000.0f) : 1.0f / 30.0f;
	rest.gravity(grav, step);
	int cal = rest.update(rate, step);
	if (cal)
	{
		logf("gyro: calibration %s, offsets %.2f %.2f %.2f dps", cal > 0 ? "took" : "refused (the console moved)",
			rest.rest[0], rest.rest[1], rest.rest[2]);
		if (asked)
			g_gyroCalNotice = cal;
		asked = false;
	}
	last = now;
	// (the offsets learned, every 10 s: a view that drifts on the console shows here)
	if (now - logged >= 10000)
	{
		logged = now;
		logf("gyro: offsets %.2f %.2f %.2f dps, held still %.1f s, gravity turning %.1f dps, x off level %.2f", rest.rest[0],
			rest.rest[1], rest.rest[2], rest.still, rest.gTurn, rest.gLevel);
	}
	// (while the console turns, its axes and gravity go to the log twice a second: the signs are still to be read on
	// the device)
	static int logFrames = 0;
	if (++logFrames >= 30 && (fabsf(rate[0]) > 10.0f || fabsf(rate[1]) > 10.0f || fabsf(rate[2]) > 10.0f))
	{
		logFrames = 0;
		logf("gyro: x %.0f y %.0f z %.0f dps (raw %d %d %d, coefficient %.3f), gravity %.0f %.0f %.0f", rate[0], rate[1],
			rate[2], r.x, r.y, r.z, toDps, grav[0], grav[1], grav[2]);
	}
	// Left / right about the world's up, up / down about the console's left-right axis: player space
	float yaw, pitch;
	gyroPlayerSpace(rate, grav, yaw, pitch);
	in.gyroYaw = gyroSoften(yaw, kSoft) * g_gyroAim / 100.0f;
	in.gyroPitch = gyroSoften(pitch, kSoft) * g_gyroAim / 100.0f;
}

// Controls > Look: the C-stick's and the D-pad's look speed (the GoldenEye pad's are goldenEyeInput's), and up / down
// the other way round when Invert is on (gyro aim keeps its own: the console's own tilt)
float lookScale()
{
	return g_lookSpeed / 100.0f;
}

PlayerInput invertLook(PlayerInput in)
{
	if (g_invertY)
		in.lookY = -in.lookY;
	return in;
}

// Three button layouts (Options > Controls; START and SELECT the same in all):
//   Xbox (Morrowind on the Xbox): R attack, L cast, X ready / put away the weapon, Y jump, B free (walk or run by the pad),
//     ZL sneak (down when levitating), ZR up when levitating, D-pad quick keys
//   Classic: X attack, Y cast, B jump, L sneak, ZL + D-pad quick keys, D-pad look, ZL + X put away
//   GoldenEye: see goldenEyeInput
PlayerInput readInput(u32 held, u32 down, u32 up, float dt)
{
	circlePosition cp, cs;
	hidCircleRead(&cp);
	hidCstickRead(&cs);
	PlayerInput in = {};
	// (both sticks past a round dead zone, rescaled from its edge: stickAxes)
	stickAxes(cp.dx, cp.dy, stickZone(), in.moveX, in.moveY);
	float cx, cy;
	stickAxes(cs.dx, cs.dy, stickZone(), cx, cy);
	in.lookX = cx * lookScale();
	in.lookY = cy * lookScale();
	in.quick = -1;
	gyroInput(in);
	if (g_controlLayout == CONTROLS_GOLDENEYE)
	{
		static GoldenEyeState state;
		goldenEyeInput(state, in, in.moveX, in.moveY, held, down, up, dt);
		return invertLook(in);
	}
	if (g_controlLayout == CONTROLS_XBOX)
	{
		if (down & KEY_DUP) in.quick = 0;
		if (down & KEY_DRIGHT) in.quick = 1;
		if (down & KEY_DDOWN) in.quick = 2;
		if (down & KEY_DLEFT) in.quick = 3;
		in.attack = held & KEY_R;
		in.cast = down & KEY_L;
		in.readyToggle = down & KEY_X;
		in.jump = down & KEY_Y;
		in.fast = held & KEY_B;
		in.togglePov = down & KEY_SELECT;
		in.up = held & KEY_ZR;
		in.down = held & KEY_ZL;
		in.sneak = held & KEY_ZL;
		return invertLook(in);
	}
	if (held & KEY_ZL)
	{
		// quick keys: ZL + a direction
		if (down & KEY_DUP) in.quick = 0;
		if (down & KEY_DRIGHT) in.quick = 1;
		if (down & KEY_DDOWN) in.quick = 2;
		if (down & KEY_DLEFT) in.quick = 3;
	}
	else
	{
		if (held & KEY_DRIGHT) in.lookX += lookScale();
		if (held & KEY_DLEFT)  in.lookX -= lookScale();
		if (held & KEY_DUP)    in.lookY += lookScale();
		if (held & KEY_DDOWN)  in.lookY -= lookScale();
	}
	in.jump = down & KEY_B;
	in.togglePov = down & KEY_SELECT;
	in.fast = held & KEY_ZR;
	in.up = held & KEY_R;
	in.down = held & KEY_L;
	// ZL + X puts the weapon away (Morrowind's Ready Weapon toggle); X alone draws and strikes
	in.sheathe = (held & KEY_ZL) && (down & KEY_X);
	in.attack = (held & KEY_X) && !(held & KEY_ZL);
	in.cast = down & KEY_Y;
	in.sneak = held & KEY_L;
	return invertLook(in);
}
