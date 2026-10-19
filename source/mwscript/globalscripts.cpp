// Global scripts: started and stopped
#include "mwworld/worldimp.hpp"

#include <3ds.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" u32 __ctru_heap_size;
#include <malloc.h>
#include "components/debug/debuglog.hpp"

int World::startGlobalScript(const std::string& name, int target)
{
	std::string l = lower(name);
	for (size_t i = 0; i < scripts.size(); i++)
		if (scripts[i].ref < 0 && scripts[i].item.empty() && lower(scripts[i].script->name) == l)
		{
			// (one that is running already keeps its reference, as in OpenMW)
			if (!scripts[i].running)
				scripts[i].target = target;
			scripts[i].running = true;
			return i;
		}
	const Script* s = game.script(l);
	if (!s)
	{
		logf("script: StartScript %s: not found", name.c_str());
		monitorOnce(("startscript:" + name).c_str(), "StartScript %s: no such script", name.c_str());
		return -1;
	}
	ScriptInstance inst;
	inst.script = s;
	inst.locals.assign(s->localNames.size(), 0.0f);
	inst.target = target;
	scripts.push_back(std::move(inst));
	return scripts.size() - 1;
}

void World::stopGlobalScript(const std::string& name)
{
	std::string l = lower(name);
	for (auto& s : scripts)
		if (s.ref < 0 && s.item.empty() && lower(s.script->name) == l)
			s.running = false;
}
