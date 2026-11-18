#pragma once

// What the inventory and magic screens share
#include <cmath>
#include <cstdio>
#include <cstring>

extern const char* kQuickHint;

// The button that casts, and how a quick key is pressed, in the active layout
static inline std::string castHint() { return g_controlLayout != CONTROLS_CLASSIC ? "L to cast" : "Y, then R to cast"; }
