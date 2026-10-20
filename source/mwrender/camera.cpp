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

// Third person: the camera orbits a point just above the head, 220 units back along the view, pulled in front of
// walls: looking up swings it down behind the player, looking down swings it up over them, and the player stays in the
// middle of the view (OpenMW's third-person camera: the focal point minus the view direction times the distance).
// The face preview looks at the player's face from in front
void Session::viewCamera(RenderCamera& cam)
{
	if (!thirdPerson && !previewFace)
		return;
	const Player& p = w.player;
	const float yaw = p.yaw + (previewFace ? previewSpin : 0.0f);
	float fx = sinf(yaw), fy = cosf(yaw);
	float head[3] = { p.feet[0], p.feet[1], p.feet[2] + (previewFace ? 118.0f : 120.0f) };
	float want = previewFace ? 70.0f : 220.0f;
	float dir = previewFace ? 1.0f : -1.0f;       // the preview stands in front, facing back
	// (dead: from behind and above, looking down at the body where it fell. The face preview is level with the face,
	// whatever way the player looked when it opened: looking up, it sat under the chin, aimed level at the chest)
	const float pitch = playerDead && deathView ? -0.6f : previewFace ? 0.0f : p.pitch;
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

// Outdoors, the fog (and with it what is drawn) closes in while a frame's work runs past 30 fps
// and eases back out once there is room again; indoors the view stays whole
//
// An Old 3DS (a CPU a third as fast) caps the frame rate instead of letting it wander: 30 while a frame's work stays
// under 30 fps' 33 ms, 20 once it keeps running past, 30 again once it keeps well under. Both are even shares of
// the screen's 60 Hz: a cap of 24 would show frames for 2 and 3 refreshes in turn, a judder worse than a steady 20.
// Only past 20 fps' 50 ms does the fog close in (interiors, most of the game, hold 30)
//
// A New 3DS runs uncapped at the screen's 60 Hz, so a frame that misses a refresh waits for the next one: 30 ms of
// measured work showed 23-25 fps on the console (3D on, outdoors), between the work thresholds, and the view stayed
// whole. It closes in by the time between frames, the frame rate the player sees, and opens out only while the work
// leaves room for 30 fps
static const float kViewStep = 0.05f;      // the drawn view moves in steps of this share of the full view (viewDrawn)

void Session::adaptView(float workMs, float frameMs)
{
	u64 tick = svcGetSystemTick();
	if (frameMs <= 0.0f)
		frameMs = adaptTick ? (float)(tick - adaptTick) * 1000.0f / SYSCLOCK_ARM11 : workMs;
	adaptTick = tick;
	// One long frame (a cell arriving) shouldn't shrink the view: each counts at most as a slow one
	workCost += (fminf(workMs, g_profile.workCap) - workCost) * 0.1f;
	frameCost += (fminf(frameMs, g_profile.workCap) - frameCost) * 0.1f;
	float budget = 34.0f;
	if (g_profile.capSteps)
	{
		// Outdoors a steady 20 fps, the view giving way to keep it (below); indoors 30, 20 while a room is too heavy:
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
		budget = 44.0f;
	}
	// Particle sprites (spell visuals): fewer at once while frames run past the budget, quickly; more again, slowly,
	// once there is room (indoors too: a fight is the same work there). Spread evenly over the visuals showing
	float spriteMs = g_profile.capSteps ? (frameCap == 30 ? 30.0f : 45.0f) : 30.0f;
	int& sb = g_profile.spriteBudget;
	if (workCost > spriteMs)
		sb = std::max(g_profile.spriteBudgetMin, sb - 8);
	else if (workCost < spriteMs - 6.0f)
		sb = std::min(g_profile.spriteBudgetMax, sb + 2);
	float minScale = g_profile.viewScaleMin, maxScale = g_profile.viewScaleMax;
	if (w.current < 0 || w.cells[w.current].interior)
	{
		// (indoors the view is whole whatever this says: the Old 3DS keeps its outdoor view for the way out)
		if (!g_profile.keepViewIndoors)
			viewScale = viewDrawn = 1.0f;
		return;
	}
	// (New 3DS: past 34.5 ms between frames is under 29 fps; under 27 ms of work held 29-30 fps on the console. Old 3DS,
	// at its 20: past 51.5 ms between frames is under 19.4 fps, or the work near the budget; it grows back only with
	// room to spare. On the console it ran 18 fps while the work, under 47 ms, said all was well: a frame a little over
	// 50 ms waits for the next refresh)
	bool slow = g_profile.capSteps ? workCost > budget || frameCost > 51.5f : frameCost > 34.5f;
	bool room = g_profile.capSteps ? workCost < budget - 8.0f && frameCost < 51.5f : workCost < 27.0f;
	if (slow)
		viewScale = fmaxf(minScale, viewScale - (g_profile.capSteps ? 0.006f : 0.004f));
	else if (room)
		viewScale = fminf(maxScale, viewScale + 0.002f);
	// The drawn view (viewDrawn): the nearest step once the scale is a whole step away from the one drawn. At the
	// threshold the scale saws between -0.006 and +0.002 a frame, well inside a step, so the fog stands still; a real
	// change of load moves it a step at a time
	if (fabsf(viewScale - viewDrawn) >= kViewStep - 0.0005f)
		viewDrawn = fmaxf(minScale, fminf(maxScale, roundf(viewScale / kViewStep) * kViewStep));
}
