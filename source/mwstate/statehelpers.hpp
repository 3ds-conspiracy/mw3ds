#pragma once

// What the save files share: the JSON helpers
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "extern/cjson/cJSON.h"
#include "components/debug/debuglog.hpp"
#include "mwgui/savegamedialog.hpp"
#include "components/files/zfile.hpp"
#include <vector>
#include <zlib.h>
#include "mwworld/worldimp.hpp"
#include <algorithm>
#include <array>
#include <functional>
#include <sys/stat.h>
#include <tex3ds.h>
#include <citro2d.h>
#include "mwrender/objectpaging.hpp"
#include "mwrender/animation.hpp"
#include "mwbase/session.hpp"
#include <cmath>
#include "mwsound/soundmanagerimp.hpp"
#include "platform/gamma.hpp"
#include "platform/linear.hpp"
#include "platform/memstat.hpp"
#include <malloc.h>
#include "mwbase/sessionhelpers.hpp"

bool packText(const std::string& text, std::vector<u8>& out);
bool unpackText(const u8* in, size_t inLen, u32 raw, std::string& out);

static inline cJSON* floats(const float* v, int n)
{
	return cJSON_CreateFloatArray(v, n);
}

static inline void readFloats(const cJSON* a, float* out, int n)
{
	for (int i = 0; i < n && i < cJSON_GetArraySize(a); i++)
		out[i] = (float)cJSON_GetArrayItem(a, i)->valuedouble;
}

static inline std::string str(const cJSON* o, const char* key)
{
	const cJSON* v = cJSON_GetObjectItem(o, key);
	return cJSON_IsString(v) ? v->valuestring : "";
}

static inline double num(const cJSON* o, const char* key, double fallback = 0.0)
{
	const cJSON* v = cJSON_GetObjectItem(o, key);
	return cJSON_IsNumber(v) ? v->valuedouble : fallback;
}
