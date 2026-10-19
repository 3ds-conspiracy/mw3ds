// Following the player: the player's recent footsteps as a trail, where a follower heads
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "mwsound/soundmanagerimp.hpp"
#include "components/debug/debuglog.hpp"
#include "mwbase/session.hpp"
#include "mwdialogue/dialoguemanagerimp.hpp"
#include "mwmechanics/combathelpers.hpp"

// The player's recent footsteps: a point every 120 units walked, numbered as they come. A follower that is held up
// (the player out of sight behind a rock, or a ledge in the straight way) goes along them, the way the player went
// (OpenMW steers with a navigation mesh; the wilds have none here)
static const int kTrailMax = 48;
static float sTrail[kTrailMax][3];
static unsigned sTrailNum[kTrailMax];
static int sTrailCount = 0, sTrailHead = 0;      // points kept, and the slot of the next one
static unsigned sTrailNext = 1;
static float sTrailStamp = -1.0f;

static int trailSlot(int back)                   // 0: the newest
{
	return (sTrailHead - 1 - back + 2 * kTrailMax) % kTrailMax;
}

void trailRecord(const World& w)
{
	if (w.time == sTrailStamp)
		return;
	sTrailStamp = w.time;
	const float* p = w.player.feet;
	if (sTrailCount > 0)
	{
		const float* last = sTrail[trailSlot(0)];
		float dx = p[0] - last[0], dy = p[1] - last[1], d2 = dx * dx + dy * dy;
		if (d2 < 120.0f * 120.0f)
			return;
		if (d2 > 700.0f * 700.0f)      // a jump (door, teleport): not a way anyone can follow
			sTrailCount = 0;
	}
	memcpy(sTrail[sTrailHead], p, sizeof(float) * 3);
	sTrailNum[sTrailHead] = sTrailNext++;
	sTrailHead = (sTrailHead + 1) % kTrailMax;
	sTrailCount = std::min(sTrailCount + 1, kTrailMax);
}

// The footstep with this number (null when it is gone, or not made yet)
static const float* trailFind(unsigned num)
{
	if (sTrailCount == 0 || num >= sTrailNext || num + sTrailCount < sTrailNext)
		return nullptr;
	int slot = trailSlot((int)(sTrailNext - 1 - num));
	return sTrailNum[slot] == num ? sTrail[slot] : nullptr;
}

// The height of the player's way at (x, y): the nearest point of the line through the recent footsteps, within 150 units across
// (-1e9 when it is further, or there are no footsteps)
float trailHeightNear(float x, float y)
{
	float bestD = 150.0f * 150.0f, bestZ = -1e9f;
	for (int k = 0; k + 1 < sTrailCount; k++)
	{
		const float* a = sTrail[trailSlot(k)];
		const float* b = sTrail[trailSlot(k + 1)];
		float ex = b[0] - a[0], ey = b[1] - a[1], len2 = ex * ex + ey * ey;
		float t = len2 > 1.0f ? ((x - a[0]) * ex + (y - a[1]) * ey) / len2 : 0.0f;
		t = fmaxf(0.0f, fminf(1.0f, t));
		float dx = a[0] + ex * t - x, dy = a[1] + ey * t - y, d2 = dx * dx + dy * dy;
		if (d2 < bestD)
		{
			bestD = d2;
			bestZ = a[2] + (b[2] - a[2]) * t;
		}
	}
	return bestZ;
}

// The follower's goal toward `who` (the player). Held up (out of sight, or no headway for a second), it walks the
// player's footsteps from the one nearest it, skipping on to the farthest of the next two it can see once past one; else straight
const float* followGoal(World& w, Ref& r, const float* who, float dt)
{
	r.trailTimer -= dt;
	bool on = r.trailUntil > w.time;
	if (r.trailTimer > 0.0f)
		return on ? r.trailGoal : who;
	r.trailTimer = 0.3f;
	float a[3] = { r.pos[0], r.pos[1], r.pos[2] + 50.0f }, b[3] = { who[0], who[1], who[2] + 50.0f };
	if (!on)
	{
		// held up: no headway for a second, or the player out of sight for as long (and no path grid route to go by)
		bool seen = w.lineOfSight(a, b);
		r.trailBlind = seen ? 0.0f : r.trailBlind + 0.3f;
		if (!r.path.empty() || (r.chaseStill < 1.0f && r.trailBlind < 1.0f))
			return who;
		r.trailBlind = 0.0f;
		r.trailUntil = w.time + 10.0f;
		r.trailSeq = 0;
		on = true;
	}
	// the next footstep to walk to: the one after the nearest, or after the last reached
	unsigned want = 0;
	if (r.trailSeq == 0)
	{
		float best = 600.0f * 600.0f;
		for (int k = 0; k < sTrailCount; k++)
		{
			const float* p = sTrail[trailSlot(k)];
			float dx = p[0] - r.pos[0], dy = p[1] - r.pos[1], d2 = dx * dx + dy * dy;
			// nearest counting height twice over: a footstep on the slope the follower stands above is the one to
			// walk to, not one beyond the ledge below that happens to be nearer on the map
			float dz = (p[2] - r.pos[2]) * 2.0f, d3 = d2 + dz * dz;
			if (d3 < best)
			{
				best = d3;
				want = sTrailNum[trailSlot(k)] + (d2 > 70.0f * 70.0f ? 0 : 1);
			}
		}
	}
	else
	{
		const float* cur = trailFind(r.trailSeq);
		float dx = cur ? cur[0] - r.pos[0] : 0.0f, dy = cur ? cur[1] - r.pos[1] : 0.0f;
		float reach = r.chaseStill >= 1.0f ? 200.0f : 70.0f;       // held up near it counts as there
		want = cur && dx * dx + dy * dy > reach * reach ? r.trailSeq : r.trailSeq + 1;
	}
	const float* p = trailFind(want);
	if (!p)
	{
		r.trailUntil = 0.0f;         // the footsteps run out: on to the player
		r.trailSeq = 0;
		return who;
	}
	bool reached = r.trailSeq != 0 && want != r.trailSeq;      // the last one is behind it: look ahead
	r.trailSeq = want;
	for (unsigned n = want + 1; n <= want + 2 && reached; n++)
	{
		const float* q = trailFind(n);
		float c[3] = { q ? q[0] : 0.0f, q ? q[1] : 0.0f, q ? q[2] + 50.0f : 0.0f };
		if (q && w.lineOfSight(a, c) && fabsf(q[2] - r.pos[2]) < 60.0f)
		{
			p = q;
			r.trailSeq = n;
		}
	}
	memcpy(r.trailGoal, p, sizeof(r.trailGoal));
	return r.trailGoal;
}
