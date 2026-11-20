// EXPECT values: gyroscope and sticks
#include <optional>
#include <3ds.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <queue>
#include <tuple>
#include "testdrive/testdrive.hpp"
#include "mwbase/session.hpp"
#include "components/debug/debuglog.hpp"
#include "platform/circlepadpro.hpp"
#include "mwinput/inputmanagerimp.hpp"

extern const char* g_playerBlock;
#include <malloc.h>
#include "testdrive/testhelpers.hpp"

std::optional<bool> numberOfInput(Session& s, const std::string& what, const std::string& arg, float& v)
{
	if (what == "gyroyaw" || what == "gyropitch")
	{
		// gyroyaw / gyropitch:<gx>,<gy>,<gz>,<gravity x>,<y>,<z>: player-space gyro aim's turn of the view (+ right / up)
		float g[3] = {}, gv[3] = {};
		sscanf(arg.c_str(), "%f,%f,%f,%f,%f,%f", &g[0], &g[1], &g[2], &gv[0], &gv[1], &gv[2]);
		float yaw, pitch;
		gyroPlayerSpace(g, gv, yaw, pitch);
		v = what == "gyroyaw" ? yaw : pitch;
		return true;
	}
	if (what == "stick")               // stick:<dx>,<dy>: a stick's counts through the dead zone: the push (the x and y
	{                                  // together, 0..1)
		float dx, dy, x, y;
		twoNumbers(arg, dx, dy);
		stickAxes(dx, dy, stickZone(), x, y);
		v = sqrtf(x * x + y * y);
		return true;
	}
	// An Old 3DS's Circle Pad Pro: cppstate (0 off, 1 looking, 2 connecting, 3 connected, 4 not found), cppconnected,
	// cppbattery (0 to 31), cppused 1 once the Classic layout reads the full pad, cppsetting the Pad Pro switch;
	// cppkey:zl|zr|r 1 while held on it; cstickx / csticky the second stick through the dead zone (-1 to 1)
	if (what == "cppstate" || what == "cppconnected" || what == "cppbattery" || what == "cppused" || what == "cppsetting")
	{
		v = what == "cppstate" ? (float)cppState() : what == "cppconnected" ? (cppConnected() ? 1.0f : 0.0f)
			: what == "cppbattery" ? (float)cppBattery() : what == "cppused" ? (classicProSeen() ? 1.0f : 0.0f)
			: (g_circlePadPro ? 1.0f : 0.0f);
		return true;
	}
	if (what == "cppkey")
	{
		u32 k = arg == "zl" ? KEY_ZL : arg == "zr" ? KEY_ZR : arg == "r" ? KEY_R : 0;
		v = (cppKeysHeld() & k) ? 1.0f : 0.0f;
		return true;
	}
	if (what == "cstickx" || what == "csticky")
	{
		circlePosition cs;
		cstickRead(&cs);
		float x, y;
		stickAxes(cs.dx, cs.dy, stickZone(), x, y);
		v = what == "cstickx" ? x : y;
		return true;
	}
	if (what == "gyrolevel")
	{
		// gyrolevel:<offset>,<jitter>,<seconds>[,<aim>[,<yaw>]]: a console held at a 2DS's angle in moving hands (a hand's
		// <jitter> on every axis: never still), its sensor resting at <offset> a second on the up / down axis (x), tilted
		// up at <aim> a second (gravity turning with it) and turned about gravity at <yaw>, for that long at 30 frames a
		// second: the up / down turn the view still gets, less the aim, over the last second (degrees a second)
		float n[5] = {};
		sscanf(arg.c_str(), "%f,%f,%f,%f,%f", &n[0], &n[1], &n[2], &n[3], &n[4]);
		GyroRest rest;
		float tilt = 0.45f, sum = 0.0f;           // radians the console leans back (gravity's y and z)
		int frames = (int)(n[2] * 30.0f), counted = 0;
		for (int f = 0; f < frames; f++)
		{
			float j = n[1] * (sinf(f * 0.9f) * 0.6f + sinf(f * 1.7f + 1.0f) * 0.4f);
			tilt += n[3] / 57.2958f / 30.0f;
			float g[3] = { -30.0f, -512.0f * cosf(tilt), -512.0f * sinf(tilt) };
			// (a turn about gravity shows on y and z as gravity lies in the console's frame)
			float rate[3] = { n[0] + n[3] + j, n[4] * cosf(tilt) + j * 0.8f, n[4] * sinf(tilt) - j * 0.6f };
			rest.gravity(g, 1.0f / 30.0f);
			rest.update(rate, 1.0f / 30.0f);
			if (f >= frames - 30)
			{
				sum += rate[0] - n[3];
				counted++;
			}
		}
		v = counted ? fabsf(sum / counted) : 0.0f;
		return true;
	}
	if (what == "gyrodrift" || what == "gyrocal")
	{
		// gyrodrift:<offset>,<jitter>,<seconds>[,<other>]: a console held still whose sensor rests at <offset> degrees a
		// second on its turning axis (and <other> on another), with a hand's <jitter>, for that long at 30 frames a
		// second: how fast the view still turns after (degrees a second). gyrocal: the same after Calibrate, the console
		// laid down while it measures (gyrocal:...,<jitter> over 2: held in a hand meanwhile, that much)
		float n[4] = {};
		sscanf(arg.c_str(), "%f,%f,%f,%f", &n[0], &n[1], &n[2], &n[3]);
		GyroRest rest;
		if (what == "gyrocal")
		{
			rest.calibrate();
			for (int f = 0; f < (int)((GyroRest::kCalibrate + 0.25f) * 30.0f) + 2; f++)
			{
				float j = n[1] > 2.0f ? n[1] * (sinf(f * 0.9f) * 0.6f + sinf(f * 0.23f + 1.0f) * 0.4f) : 0.0f;
				float rate[3] = { j * 0.5f, n[0] + j, n[3] + j * 0.3f };
				rest.update(rate, 1.0f / 30.0f);
			}
		}
		float sum = 0.0f;
		int frames = (int)(n[2] * 30.0f), counted = 0;
		for (int f = 0; f < frames; f++)
		{
			// (a hand's jitter: a few slow wobbles, the same every run)
			float j = n[1] * (sinf(f * 0.9f) * 0.6f + sinf(f * 0.23f + 1.0f) * 0.4f);
			float rate[3] = { j * 0.5f, n[0] + j, n[3] + j * 0.3f };
			rest.update(rate, 1.0f / 30.0f);
			if (f >= frames - 30)                       // (the last second's average: the wobble left out)
			{
				sum += rate[1];
				counted++;
			}
		}
		v = counted ? fabsf(sum / counted) : 0.0f;
		return true;
	}
	if (what == "gyrosoft")            // gyrosoft:<rate>,<soft>: gyro aim's soft threshold applied to a turn
	{
		float rate, soft;
		twoNumbers(arg, rate, soft);
		v = gyroSoften(rate, soft);
		return true;
	}
	if (what == "gyrodps")             // gyrodps:<raw>,<coefficient>: the gyroscope's counts as degrees a second
	{
		float raw, coeff;
		twoNumbers(arg, raw, coeff);
		v = gyroRawToDps(raw, coeff);
		return true;
	}
	return std::nullopt;
}
