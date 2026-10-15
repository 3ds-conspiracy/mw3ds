// Compiles every case of tools/test/script_compile_check.py's cases.json with the device's compiler
// (source/scriptc.cpp) and prints the trees as a JSON array: {"ok": tree} or {"error": message} per case.
//     scriptc-check.exe cases.json
#include <cstdio>
#include <string>
#include <unordered_map>

#include "cJSON.h"
#include "scriptc.h"

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		fprintf(stderr, "usage: scriptc-check cases.json\n");
		return 2;
	}
	FILE* f = fopen(argv[1], "rb");
	if (!f)
	{
		fprintf(stderr, "cannot open %s\n", argv[1]);
		return 2;
	}
	std::string text;
	char buf[65536];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		text.append(buf, n);
	fclose(f);
	cJSON* root = cJSON_Parse(text.c_str());
	if (!root)
	{
		fprintf(stderr, "bad JSON\n");
		return 2;
	}
	std::unordered_map<std::string, char> globals;
	const cJSON* g;
	cJSON_ArrayForEach(g, cJSON_GetObjectItem(root, "globals"))
		globals[g->string] = cJSON_IsString(g) && g->valuestring[0] ? g->valuestring[0] : 'f';
	cJSON* out = cJSON_CreateArray();
	const cJSON* c;
	cJSON_ArrayForEach(c, cJSON_GetObjectItem(root, "cases"))
	{
		ScriptCompileInput in;
		in.globals = &globals;
		const cJSON* l;
		cJSON_ArrayForEach(l, cJSON_GetObjectItem(c, "locals"))
			in.locals.push_back({ "unknown", l->valuestring });
		std::string src = cJSON_GetObjectItem(c, "text")->valuestring;
		bool script = std::string(cJSON_GetObjectItem(c, "kind")->valuestring) == "script";
		std::string error;
		cJSON* tree = script ? scriptCompile(src, in, &error) : scriptCompileSnippet(src, in, &error);
		cJSON* item = cJSON_CreateObject();
		if (tree)
			cJSON_AddItemToObject(item, "ok", tree);
		else
			cJSON_AddStringToObject(item, "error", error.c_str());
		cJSON_AddItemToArray(out, item);
	}
	char* s = cJSON_PrintUnformatted(out);
	fputs(s, stdout);
	return 0;
}
