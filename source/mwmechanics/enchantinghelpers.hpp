#pragma once

// What enchanting and its screens share
#include <cmath>
#include <functional>

#include "components/debug/debuglog.hpp"
#include "mwgui/windowmanagerimp.hpp"
#include "mwbase/session.hpp"
#include "mwgui/widgets.hpp"

extern const char* kCastNames[4];
int castTypesFor(const Object* o);
