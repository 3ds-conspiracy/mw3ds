// The dialogue responses kept on the card instead of the heap. Of 27,000 responses (Bloodmoon's data) only the Voice
// topics' 6,000 are needed at any moment (Hello, Idle, Attack ... said around the player); the topics, greetings and
// persuasion answers are needed while someone talks. Held in the heap as they were, they took 2.5 MB of an Old 3DS's
// 39 MB, room for one more cell. At the load they go to the card (sdmc:/3ds/mw3ds/dialogue.bin), each topic's
// answers one slice; a talk reads back the topics it looks at, and they go again once it is over.
//
// The file is written afresh at each start, as the topics are read. Texts and result scripts stay where CardText put
// them (texts.bin); ids are written out and interned again when read (Atom finds its own copy).
// Each response keeps its number in load order (uid): a save's cleared answers ("speaker|uid") stay the same ones.
// Without a card (the file can't be written) every topic stays in the heap, as before. The main thread only.
#include <3ds.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <vector>

#include "mwdialogue/dialoguemanagerimp.hpp"
#include "mwworld/worldimp.hpp"
#include "components/debug/debuglog.hpp"
#include "platform/memstat.hpp"

extern "C" u32 __ctru_heap_size;

namespace
{
const char* const kPath = "sdmc:/3ds/mw3ds/dialogue.bin";

std::vector<Topic*> s_loaded;        // spilled topics read back for a talk
size_t s_loadedBytes = 0;
FILE* s_read = nullptr;
char s_readBuf[8 * 1024];            // (static: a FILE's own buffer would come out of the heap)


void putU32(std::vector<u8>& b, u32 v)
{
	u8 t[4];
	memcpy(t, &v, 4);
	b.insert(b.end(), t, t + 4);
}

void putAtom(std::vector<u8>& b, const Atom& a)
{
	size_t n = a.size();
	putU32(b, (u32)n);
	b.insert(b.end(), (const u8*)a.c_str(), (const u8*)a.c_str() + n);
}

void putInfo(std::vector<u8>& b, const Info& info)
{
	putU32(b, (u32)info.uid);
	putU32(b, (u32)info.whoSex);
	putU32(b, (u32)info.whoRank);
	putU32(b, (u32)info.disposition);
	putU32(b, info.text.raw());
	putU32(b, info.script.raw());
	putAtom(b, info.whoId);
	putAtom(b, info.whoRace);
	putAtom(b, info.whoClass);
	putAtom(b, info.whoFaction);
	putAtom(b, info.whoCell);
	putAtom(b, info.sound);
	putU32(b, (u32)info.actors.size());
	for (int a : info.actors)
		putU32(b, (u32)a);
	putU32(b, (u32)info.conds.size());
	for (const Cond& c : info.conds)
	{
		b.push_back((u8)c.kind);
		b.push_back((u8)c.op);
		putU32(b, (u32)c.func);
		u32 v;
		memcpy(&v, &c.value, 4);
		putU32(b, v);
		putAtom(b, c.name);
	}
}

// Reads back what putInfo wrote; false when the slice runs short (a damaged file)
struct Reader
{
	const u8* p;
	const u8* end;
	bool ok = true;
	u32 u()
	{
		if (end - p < 4)
		{
			ok = false;
			return 0;
		}
		u32 v;
		memcpy(&v, p, 4);
		p += 4;
		return v;
	}
	u8 byte()
	{
		if (p >= end)
		{
			ok = false;
			return 0;
		}
		return *p++;
	}
	Atom atom()
	{
		u32 n = u();
		if (!ok || (u32)(end - p) < n)
		{
			ok = false;
			return Atom();
		}
		Atom a(std::string((const char*)p, n));
		p += n;
		return a;
	}
};

bool getInfo(Reader& r, Info& info)
{
	info.uid = (int)r.u();
	info.whoSex = (int)r.u();
	info.whoRank = (int)r.u();
	info.disposition = (int)r.u();
	info.text = CardText::fromRaw(r.u());
	info.script = CardText::fromRaw(r.u());
	info.whoId = r.atom();
	info.whoRace = r.atom();
	info.whoClass = r.atom();
	info.whoFaction = r.atom();
	info.whoCell = r.atom();
	info.sound = r.atom();
	u32 na = r.u();
	for (u32 k = 0; k < na && r.ok; k++)
		info.actors.push_back((int)r.u());
	u32 nc = r.u();
	info.conds.reserve(nc < 64 ? nc : 0);
	for (u32 k = 0; k < nc && r.ok; k++)
	{
		Cond c;
		c.kind = (char)r.byte();
		c.op = (char)r.byte();
		c.func = (int)r.u();
		u32 v = r.u();
		memcpy(&c.value, &v, 4);
		c.name = r.atom();
		info.conds.push_back(c);
	}
	return r.ok;
}

size_t infoBytes(const std::vector<Info>& infos)
{
	size_t n = infos.capacity() * sizeof(Info);
	for (const Info& i : infos)
		n += i.conds.capacity() * sizeof(Cond) + i.actors.capacity() * sizeof(int);
	return n;
}
}

// The parse writes the file afresh at each start, topic by topic as each is read (topicSpillOne), and lets the topic's
// answers go at once: read all and let go at the end, their blocks left holes all through the game's data, between
// what stays for the run (an Old 3DS's heap ended up in pieces no cell fitted)
static FILE* s_write = nullptr;
static u32 s_writeAt = 0;
static int s_spilled = 0;
static bool s_writeFailed = false;
static std::vector<u8> s_blob;

void topicsSpillBegin()
{
	topicsRelease();                 // (the data loaded again: nothing read back points into the old topics)
	s_write = fopen(kPath, "wb");
	s_writeAt = 0;
	s_spilled = 0;
	s_writeFailed = s_write == nullptr;
	if (s_write)
	{
		static char writeBuf[16 * 1024];
		setvbuf(s_write, writeBuf, _IOFBF, sizeof(writeBuf));
		s_blob.reserve(256 * 1024);
	}
	else
		logf("dialogue: %s couldn't be written: every topic's responses stay in the heap", kPath);
}

void topicSpillOne(GameData& g, Topic& t)
{
	if (!s_write || s_writeFailed || t.type == TOPIC_VOICE || t.infos.empty())
		return;
	s_blob.clear();
	putU32(s_blob, (u32)t.infos.size());
	for (const Info& info : t.infos)
		putInfo(s_blob, info);
	if (fwrite(s_blob.data(), 1, s_blob.size(), s_write) != s_blob.size())
	{
		s_writeFailed = true;            // (this one and the rest stay in the heap)
		return;
	}
	t.spillOff = s_writeAt;
	t.spillLen = (u32)s_blob.size();
	t.spillCount = (u32)t.infos.size();
	s_writeAt += (u32)s_blob.size();
	// (who some answer names as its speaker: GameData::speakers, read from the answers while they are here)
	for (const Info& info : t.infos)
		if (!info.whoId.empty())
			g.speakers.insert(lower(info.whoId.str()));
	std::vector<Info>().swap(t.infos);
	t.spilled = true;
	s_spilled++;
}

void topicsSpillEnd()
{
	std::vector<u8>().swap(s_blob);
	if (!s_write)
		return;
	if (fclose(s_write) != 0 || s_writeFailed)
		monitorOnce("topicwrite", "dialogue: %s wasn't written whole", kPath);
	s_write = nullptr;
	logf("dialogue: %d topics' responses on the card (%u KB)", s_spilled, (unsigned)(s_writeAt / 1024));
}

const std::vector<Info>& topicInfos(World& w, const Topic& t)
{
	if (!t.spilled || !t.infos.empty() || t.spillCount == 0)
		return t.infos;
	if (!s_read)
	{
		s_read = fopen(kPath, "rb");
		if (s_read)
			setvbuf(s_read, s_readBuf, _IOFBF, sizeof(s_readBuf));
	}
	std::vector<u8> slice(t.spillLen);
	if (!s_read || fseek(s_read, t.spillOff, SEEK_SET) != 0 || fread(slice.data(), 1, slice.size(), s_read) != slice.size())
	{
		monitorOnce("topicread", "dialogue: %s's responses couldn't be read from the card", t.name.c_str());
		return t.infos;
	}
	// (heap kept: a long talk late in a game on an Old 3DS, its cells' memory first, as a cell read waits for)
	size_t need = (size_t)t.spillCount * (sizeof(Info) + 2 * sizeof(Cond)) + slice.size();
	if (heapInUse() + need + (512u << 10) > __ctru_heap_size && w.memoryLowHook)
		w.memoryLowHook(true);
	Reader r{ slice.data(), slice.data() + slice.size() };
	u32 n = r.u();
	t.infos.reserve(n);
	for (u32 k = 0; k < n && r.ok; k++)
	{
		t.infos.emplace_back();
		if (!getInfo(r, t.infos.back()))
			t.infos.pop_back();
	}
	if (!r.ok)
		monitorOnce("topicdamaged", "dialogue: %s's responses on the card are damaged", t.name.c_str());
	s_loaded.push_back(const_cast<Topic*>(&t));
	s_loadedBytes += infoBytes(t.infos);
	return t.infos;
}

void topicsRelease()
{
	if (s_loaded.empty())
		return;
	for (Topic* t : s_loaded)
		std::vector<Info>().swap(t->infos);
	s_loaded.clear();
	s_loadedBytes = 0;
	if (s_read)
	{
		fclose(s_read);
		s_read = nullptr;
	}
}

size_t topicsLoadedBytes()
{
	return s_loadedBytes;
}
