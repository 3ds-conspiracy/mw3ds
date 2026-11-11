#pragma once

// What mwbase/session.cpp and the activation code share
#include <3ds.h>
#include <algorithm>
#include <array>
#include <functional>
#include <sys/stat.h>
#include <tex3ds.h>
#include <citro2d.h>

#include <cmath>
#include <cstdlib>
#include <cstring>

#include <malloc.h>

#include "mwbase/session.hpp"

static const float kReach = 192.0f;          // iMaxActivateDist

// update's time by part (the profile line each second): from since to now into g_profParts[part]
static inline u64 profTick()
{
	return svcGetSystemTick();
}

static inline void profAdd(int part, u64& since)
{
	u64 now = svcGetSystemTick();
	g_profParts[part] += (now - since) * 1000.0f / SYSCLOCK_ARM11;
	since = now;
}

// ... into PROF_OTHER and the part of it this was (PROF_O_*)
static inline void profAddOther(int part, u64& since)
{
	u64 now = svcGetSystemTick();
	float ms = (now - since) * 1000.0f / SYSCLOCK_ARM11;
	g_profParts[PROF_OTHER] += ms;
	g_profParts[part] += ms;
	since = now;
}
