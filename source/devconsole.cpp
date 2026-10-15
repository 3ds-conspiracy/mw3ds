#include "devconsole.h"

#include <cstdio>

#include "cJSON.h"
#include "game.h"
#include "log.h"
#include "script.h"
#include "scriptc.h"
#include "world.h"

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
