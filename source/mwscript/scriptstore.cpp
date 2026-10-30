// The scripts' trees kept on the card instead of the heap. Of 1,200 scripts (Bloodmoon's data) a few dozen run at any
// moment (the loaded cells' objects, the global ones started); as trees in the heap they all took about 2 MB of an Old
// 3DS's 39 MB. At the load each tree goes to the card (sdmc:/3ds/mw3ds/scripts.bin); scriptBody reads one back when the
// script runs, and one that hasn't run for ten seconds goes again (scriptsTick).
//
// The file is kept from one start to the next (components/misc/cardslices.hpp): a start with the same data writes
// nothing. A tree's texts are written out as letters and found again in the Atom pool when read (the parse interned
// every one of them, the message texts too). The script's name, locals and flags stay in the heap. Without a card (the file can't be written) the trees stay in the heap, as
// before. The main thread only.
#include <3ds.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "mwscript/scriptstore.hpp"
#include "mwworld/esmstore.hpp"
#include "components/debug/debuglog.hpp"
#include "components/misc/cardslices.hpp"
#include "platform/cellarena.hpp"

namespace
{
const char* const kPath = "sdmc:/3ds/mw3ds/scripts.bin";
CardSlices s_file(kPath, 2);         // (2: texts as letters; 1 wrote them as pool addresses)
const unsigned kIdle = 300;           // frames a tree stays after its script last ran (ten seconds at 30)

std::vector<const Script*> s_loaded;
// The block of the tree being read (treePoolTake hands out of it), and every read-back tree's block (treePoolOwns)
char* s_pool = nullptr;
size_t s_poolUsed = 0, s_poolSize = 0;
std::vector<std::pair<char*, size_t>> s_pools;
unsigned s_tick = 1;
FILE* s_read = nullptr;
char s_readBuf[8 * 1024];             // (static: a FILE's own buffer would come out of the heap)

void put(std::vector<u8>& b, const void* p, size_t n)
{
	b.insert(b.end(), (const u8*)p, (const u8*)p + n);
}

void putText(std::vector<u8>& b, const Atom& t)
{
	u32 n = (u32)t.size();
	put(b, &n, sizeof(n));
	put(b, t.c_str(), n);
}

void putNode(std::vector<u8>& b, const Node& n)
{
	b.push_back((u8)n.kind);
	b.push_back(n.op);
	put(b, &n.func, sizeof(n.func));
	put(b, &n.num, sizeof(n.num));
	putText(b, n.a);
	putText(b, n.b);
	u32 kids = (u32)n.kids.size();
	put(b, &kids, sizeof(kids));
	for (const Node& k : n.kids)
		putNode(b, k);
}

struct Reader
{
	const u8* p;
	const u8* end;
	bool ok = true;
	bool get(void* out, size_t n)
	{
		if ((size_t)(end - p) < n)
			return ok = false;
		memcpy(out, p, n);
		p += n;
		return true;
	}
	// A text written as its length and letters: the pool's copy (the parse interned it), or false past the end
	bool text(Atom* out)
	{
		u32 n = 0;
		if (!get(&n, sizeof(n)) || (size_t)(end - p) < n)
			return ok = false;
		if (out)
		{
			const char* s = Atom::existing((const char*)p, n);
			*out = s ? Atom::pooled(s) : Atom(std::string((const char*)p, n));
		}
		p += n;
		return true;
	}
};

// How many nodes a slice holds (each one's child list comes out of the tree's block)
bool countNodes(Reader& r, size_t& count)
{
	u8 head[2 + sizeof(int) + sizeof(float)];
	u32 kids = 0;
	if (!r.get(head, sizeof(head)) || !r.text(nullptr) || !r.text(nullptr) || !r.get(&kids, sizeof(kids)))
		return false;
	count++;
	for (u32 k = 0; k < kids; k++)
		if (!countNodes(r, count))
			return false;
	return true;
}

bool getNode(Reader& r, Node& n)
{
	u8 kind = 0;
	u32 kids = 0;
	if (!r.get(&kind, 1) || !r.get(&n.op, 1) || !r.get(&n.func, sizeof(n.func)) || !r.get(&n.num, sizeof(n.num))
		|| !r.text(&n.a) || !r.text(&n.b) || !r.get(&kids, sizeof(kids)))
		return false;
	n.kind = (NodeKind)kind;
	n.kids.resize(kids);                  // (sized exactly, as the parse sizes them)
	for (Node& k : n.kids)
		if (!getNode(r, k))
			return false;
	return true;
}

void freeTree(const Script& s)
{
	Node().kids.swap(s.body.kids);
	s.body = Node();
	s.loaded = false;
	if (s.pool)
	{
		for (size_t i = 0; i < s_pools.size(); i++)
			if (s_pools[i].first == s.pool)
			{
				s_pools[i] = s_pools.back();
				s_pools.pop_back();
				break;
			}
		if (!g_scriptArena.release(s.pool))
			free(s.pool);
		s.pool = nullptr;
	}
}
}

// The parse puts the trees on the card one by one as each is read (scriptSpillOne), letting each tree go at once: built
// all and let go at the end, the trees' small blocks left holes all through the game's data
static bool s_spilling = false;
static int s_spilled = 0;
static std::vector<u8> s_blob;

void scriptsSpillBegin()
{
	for (const Script* s : s_loaded)
		s->loaded = false;
	s_loaded.clear();
	if (s_read)
	{
		fclose(s_read);
		s_read = nullptr;
	}
	s_spilled = 0;
	s_spilling = s_file.begin();
	if (s_spilling)
		s_blob.reserve(64 * 1024);
	else
		logf("scripts: %s couldn't be written: the scripts' trees stay in the heap", kPath);
}

void scriptSpillOne(Script& s)
{
	if (!s_spilling)
		return;
	s_blob.clear();
	putNode(s_blob, s.body);
	u32 off = 0, check = 0;
	if (!s_file.put(s_blob.data(), (u32)s_blob.size(), off, check))
		return;                          // (this one and the rest stay in the heap)
	s.spillOff = off;
	s.spillCheck = check;
	s.spillLen = (u32)s_blob.size();
	freeTree(s);
	s.spilled = true;
	s_spilled++;
}

void scriptsSpillEnd()
{
	std::vector<u8>().swap(s_blob);
	if (!s_spilling)
		return;
	s_spilling = false;
	if (!s_file.end())
		monitorOnce("scriptwrite", "scripts: %s wasn't written whole", kPath);
	logf("scripts: %d trees on the card (%u KB, %u KB written)", s_spilled, (unsigned)(s_file.size() / 1024),
		(unsigned)(s_file.written() / 1024));
}

const Node& scriptBody(const Script& s)
{
	s.lastRun = s_tick;
	if (!s.spilled || s.loaded)
		return s.body;
	if (!s_read)
	{
		s_read = fopen(kPath, "rb");
		if (s_read)
			setvbuf(s_read, s_readBuf, _IOFBF, sizeof(s_readBuf));
	}
	std::vector<u8> slice(s.spillLen);
	if (!s_read || fseek(s_read, s.spillOff, SEEK_SET) != 0 || fread(slice.data(), 1, slice.size(), s_read) != slice.size())
	{
		monitorOnce("scriptread", "scripts: %s's tree couldn't be read from the card", s.name.c_str());
		return s.body;
	}
	// (not what was put there: the next start writes the file again)
	if (CardSlices::check(slice.data(), (u32)slice.size()) != s.spillCheck)
	{
		monitorOnce("scriptdamaged", "scripts: %s's tree on the card is damaged", s.name.c_str());
		fclose(s_read);
		s_read = nullptr;
		s_file.damaged();
		return s.body;
	}
	// one block for the whole tree: every node but the root sits in its parent's child list, each list 8-aligned
	size_t count = 0;
	Reader c{ slice.data(), slice.data() + slice.size() };
	if (countNodes(c, count) && count > 1)
	{
		s_poolSize = (count - 1) * sizeof(Node) + count * 8 + 16;
		s_pool = (char*)g_scriptArena.alloc(s_poolSize);       // (its own arena: platform/cellarena.hpp)
		if (!s_pool)
			s_pool = (char*)malloc(s_poolSize);
		s_poolUsed = 0;
	}
	Reader r{ slice.data(), slice.data() + slice.size() };
	bool ok = getNode(r, s.body);
	if (s_pool)
	{
		s.pool = s_pool;
		s_pools.push_back({ s_pool, s_poolSize });
		s_pool = nullptr;
	}
	if (!ok)
	{
		monitorOnce("scriptdamaged", "scripts: %s's tree on the card is damaged", s.name.c_str());
		freeTree(s);
		return s.body;
	}
	s.loaded = true;
	s_loaded.push_back(&s);
	return s.body;
}

void scriptsTick()
{
	s_tick++;
	if (s_tick % 30 != 0)
		return;
	for (size_t i = 0; i < s_loaded.size();)
		if (s_tick - s_loaded[i]->lastRun > kIdle)
		{
			freeTree(*s_loaded[i]);
			s_loaded[i] = s_loaded.back();
			s_loaded.pop_back();
		}
		else
			i++;
}

int scriptsLoaded()
{
	return (int)s_loaded.size();
}

void* treePoolTake(size_t bytes)
{
	size_t at = (s_poolUsed + 7) & ~(size_t)7;
	if (!s_pool || at + bytes > s_poolSize)
		return nullptr;
	s_poolUsed = at + bytes;
	return s_pool + at;
}

bool treePoolOwns(const void* p)
{
	const char* c = (const char*)p;
	for (auto& pl : s_pools)
		if (c >= pl.first && c < pl.first + pl.second)
			return true;
	return false;
}
