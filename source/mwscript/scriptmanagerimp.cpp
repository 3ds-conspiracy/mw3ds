// Compiled scripts (game.json) into the interpreter's trees
#include "mwworld/esmstore.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>

#include "extern/cjson/cJSON.h"
#include "components/debug/debuglog.hpp"
#include "components/files/zfile.hpp"
#include "components/interpreter/interpreter.hpp"
#include "mwworld/esmhelpers.hpp"

static void convertExpr(const cJSON* j, Node& n, const Script* scope);

static OpKind opKind(const char* s)
{
	static const char* names[] = { "+", "-", "*", "/", "==", "!=", "<", "<=", ">", ">=", "//" };
	for (int i = 0; i < 11; i++)
		if (strcmp(s, names[i]) == 0)
			return (OpKind)i;
	return OP_EQ;
}

static void convertCall(const cJSON* j, Node& n, const Script* scope)
{
	n.kind = N_CALL;
	const cJSON* ref = cJSON_GetArrayItem(j, 1);
	n.a = cJSON_IsString(ref) ? ref->valuestring : "";
	n.b = cJSON_GetArrayItem(j, 2)->valuestring;
	n.func = scriptFunctionId(n.b);
	const cJSON* args = cJSON_GetArrayItem(j, 3);
	const cJSON* a;
	n.kids.reserve(cJSON_GetArraySize(args));       // (the trees' vectors sized exactly: doubling wasted a third)
	cJSON_ArrayForEach(a, args)
	{
		n.kids.emplace_back();
		convertExpr(a, n.kids.back(), scope);
	}
}

static void convertExpr(const cJSON* j, Node& n, const Script* scope)
{
	if (cJSON_IsNumber(j))
	{
		n.kind = N_NUM;
		n.num = (float)j->valuedouble;
		return;
	}
	if (!cJSON_IsArray(j) || cJSON_GetArraySize(j) == 0)
	{
		n.kind = N_NUM;
		return;
	}
	const char* tag = cJSON_GetArrayItem(j, 0)->valuestring;
	if (strcmp(tag, "s") == 0)
	{
		n.kind = N_STR;
		n.a = cJSON_GetArrayItem(j, 1)->valuestring;
	}
	else if (strcmp(tag, "l") == 0)
	{
		n.kind = N_LOCAL;
		n.a = cJSON_GetArrayItem(j, 1)->valuestring;
		n.func = scope ? scope->localIndex(n.a) : -1;
	}
	else if (strcmp(tag, "g") == 0)
	{
		n.kind = N_GLOBAL;
		n.a = cJSON_GetArrayItem(j, 1)->valuestring;
	}
	else if (strcmp(tag, "r") == 0)
	{
		n.kind = N_REMOTE;
		n.a = cJSON_GetArrayItem(j, 1)->valuestring;
		n.b = cJSON_GetArrayItem(j, 2)->valuestring;
	}
	else if (strcmp(tag, "c") == 0)
		convertCall(j, n, scope);
	else if (strcmp(tag, "neg") == 0)
	{
		n.kind = N_NEG;
		n.kids.emplace_back();
		convertExpr(cJSON_GetArrayItem(j, 1), n.kids.back(), scope);
	}
	else
	{
		n.kind = N_OP;
		n.op = opKind(tag);
		n.kids.resize(2);
		convertExpr(cJSON_GetArrayItem(j, 1), n.kids[0], scope);
		convertExpr(cJSON_GetArrayItem(j, 2), n.kids[1], scope);
	}
}

void convertStatement(const cJSON* j, Node& n, const Script* scope)
{
	const char* tag = cJSON_GetArrayItem(j, 0)->valuestring;
	if (strcmp(tag, "set") == 0)
	{
		n.kind = N_SET;
		n.kids.resize(2);
		convertExpr(cJSON_GetArrayItem(j, 1), n.kids[0], scope);
		convertExpr(cJSON_GetArrayItem(j, 2), n.kids[1], scope);
	}
	else if (strcmp(tag, "if") == 0)
	{
		n.kind = N_IF;
		const cJSON* branch;
		n.kids.reserve(cJSON_GetArraySize(cJSON_GetArrayItem(j, 1)) * 2 + 1);
		cJSON_ArrayForEach(branch, cJSON_GetArrayItem(j, 1))
		{
			n.kids.emplace_back();
			convertExpr(cJSON_GetArrayItem(branch, 0), n.kids.back(), scope);
			n.kids.emplace_back();
			convertBlock(cJSON_GetArrayItem(branch, 1), n.kids.back(), scope);
		}
		const cJSON* elseBody = cJSON_GetArrayItem(j, 2);
		if (cJSON_IsArray(elseBody))
		{
			n.kids.emplace_back();
			convertBlock(elseBody, n.kids.back(), scope);
		}
	}
	else if (strcmp(tag, "while") == 0)
	{
		n.kind = N_WHILE;
		n.kids.resize(2);
		convertExpr(cJSON_GetArrayItem(j, 1), n.kids[0], scope);
		convertBlock(cJSON_GetArrayItem(j, 2), n.kids[1], scope);
	}
	else if (strcmp(tag, "ret") == 0)
		n.kind = N_RET;
	else if (strcmp(tag, "c") == 0)
		convertCall(j, n, scope);
}

// The device's own compiler (components/compiler/compiler.cpp, the console) builds the same JSON as the converter: the same conversion
void scriptBlockFromJson(const cJSON* list, Node& n, const Script* scope)
{
	convertBlock(list, n, scope);
}

void scriptExprFromJson(const cJSON* j, Node& n, const Script* scope)
{
	convertExpr(j, n, scope);
}
