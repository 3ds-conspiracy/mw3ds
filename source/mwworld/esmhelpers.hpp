#pragma once

// What the game data loader and the script tree share

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>


void convertStatement(const cJSON* j, Node& n, const Script* scope);

static inline double num(const cJSON* obj, const char* key, double fallback = 0.0)
{
	const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
	return cJSON_IsNumber(v) ? v->valuedouble : fallback;
}

static inline void convertBlock(const cJSON* list, Node& n, const Script* scope)
{
	n.kind = N_BLOCK;
	const cJSON* s;
	n.kids.reserve(cJSON_GetArraySize(list));
	cJSON_ArrayForEach(s, list)
	{
		n.kids.emplace_back();
		convertStatement(s, n.kids.back(), scope);
	}
}
