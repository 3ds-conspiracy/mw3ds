#pragma once

// Compiled scripts: the syntax tree the interpreter runs (OpenMW's ESM::Script)
#include "components/misc/atom.hpp"
#include <cstddef>
#include <new>
#include <vector>

// ---- Script syntax tree (built by tools/convert/mwscript.py, see there for the JSON shapes)

enum NodeKind : unsigned char
{
	N_NUM, N_STR, N_LOCAL, N_GLOBAL, N_REMOTE, N_CALL, N_OP, N_NEG,
	N_SET, N_IF, N_WHILE, N_RET, N_BLOCK,
};

enum OpKind : unsigned char { OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE, OP_IDIV };

// A tree read back from the card (mwscript/scriptstore.cpp) is one block: while one is read its nodes' child lists come
// out of it (treePoolTake), and freeing them gives nothing back but the whole block at once. Trees read and let go of
// every few seconds as hundreds of small blocks each cut an Old 3DS's heap into holes no cell fitted (Skaal Village's
// neighbours never came, the biggest free block 1.1 MB with 17 MB free). Anything else: the heap, as ever
void* treePoolTake(size_t bytes);           // nullptr: no tree being read, or it is full
bool treePoolOwns(const void* p);
template <class T> struct TreeAlloc
{
	typedef T value_type;
	TreeAlloc() = default;
	template <class U> TreeAlloc(const TreeAlloc<U>&) {}
	T* allocate(size_t n)
	{
		if (void* p = treePoolTake(n * sizeof(T)))
			return (T*)p;
		return (T*)::operator new(n * sizeof(T));
	}
	void deallocate(T* p, size_t)
	{
		if (!treePoolOwns(p))
			::operator delete(p);
	}
	template <class U> bool operator==(const TreeAlloc<U>&) const { return true; }
	template <class U> bool operator!=(const TreeAlloc<U>&) const { return false; }
};

struct Node
{
	NodeKind kind = N_BLOCK;
	unsigned char op = 0;
	int func = 0;              // N_CALL: function id (components/interpreter/interpreter.cpp); N_LOCAL: slot or -1 = look up by name
	float num = 0.0f;
	Atom a, b;                 // N_STR text; N_LOCAL/N_GLOBAL name; N_REMOTE ref + var; N_CALL ref + name
	std::vector<Node, TreeAlloc<Node>> kids;   // N_CALL args; N_OP/N_NEG operands; N_SET target, value;
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
	// Its tree: on the card between runs of it (mwscript/scriptstore.cpp: scriptBody reads it back when it runs, and it
	// goes again once it hasn't run for a while)
	mutable Node body;
	bool spilled = false;
	mutable bool loaded = false;
	mutable unsigned lastRun = 0;
	mutable void* pool = nullptr;        // the block its read-back tree lives in
	unsigned spillOff = 0, spillLen = 0, spillCheck = 0;
	bool usesOnActivate = false;         // activation is handed to the script instead of the default action
	bool scriptedHits = false;           // HitOnMe / SetHealth: the script runs the fight (the Heart of Lorkhan)
	int localIndex(const std::string& lowerName) const;
};
