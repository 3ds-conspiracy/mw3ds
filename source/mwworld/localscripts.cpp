// Scripts on references
#include "mwworld/worldimp.hpp"

#include <3ds.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" u32 __ctru_heap_size;
#include <malloc.h>

void World::startScriptFor(int ri)
{
	Ref& ref = refs[ri];
	if (!ref.obj || ref.obj->script.empty() || ref.script >= 0)
		return;
	const Script* s = game.script(ref.obj->script);
	if (!s)
		return;
	ScriptInstance inst;
	inst.script = s;
	inst.locals.assign(s->localNames.size(), 0.0f);
	inst.ref = ri;
	ref.script = scripts.size();
	scripts.push_back(std::move(inst));
}
