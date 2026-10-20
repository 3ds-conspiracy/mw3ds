#pragma once

// Compiled scripts: the syntax tree the interpreter runs (OpenMW's ESM::Script)
#include "components/misc/atom.hpp"
#include <vector>

// ---- Script syntax tree (built by tools/convert/mwscript.py, see there for the JSON shapes)

enum NodeKind : unsigned char
{
	N_NUM, N_STR, N_LOCAL, N_GLOBAL, N_REMOTE, N_CALL, N_OP, N_NEG,
	N_SET, N_IF, N_WHILE, N_RET, N_BLOCK,
};

enum OpKind : unsigned char { OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE, OP_IDIV };

struct Node
{
	NodeKind kind = N_BLOCK;
	unsigned char op = 0;
	int func = 0;              // N_CALL: function id (components/interpreter/interpreter.cpp); N_LOCAL: slot or -1 = look up by name
	float num = 0.0f;
	Atom a, b;                 // N_STR text; N_LOCAL/N_GLOBAL name; N_REMOTE ref + var; N_CALL ref + name
	std::vector<Node> kids;    // N_CALL args; N_OP/N_NEG operands; N_SET target, value;
	                           // N_IF cond, block, cond, block, ..., [else block]; N_WHILE cond, block
};

struct cJSON;
struct Script;
// Script trees from JSON (mwscript.py's shapes): a block, or one expression; scope gives the locals' slots
void scriptBlockFromJson(const cJSON* list, Node& n, const Script* scope);
void scriptExprFromJson(const cJSON* j, Node& n, const Script* scope);

struct Script
{
	std::string name;
	std::vector<std::string> localNames;
	std::vector<char> localTypes;        // 's', 'l', 'f'
	Node body;
	bool usesOnActivate = false;         // activation is handed to the script instead of the default action
	bool scriptedHits = false;           // HitOnMe / SetHealth: the script runs the fight (the Heart of Lorkhan)
	int localIndex(const std::string& lowerName) const;
};
