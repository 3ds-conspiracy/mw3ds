#pragma once

#include <functional>
#include <string>

// Development builds update themselves at launch (source/devupdate.cpp). A dev CIA
// (tools/make-dev.ps1) carries romfs:/devhost.txt ("ip port" of the PC's tools/serve-cia.js dev
// port) and romfs:/buildid.txt. At launch it:
//   1. brings sdmc:/3ds/mw3ds/data in line with the PC's out/data (only changed files, resumed
//      and retried through Wi-Fi stalls; sdmc:/3ds/mw3ds/data/.manifest lists what is complete),
//   2. installs a newer code CIA from the PC through AM and relaunches into it.
// Unreachable PC: plays what it has.
enum DevUpdateResult { DEV_NONE, DEV_UPDATED, DEV_RELAUNCH, DEV_QUIT };   // QUIT: closed from the HOME menu

// progress(text, detail) redraws the loading screen
// Deletes old data moved aside by an update, slowly, on a background thread
void devEmptyTrash();

DevUpdateResult devUpdate(const std::function<void(const std::string&, const std::string&)>& progress);
