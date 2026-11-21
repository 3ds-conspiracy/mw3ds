#pragma once

#include <string>

struct World;
struct Session;

// A bug report the player writes in-game: one entry appended to sdmc:/3ds/mw3ds/reports.json, with a
// screenshot (report_NN.bmp) and a copy of the game (report_NN_save.json, the save format) beside it, so a
// report says where to look and can be replayed exactly. An entry holds what a report needs to find the place
// again without the player typing coordinates: the world position, the cell and its grid square, the region,
// the in-game date, the build and the console, and the "monitor:" warnings the engine logged this run
// (missing textures and the like). Reports go to the folder the log and crash notes already use, and every
// report gets its own number, so nothing is ever written over an older one.
void bugReportPrompt(Session& session);   // asks the system keyboard what went wrong (the game menu's button)
int bugReportCommand(World& w, Session& session, const std::string& line);
                                        // the console's "report [what went wrong]" line, tried before the
                                        // line is compiled; 0 when the line is something else
// Files an entry with the description in hand (the keyboard's answer, the console's line). The screenshot is
// asked for here and taken at the next C3D_FrameBegin (engine.cpp), the frame last drawn being the place as
// the player saw it. wantShot false: text only (the autotest draws nothing worth keeping). Returns the
// report's number, or 0 if the card refused the write.
int bugReportFile(World& w, Session& session, const std::string& description, bool wantShot = true);
