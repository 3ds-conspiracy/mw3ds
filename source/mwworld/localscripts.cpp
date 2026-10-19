// Scripts on references
#include "mwworld/worldimp.hpp"
#include "mwrender/objectpaging.hpp"
#include "components/files/datapath.hpp"

#include <3ds.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "extern/cjson/cJSON.h"
#include "platform/linear.hpp"
#include "platform/console.hpp"
extern "C" u32 __ctru_heap_size;
#include "platform/memstat.hpp"
#include <malloc.h>
#include "components/debug/debuglog.hpp"
#include "components/files/zfile.hpp"
#include "mwworld/worldhelpers.hpp"

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
