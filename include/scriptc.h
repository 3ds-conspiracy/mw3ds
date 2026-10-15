#pragma once

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct cJSON;

// The MWScript compiler on the device (the console's), a port of tools/convert/mwscript.py: the same JSON tree out,
// which game.cpp turns into Nodes as it does the converter's. tools/test/script_compile_check.py compiles every
// script and dialogue result of the game with both and compares them.
struct ScriptCompileInput
{
	const std::unordered_map<std::string, char>* globals = nullptr;   // lowercase name: 's' 'l' 'f'
	// The script's locals in order: type ("short" "long" "float", or "unknown" for a speaker's variables), lowercase name
	std::vector<std::pair<std::string, std::string>> locals;
	// The console: names the engine runs that the tables lack (OpenMW registers some in loops, the player-control
	// switches): a "get..." one is read as a function that returns a value, any other as an instruction
	bool (*engineKnows)(const std::string& lowerName) = nullptr;
};

// A whole script: {"name", "locals": [[type, name]], "body": [...]}. nullptr and *error when it does not compile.
cJSON* scriptCompile(const std::string& text, const ScriptCompileInput& in, std::string* error);

// A dialogue result script (no begin / end): the body, a JSON array
cJSON* scriptCompileSnippet(const std::string& text, const ScriptCompileInput& in, std::string* error);

// One console line, as OpenMW's console reads it (its LineParser with expressions allowed): a line that is an
// expression (a number, a variable, X.var, a function that returns a value) is evaluated and its value printed.
// Returns the JSON expression and sets *valueType to 'l' (printed as a whole number) or 'f'; or returns the body (a
// JSON array of one statement) and sets *valueType to 0. nullptr and *error when the line does not compile.
cJSON* scriptCompileConsole(const std::string& line, const ScriptCompileInput& in, char* valueType, std::string* error);

// Whether a name is a function or instruction the compiler knows (OpenMW's, mwscript.py's tables)
bool scriptKnownName(const std::string& lowerName);

// Whether a known function, called without a reference ("X->"), needs an object in context: OpenMW's console with
// nothing selected refuses such a line ("no implicit reference")
bool scriptNeedsRef(const std::string& lowerName);
