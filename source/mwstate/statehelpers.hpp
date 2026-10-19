#pragma once

// What the save files share: the JSON helpers
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <vector>
#include <zlib.h>
#include <algorithm>
#include <array>
#include <functional>
#include <sys/stat.h>
#include <tex3ds.h>
#include <citro2d.h>
#include <cmath>
#include <malloc.h>

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
