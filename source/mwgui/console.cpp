#include "mwgui/console.hpp"

#include <cstdio>

#include "extern/cjson/cJSON.h"
#include "mwworld/esmstore.hpp"
#include "components/debug/debuglog.hpp"
#include "components/interpreter/interpreter.hpp"
#include "components/compiler/compiler.hpp"
#include "mwworld/worldimp.hpp"
#include <cmath>
#include <cstring>
#include "mwsound/soundmanagerimp.hpp"
#include "platform/gamma.hpp"
#include "platform/linear.hpp"
#include "mwrender/renderingmanager.hpp"
#include "mwgui/windowmanagerimp.hpp"
#include "mwbase/session.hpp"
#include <algorithm>
#include <array>
#include <functional>
#include <sys/stat.h>
#include <tex3ds.h>
#include <citro2d.h>
#include "mwrender/objectpaging.hpp"
#include "mwrender/animation.hpp"
#include <cstdlib>
#include "platform/memstat.hpp"
#include <malloc.h>
#include "mwbase/sessionhelpers.hpp"
#include "mwgui/windowhelpers.hpp"

void DevConsole::print(const std::string& text, bool error)
{
	size_t start = 0;
	while (start <= text.size())
	{
		size_t nl = text.find('\n', start);
		std::string one = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
		logf("console: %s%s", error ? "(error) " : "", one.c_str());
		lines.push_back({ one, error });
		if (nl == std::string::npos)
			break;
		start = nl + 1;
	}
	if ((int)lines.size() > kMaxLines)
	{
		size_t drop = lines.size() - kMaxLines;
		lines.erase(lines.begin(), lines.begin() + drop);
		runStart = runStart > drop ? runStart - drop : 0;
	}
}

std::string DevConsole::selectedName(const World& w) const
{
	if (selected < 0 || selected >= (int)w.refs.size())
		return "";
	return w.refs[selected].id.str();
}

// A function the interpreter does not run (OpenMW has it): named so the line is refused, not half run
static const char* unsupportedCall(const Node& n)
{
	if (n.kind == N_CALL && !scriptFunctionSupported(n.b.str()))
		return n.b.c_str();
	for (const Node& k : n.kids)
		if (const char* name = unsupportedCall(k))
			return name;
	return nullptr;
}

// A call without a reference to a function that needs one, while nothing is selected
static const char* implicitCall(const Node& n)
{
	if (n.kind == N_CALL && n.a.empty() && scriptNeedsRef(n.b.str()))
		return n.b.c_str();
	for (const Node& k : n.kids)
		if (const char* name = implicitCall(k))
			return name;
	return nullptr;
}

// OpenMW's Console::execute: the line is compiled with the selected object's script locals, then run in its context;
// an expression's value is reported as a whole number (%d) or a float (%f)
void DevConsole::run(World& w, ScriptHost& host, const std::string& line)
{
	history.push_back(line);
	runStart = lines.size();
	print("> " + line);
	lastError = false;
	hasValue = false;
	if (selected >= (int)w.refs.size())
		selected = -1;

	std::unordered_map<std::string, char> globals;
	for (auto& g : w.game.globals)
		globals[g.first] = g.second.type;
	ScriptCompileInput in;
	in.globals = &globals;
	in.engineKnows = scriptFunctionSupported;
	const Script* scope = nullptr;
	if (selected >= 0 && w.refs[selected].script >= 0 && w.refs[selected].script < (int)w.scripts.size())
	{
		scope = w.scripts[w.refs[selected].script].script;
		if (scope)
			for (size_t k = 0; k < scope->localNames.size(); k++)
			{
				char t = k < scope->localTypes.size() ? scope->localTypes[k] : 'f';
				in.locals.push_back({ t == 's' ? "short" : t == 'l' ? "long" : "float", scope->localNames[k] });
			}
	}

	char type = 0;
	std::string error;
	cJSON* tree = scriptCompileConsole(line, in, &type, &error);
	if (!tree)
	{
		print("error: " + error, true);
		lastError = true;
		return;
	}
	Node node;
	if (type)
		scriptExprFromJson(tree, node, scope);
	else
		scriptBlockFromJson(tree, node, scope);
	cJSON_Delete(tree);
	if (const char* name = unsupportedCall(node))
	{
		print(std::string("error: not supported here: ") + name, true);
		lastError = true;
		return;
	}
	// OpenMW's console with nothing selected: "no implicit reference" (Player->... or select an object)
	if (selected < 0 && implicitCall(node))
	{
		print(std::string("error: no implicit reference (") + implicitCall(node) + ": select an object, or write X->)", true);
		lastError = true;
		return;
	}
	// X.var alone: printed as that object's script declares the variable (OpenMW's compiler asks its locals)
	if (type && node.kind == N_REMOTE)
	{
		int ri = w.findRefAnywhere(node.a);
		const Script* sc = ri >= 0 && w.refs[ri].script >= 0 ? w.scripts[w.refs[ri].script].script : nullptr;
		int k = sc ? sc->localIndex(node.b) : -1;
		if (k >= 0 && k < (int)sc->localTypes.size())
			type = sc->localTypes[k] == 'f' ? 'f' : 'l';
	}
	if (type)
	{
		lastValue = scriptEval(w, host, node, selected);
		hasValue = true;
		char buf[64];
		if (type == 'l')
			snprintf(buf, sizeof(buf), "%d", (int)lastValue);
		else
			snprintf(buf, sizeof(buf), "%f", lastValue);
		print(buf);
	}
	else
		scriptRunSnippet(w, host, node, selected);
}

// Options > Console (OpenMW's console, devconsole.cpp): the object the lines act on, what they printed, and the system
// keyboard for the next line. D-pad / C-stick up and down scroll the output; Edit last opens the keyboard on the line
// typed last
void Session::drawConsole()
{
	header("Console");
	const UiInput& in = uiIn();
	if (console.selected >= (int)w.refs.size())
		console.selected = -1;
	std::string sel = console.selectedName(w);
	uiText(8, 24, 0.4f, col::textDim, sel.empty() ? "Object: none (write Player->... for yourself)" : "Object: " + sel);
	// the output, wrapped, newest at the bottom
	const float kTop = 40.0f, kHeight = 162.0f, kScale = 0.4f;
	uiRect(4, kTop, 312, kHeight, col::panel);
	uiFrame(4, kTop, 312, kHeight);
	struct Row { std::string text; bool error; };
	std::vector<Row> rows;
	for (auto& l : console.lines)
		for (auto& part : uiWrap(l.text.empty() ? " " : l.text, 300.0f, kScale))
			rows.push_back({ part, l.error });
	float lh = uiLineHeight(kScale);
	int fit = (int)((kHeight - 6.0f) / lh);
	int maxBack = std::max(0, (int)rows.size() - fit);
	if (in.down & KEY_UP) consoleBack += 3.0f;
	if (in.down & KEY_DOWN) consoleBack -= 3.0f;
	if (in.touching)
		consoleBack += in.dragDY / lh;
	consoleBack -= uiStickScroll() / lh;
	consoleBack = fmaxf(0.0f, fminf((float)maxBack, consoleBack));
	int last = (int)rows.size() - (int)consoleBack;
	int first = std::max(0, last - fit);
	for (int i = first; i < last; i++)
		uiText(8, kTop + 3 + (i - first) * lh, kScale, rows[i].error ? col::health : col::text, rows[i].text);
	if (rows.empty())
	{
		uiText(8, kTop + 3, kScale, col::textDim, "Tap Type, write a line: OK on the keyboard runs it.");
		uiText(8, kTop + 3 + lh, kScale, col::textDim, "Player->AddItem gold_001 100, coc Balmora, tgm, sv");
	}
	// Type, Edit last, Clear, Close (focus 0..3: D-pad left / right)
	if (in.down & KEY_RIGHT) focus = focus >= 3 ? 0 : focus + 1;
	else if (in.down & KEY_LEFT) focus = focus <= 0 ? 3 : focus - 1;
	int b = -1;
	const std::string row[4] = { "Type", "Edit last", "Clear", w.game.gmst("sclose", "Close") };
	for (int i = 0; i < 4; i++)
		if (uiButton(4 + i * 79, 208, 75, 28, row[i], focus == i, i != 1 || !console.history.empty())
			|| (focus == i && (in.down & KEY_A)))
			b = i;
	if (b == 0 || (b == 1 && !console.history.empty()))
	{
		consoleDraft = b == 1 ? console.history.back() : "";
		wantText = TEXT_CONSOLE;
	}
	else if (b == 2)
	{
		console.lines.clear();
		console.runStart = 0;
		consoleBack = 0.0f;
	}
	else if (b == 3 || (in.down & KEY_B))
	{
		openScreen(SCR_OPTIONS);
		focus = kPrefsConsoleFocus;
	}
}
