#pragma once

#include <string>
#include <vector>

struct World;
struct ScriptHost;

// The in-game console (OpenMW's, mwgui/console.cpp): one MWScript line at a time, compiled on the device
// (scriptc.cpp) and run by the interpreter on the selected object, with that object's script locals.
// (include/console.h is something else: the 3DS model's profile.)
struct DevConsole
{
	struct Line
	{
		std::string text;
		bool error;
	};
	static const int kMaxLines = 200;

	std::vector<Line> lines;             // what it printed, oldest first
	std::vector<std::string> history;    // the lines typed, oldest first
	int selected = -1;                   // the reference the lines act on: -1 none (then a call that needs one is refused)
	bool lastError = false;              // the last line did not run (it did not compile, or names what we do not run)
	bool hasValue = false;               // the last line was an expression: its value
	float lastValue = 0.0f;
	size_t runStart = 0;                 // lines[runStart..]: what the last line printed (its echo first)

	void run(World& w, ScriptHost& host, const std::string& line);
	void print(const std::string& text, bool error = false);
	std::string selectedName(const World& w) const;      // the selected object's ID, "" for none
};
