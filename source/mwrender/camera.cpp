// The camera: first and third person, stereo 3D depth, the adaptive view distance
#include <algorithm>
#include <array>
#include <functional>
#include <sys/stat.h>
#include <tex3ds.h>
#include <citro2d.h>
#include "mwbase/session.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "components/debug/debuglog.hpp"
#include <malloc.h>

// SELECT (OpenMW's TogglePOV, its camera.lua): a tap, let go within kSwitchLimit, switches first / third person as
// it is let go. Held, the camera turns round the player (OpenMW's preview mode): at once in third person, once held
// past kSwitchLimit in first person, which comes back when it is let go. Meanwhile the look (the C-stick, the D-pad
// where it looks, gyro aim, a stylus slide) and the Circle Pad's left / right swing the camera round and tilt it; the
// Circle Pad's up / down zooms in and out (OpenMW's Zoom3rdPerson, the mouse wheel there), and the zoom stays. The
// player stands still: the pads are the camera's. Let go, the camera is back behind the player
void Session::povUpdate(PlayerInput& in, bool menu, float dt)
{
	const float kSwitchLimit = 0.25f;
	// (OpenMW zooms 30 to its Maximum Camera Distance, 800 by default; nearer than 60 our camera, over the head, sees
	// into it)
	const float kZoomMin = 60.0f, kZoomMax = 800.0f;
	// (not while a script has the view switch off, as Morrowind's CharGen script has until character creation is over:
	// OpenMW's view key does nothing then, a tap or a hold alike, its playerviewswitch control)
	bool can = !menu && !playerDead && !(w.controlsOff & 2u);
	if (povHold < 0.0f && in.togglePov && can)
		povHold = 0.0f;
	if (povHold < 0.0f)
		return;
	if (in.povHeld && can)
	{
		povHold += dt;
		if (!orbiting && (thirdPerson || povHold >= kSwitchLimit))
		{
			orbiting = true;
			orbitFromFirst = !thirdPerson;
			orbitYaw = 0.0f;
			orbitPitch = w.player.pitch;
			if (!thirdPerson)
			{
				thirdPerson = true;
				thirdDistance = 0.0f;
				body.acting = BODY_NONE;
			}
		}
	}
	else
	{
		// Let go (or a menu came up, or the player died): back as it was, or the other view after a tap
		bool tap = can && povHold <= kSwitchLimit, was = thirdPerson;
		if (orbiting && orbitFromFirst)
			thirdPerson = false;
		orbiting = orbitFromFirst = false;
		povHold = -1.0f;
		if (tap)
			thirdPerson = !thirdPerson;
		if (thirdPerson != was)
		{
			thirdDistance = 0.0f;
			body.acting = BODY_NONE;          // a blow or cast under way when the view changes starts afresh
		}
		return;
	}
	if (!orbiting)
		return;
	const float kDegrees = 3.14159265f / 180.0f;
	orbitYaw += (in.lookX + in.moveX) * kLookSpeed * dt + in.gyroYaw * kDegrees * dt + in.stylusYaw;
	orbitPitch += in.lookY * kLookSpeed * dt + in.gyroPitch * kDegrees * dt + in.stylusPitch;
	orbitPitch = fmaxf(-1.5f, fminf(1.5f, orbitPitch));
	// (a full push takes it from 220 to 800 in about a second, the same share of the distance a second however far)
	thirdZoom = fmaxf(kZoomMin, fminf(kZoomMax, thirdZoom * expf(-in.moveY * 1.2f * dt)));
	in.moveX = in.moveY = in.lookX = in.lookY = 0.0f;
	in.gyroYaw = in.gyroPitch = in.stylusYaw = in.stylusPitch = 0.0f;
	in.quick = -1;
}

// Third person: the camera orbits a point just above the head, thirdZoom units back along the view (220 unless zoomed),
// pulled in front of walls: looking up swings it down behind the player, looking down swings it up over them, and the
// player stays in the middle of the view (OpenMW's third-person camera: the focal point minus the view direction times
// the distance). SELECT held turns it round the player (povUpdate). The face preview looks at the player's face from
// in front
void Session::viewCamera(RenderCamera& cam)
{
	if (!thirdPerson && !previewFace)
		return;
	const Player& p = w.player;
	bool orbit = orbiting && !previewFace && !(playerDead && deathView);
	const float yaw = p.yaw + (previewFace ? previewSpin : orbit ? orbitYaw : 0.0f);
	float fx = sinf(yaw), fy = cosf(yaw);
	float head[3] = { p.feet[0], p.feet[1], p.feet[2] + (previewFace ? 118.0f : 120.0f) };
	float want = previewFace ? 70.0f : thirdZoom;
	float dir = previewFace ? 1.0f : -1.0f;       // the preview stands in front, facing back
	// (dead: from behind and above, looking down at the body where it fell. The face preview is level with the face,
	// whatever way the player looked when it opened: looking up, it sat under the chin, aimed level at the chest)
	const float pitch = playerDead && deathView ? -0.6f : previewFace ? 0.0f : orbit ? orbitPitch : p.pitch;
	float cp = cosf(pitch), sp = sinf(pitch);
	float to[3] = { head[0] + dir * fx * cp * want, head[1] + dir * fy * cp * want, head[2] - sp * want };
	// walls between the head and the camera pull it in
	float best = 1.0f;
	for (LoadedCell* l : w.loaded)
	{
		float t;
		if (collisionRaycast(l->cell.collision, head, to, &t) && t < best)
			best = t;
	}
	float d = fmaxf(20.0f, want * best - 12.0f);
	thirdDistance = thirdDistance <= 0.0f ? d : (d < thirdDistance ? d : thirdDistance + (d - thirdDistance) * 0.1f);
	for (int k = 0; k < 3; k++)
		cam.pos[k] = head[k] + (to[k] - head[k]) * (thirdDistance / want);
	if (previewFace)
	{
		cam.yaw = yaw + 3.14159265f;
		cam.pitch = 0.0f;
	}
	else if (playerDead && deathView)
		cam.pitch = pitch;
	else if (orbit)
	{
		cam.yaw = yaw;
		cam.pitch = pitch;
	}
}

// ---- Stereo 3D

// One eye's render time, smoothed over about a second (a stereo frame counts half per eye)
void Session::noteRenderCost(float cpuMs, float gpuMs, bool stereo, float eyeCmd)
{
	float cost = fmaxf(cpuMs, gpuMs) * (stereo ? 0.5f : 1.0f);
	renderCost += (cost - renderCost) * 0.05f;
	// GPU command buffer share of one eye: rises at once, falls slowly
	eyeCmdUse = eyeCmd > eyeCmdUse ? eyeCmd : eyeCmdUse + (eyeCmd - eyeCmdUse) * 0.02f;
}

// Distance to what the crosshair points at: the target when there is one, else the first wall / ground
// along the view (up to 3000)
float Session::aimDepth()
{
	const float kFar = 3000.0f;
	float cp = cosf(w.player.pitch);
	float dir[3] = { sinf(w.player.yaw) * cp, cosf(w.player.yaw) * cp, sinf(w.player.pitch) };
	float from[3] = { w.player.feet[0], w.player.feet[1], playerEyeZ(w.player) };
	float to[3] = { from[0] + dir[0] * kFar, from[1] + dir[1] * kFar, from[2] + dir[2] * kFar };
	float best = kFar;
	for (LoadedCell* l : w.loaded)
	{
		float t;
		if (collisionRaycast(l->cell.collision, from, to, &t))
			best = fminf(best, t * kFar);
	}
	if (target >= 0 && w.active(target))
	{
		const Ref& r = w.refs[target];
		float dx = r.pos[0] - from[0], dy = r.pos[1] - from[1];
		best = fminf(best, sqrtf(dx * dx + dy * dy));
	}
	return fmaxf(best, 20.0f);
}

// 0 or 1: whether this frame should be stereo: the player's choice (the 3D slider), whatever the frame rate; only a
// second eye that wouldn't fit the GPU command buffer waits
float Session::stereoTarget(float slider, float dt)
{
	(void)dt;
	if (slider <= 0.0f || w.current < 0)
		return 0.0f;
	// A second eye has to fit in the frame's GPU command buffer next to the first (and the HUDs)
	if (!stereoCmdRoom && eyeCmdUse < 0.3f)
		stereoCmdRoom = true;
	else if (stereoCmdRoom && eyeCmdUse > 0.4f)
		stereoCmdRoom = false;
	return stereoCmdRoom ? 1.0f : 0.0f;
}

// After each frame: the frame rate cap, the particle budget, and View distance Auto's view outdoors
//
// An Old 3DS (a CPU a third as fast) caps the frame rate instead of letting it wander: 30 while a frame's work stays
// under 30 fps' 33 ms, 20 once it keeps running past, 30 again once it keeps well under. Both are even shares of
// the screen's 60 Hz: a cap of 24 would show frames for 2 and 3 refreshes in turn, a judder worse than a steady 20
//
// View distance Auto, two ways by console (g_profile.viewEases):
//
// A New 3DS (user, 2026-11-11: as it was) eases the fog: it runs uncapped at the screen's 60 Hz, so a frame that misses
// a refresh waits for the next one: 30 ms of measured work showed 23-25 fps on the console (3D on, outdoors), between
// the work thresholds, and the view stayed whole. It closes in by the time between frames, the frame rate the player
// sees, and opens out only while the work leaves room for 30 fps, drawn in steps of kViewStep
//
// An Old 3DS (user, 2026-11-10) switches between two views: the console's longest, dropped to its middle one in a
// single step when the frame rate over a second falls under its floor (20), and tried at the longest again a second
// later. Dropped again soon after a try, the next try waits twice as long (up to half a minute: a heavy town isn't
// tried every other second); once the longest has held for ten seconds the wait is a second again. Only those two
// views, measured only once a second. Eased, it moved the fog a little every frame, out from its shortest after every
// trip: the Old 3DS sat at its shortest view for minutes at 13-18 fps, the frames no faster there (the time went to the
// people, not the land), and it holds its longest most of the time
static const float kViewStep = 0.05f;      // eased: the drawn view moves in steps of this share of the full view (viewDrawn)
static const float kHighHolds = 10.0f;     // two views: seconds at the longest view that put the next drop's wait back to one
static const float kRetryWaitMax = 30.0f;  // ... and the longest wait before the longest view is tried again

void Session::adaptView(float workMs, float frameMs)
{
	u64 tick = svcGetSystemTick();
	if (frameMs <= 0.0f)
		frameMs = adaptTick ? (float)(tick - adaptTick) * 1000.0f / SYSCLOCK_ARM11 : workMs;
	// (a frame takes at least its work: the GPU's drawing too. On the console the time between frames already holds it;
	// the PC build's GPU estimate (GPUMODEL:on) shows here only through this)
	frameMs = fmaxf(frameMs, workMs);
	adaptTick = tick;
	// One long frame (a cell arriving) counts at most as a slow one
	workCost += (fminf(workMs, g_profile.workCap) - workCost) * 0.1f;
	frameCost += (fminf(frameMs, g_profile.workCap) - frameCost) * 0.1f;
	if (g_profile.capSteps)
	{
		// 30, 20 while the place is too heavy:
		// two seconds past the 30 fps budget: 20; four seconds well under it: 30 again
		// (outdoors too: it was held at 20 there whatever the work, and a quiet road at 22 ms of work ran at 20 for nothing)
		int cap = frameCap;
		{
			bool slow = frameCap == 30 ? workCost > 31.0f : workCost < 24.0f;
			capFrames = slow ? capFrames + 1 : 0;
			if (capFrames >= (frameCap == 30 ? 60 : 80))
			{
				cap = frameCap == 30 ? 20 : 30;
				capFrames = 0;
			}
		}
		if (cap != frameCap)
		{
			frameCap = cap;
			logf("view: frame rate capped at %d (frame work %.1f ms, view %.0f%%)", frameCap, workCost, viewScale * 100.0f);
		}
	}
	// Particle sprites (spell visuals): fewer at once while frames run past the budget, quickly; more again, slowly,
	// once there is room (indoors too: a fight is the same work there). Spread evenly over the visuals showing
	float spriteMs = g_profile.capSteps ? (frameCap == 30 ? 30.0f : 45.0f) : 30.0f;
	int& sb = g_profile.spriteBudget;
	if (workCost > spriteMs)
		sb = std::max(g_profile.spriteBudgetMin, sb - 8);
	else if (workCost < spriteMs - 6.0f)
		sb = std::min(g_profile.spriteBudgetMax, sb + 2);
	// (Mournhold's big areas under the sky adapt as outdoors: case perf-mournhold-sky-interiors)
	if (w.current < 0 || !w.cells[w.current].underSky())
	{
		// (indoors the view is whole whatever this says: the Old 3DS keeps its outdoor view for the way out)
		if (!g_profile.keepViewIndoors)
			viewScale = viewDrawn = 1.0f;
		viewWindowMs = 0.0f, viewWindowFrames = 0;
		return;
	}
	if (g_profile.viewEases)
	{
		// (past 34.5 ms between frames is under 29 fps; under 27 ms of work held 29-30 fps on the console)
		const float minScale = g_profile.viewScaleMin, maxScale = g_profile.viewScaleMax;
		if (frameCost > 34.5f)
			viewScale = fmaxf(minScale, viewScale - 0.004f);
		else if (workCost < 27.0f)
			viewScale = fminf(maxScale, viewScale + 0.002f);
		// The drawn view (viewDrawn): the nearest step once the scale is a whole step away from the one drawn. At the
		// threshold the scale saws between -0.004 and +0.002 a frame, well inside a step, so the fog stands still; a real
		// change of load moves it a step at a time
		if (fabsf(viewScale - viewDrawn) >= kViewStep - 0.0005f)
			viewDrawn = fmaxf(minScale, fminf(maxScale, roundf(viewScale / kViewStep) * kViewStep));
		return;
	}
	const float high = g_profile.viewScaleMax, mid = 0.5f * (g_profile.viewScaleMin + g_profile.viewScaleMax);
	// (one long frame, a cell arriving, counts as a slow one, no more)
	viewWindowMs += fminf(frameMs, g_profile.workCap);
	viewWindowFrames++;
	if (viewWindowMs < 1000.0f)
		return;
	float fps = viewWindowFrames * 1000.0f / viewWindowMs, secs = viewWindowMs / 1000.0f;
	viewWindowMs = 0.0f, viewWindowFrames = 0;
	if (viewScale > mid + 0.001f)
	{
		viewScale = viewDrawn = high;
		viewHighFor += secs;
		if (viewHighFor >= kHighHolds)
			viewRetryWait = 1.0f;
		if (fps < g_profile.viewFloorFps)
		{
			viewScale = viewDrawn = mid;
			viewRetryIn = viewRetryWait;
			logf("view: Auto at the middle view (%.1f fps), the longest tried again in %.0f s", fps, viewRetryIn);
			viewRetryWait = fminf(kRetryWaitMax, viewRetryWait * 2.0f);
			viewHighFor = 0.0f;
		}
	}
	else if ((viewRetryIn -= secs) <= 0.0f)
	{
		viewScale = viewDrawn = high;
		viewHighFor = 0.0f;
		logf("view: Auto at the longest view (%.1f fps at the middle)", fps);
	}
	else
		viewScale = viewDrawn = mid;
}
