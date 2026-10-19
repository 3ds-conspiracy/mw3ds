#pragma once

#include <3ds.h>
#include <citro3d.h>

#include "mwbase/session.hpp"
#include "mwgui/widgets.hpp"
#include "testdrive/autoinput.hpp"
#include "testdrive/testdrive.hpp"

// One frame of a scripted run (autoinput.txt), from appMain's frame loop: every argument is the loop's own
void runScriptedStep(Session* session, ScriptedSteps& autoinput, size_t& inputIndex, float& inputTime, float& dt, PlayerInput& in, u32& down, u32& held, UiInput& ui, bool& touching, bool& autoShotNow, TestDriver& driver, bool& chainMode, bool& stepBlocking, bool& stepShot);
