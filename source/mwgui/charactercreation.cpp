// Character creation: the name
#include <algorithm>
#include <array>
#include <functional>
#include <sys/stat.h>
#include <tex3ds.h>
#include <citro2d.h>
#include "mwbase/session.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "components/debug/debuglog.hpp"
#include <malloc.h>

void Session::setName(const std::string& name)
{
	w.stats.name = name.empty() ? "Stranger" : name;
	wantName = false;
	logf("chargen: name %s", w.stats.name.c_str());
}
