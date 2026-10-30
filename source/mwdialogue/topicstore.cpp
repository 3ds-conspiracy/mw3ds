// The dialogue responses kept on the card instead of the heap. Of 27,000 responses (Bloodmoon's data) only the Voice
// topics' 6,000 are needed at any moment (Hello, Idle, Attack ... said around the player); the topics, greetings and
// persuasion answers are needed while someone talks. Held in the heap as they were, they took 2.5 MB of an Old 3DS's
// 39 MB, room for one more cell. At the load they go to the card (sdmc:/3ds/mw3ds/dialogue.bin), each topic's
// answers one slice; a talk reads back the topics it looks at, and they go again once it is over.
//
// The file is kept from one start to the next (components/misc/cardslices.hpp): a start with the same data writes
// nothing. Texts and result scripts stay where CardText put them (texts.bin); ids are written out and interned again
// when read (Atom finds its own copy).
// Each response keeps its number in load order (uid): a save's cleared answers ("speaker|uid") stay the same ones.
// Without a card (the file can't be written) every topic stays in the heap, as before. The main thread only.
#include <3ds.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <malloc.h>
#include <vector>

#include "mwdialogue/dialoguemanagerimp.hpp"
#include "mwworld/worldimp.hpp"
#include "components/misc/cardslices.hpp"
#include "components/debug/debuglog.hpp"
#include "platform/console.hpp"
#include "platform/memstat.hpp"

extern "C" u32 __ctru_heap_size;

namespace
{
const char* const kPath = "sdmc:/3ds/mw3ds/dialogue.bin";
CardSlices s_file(kPath, 1);         // (1: putInfo's layout)

std::vector<Topic*> s_loaded;        // spilled topics read back for a talk
size_t s_loadedBytes = 0;
FILE* s_read = nullptr;
// The file read a window at a time: a talk reads back the topics it asks in their order in the file (by name), so the
// next is mostly in the window already. Read one by one, each a seek and a read of the card, an Old 3DS stood still a
// second a talk for 141 topics. (static: out of the heap)
u8 s_win[32 * 1024];
u32 s_winOff = 0, s_winLen = 0;
// A New 3DS keeps the whole file in memory from the load on (2.1 MB with Bloodmoon's data, of a heap with room to
// spare): a talk reads nothing from the card. An Old 3DS reads the card, a window at a time: those 2 MB are a cell's
std::vector<u8> s_all;

// What the talk read (the log as it ends): topics decoded whole, their bytes, reads of the card, ms; topics probed,
// responses walked and the few decoded for them
u32 s_talkTopics = 0, s_talkBytes = 0, s_talkReads = 0, s_talkProbed = 0, s_talkWalked = 0, s_talkDecoded = 0;
u64 s_talkMs = 0;

bool openRead()
{
	if (!s_all.empty())
		return true;
	if (!s_read)
	{
		s_read = fopen(kPath, "rb");
		if (s_read)
			setvbuf(s_read, nullptr, _IONBF, 0);     // (the window is the buffer)
		s_winLen = 0;
	}
	return s_read != nullptr;
}

bool sameText(const char* s, u32 n, const std::string& t)
{
	return n == t.size() && memcmp(s, t.data(), n) == 0;
}

// The slice's bytes: in the window (read anew from its start when it isn't), else into `big`; nullptr when unread
const u8* readSlice(u32 off, u32 len, std::vector<u8>& big)
{
	if (!s_all.empty())
		return (size_t)off + len <= s_all.size() ? s_all.data() + off : nullptr;
	if (off >= s_winOff && off + len <= s_winOff + s_winLen)
		return s_win + (off - s_winOff);
	s_talkReads++;
	if (len > sizeof(s_win))
	{
		big.resize(len);
		return fseek(s_read, off, SEEK_SET) == 0 && fread(big.data(), 1, len, s_read) == len ? big.data() : nullptr;
	}
	s_winLen = 0;
	if (fseek(s_read, off, SEEK_SET) != 0)
		return nullptr;
	s_winOff = off;
	s_winLen = (u32)fread(s_win, 1, sizeof(s_win), s_read);
	return s_winLen >= len ? s_win : nullptr;
}


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
		// (the parse interned every one: found, not made again; a std::string and an insert for each of 30,000 a talk)
		const char* s = Atom::existing((const char*)p, n);
		Atom a = s ? Atom::pooled(s) : Atom(std::string((const char*)p, n));
		p += n;
		return a;
	}
	// A text as it lies in the slice (its length and letters), not interned
	bool view(const char*& s, u32& n)
	{
		n = u();
		if (!ok || (u32)(end - p) < n)
			return ok = false;
		s = (const char*)p;
		p += n;
		return true;
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

// The parse puts the topics on the card one by one as each is read (topicSpillOne), and lets the topic's answers go at
// once: read all and let go at the end, their blocks left holes all through the game's data, between what stays for
// the run (an Old 3DS's heap ended up in pieces no cell fitted)
static bool s_spilling = false;
static int s_spilled = 0;
static std::vector<u8> s_blob;

// Who may answer each topic on the card. A talk asks every topic the player knows whether the speaker has an answer
// (the topic list): read back each to ask, an Old 3DS stood still 2 to 4 seconds a talk late in a game (478 topics).
// Most topics (1,772 of 2,119) answer only someone named by each response: by id, else faction, class or race (a
// filter the response can't pass without). One whose every response names someone keeps those names here; a
// speaker none of them fits gets no answer without the read. A topic with a response anyone may give has none
enum GateKind : u8 { GATE_ID, GATE_FACTION, GATE_CLASS, GATE_RACE };
struct TopicGate
{
	Atom who;
	GateKind kind;
};
static std::vector<TopicGate> s_gates;

static void keepGates(Topic& t)
{
	t.gateAt = (u32)s_gates.size();
	t.gateCount = 0;
	size_t start = s_gates.size();
	for (const Info& info : t.infos)
	{
		TopicGate g = { Atom(), GATE_ID };
		if (!info.whoId.empty())
			g = { info.whoId, GATE_ID };
		else if (!info.whoFaction.empty() && info.whoFaction != "ffff")
			g = { info.whoFaction, GATE_FACTION };
		else if (!info.whoClass.empty())
			g = { info.whoClass, GATE_CLASS };
		else if (!info.whoRace.empty())
			g = { info.whoRace, GATE_RACE };
		if (info.whoId.empty() && (!info.actors.empty() || g.who.empty()))
		{
			s_gates.resize(start);           // (a response anyone may give, or a list of actors: no gates, read back to ask)
			return;
		}
		bool have = false;
		for (size_t k = start; k < s_gates.size() && !have; k++)
			have = s_gates[k].kind == g.kind && s_gates[k].who == g.who;
		if (!have)
			s_gates.push_back(g);
	}
	if (s_gates.size() - start > 0xffff || t.infos.empty())
	{
		s_gates.resize(start);
		return;
	}
	t.gateCount = (u16)(s_gates.size() - start);
}

bool topicMayAnswer(World& w, const Topic& t, int ref)
{
	if (!t.spilled || t.gateCount == 0 || ref < 0)
		return true;
	const Ref& speaker = w.refs[ref];
	const ActorDef* sa = speaker.actor >= 0 ? &w.game.actors[speaker.actor] : nullptr;
	if (!sa)
		return false;
	// (as filter.cpp's passes: a creature says only what names it, with no race, class or faction asked)
	std::string id = lower(sa->id), faction = lower(sa->faction), cls = lower(sa->cls), race = lower(sa->race);
	for (u32 k = t.gateAt; k < t.gateAt + t.gateCount; k++)
	{
		const TopicGate& g = s_gates[k];
		if (g.kind == GATE_ID ? g.who == id
			: !sa->creature && (g.kind == GATE_FACTION ? g.who == faction : g.kind == GATE_CLASS ? g.who == cls : g.who == race))
			return true;
	}
	return false;
}

void topicsSpillBegin()
{
	topicsRelease();                 // (the data loaded again: nothing read back points into the old topics)
	if (s_read)
	{
		fclose(s_read);
		s_read = nullptr;
	}
	std::vector<u8>().swap(s_all);
	s_spilled = 0;
	std::vector<TopicGate>().swap(s_gates);
	s_gates.reserve(6144);           // (Bloodmoon's data: 4,202; reserved once, not grown in steps through the load)
	s_spilling = s_file.begin();
	if (s_spilling)
		s_blob.reserve(256 * 1024);
	else
		logf("dialogue: %s couldn't be written: every topic's responses stay in the heap", kPath);
}

void topicSpillOne(GameData& g, Topic& t)
{
	if (!s_spilling || t.type == TOPIC_VOICE || t.infos.empty())
		return;
	s_blob.clear();
	putU32(s_blob, (u32)t.infos.size());
	for (const Info& info : t.infos)
		putInfo(s_blob, info);
	if (!s_file.put(s_blob.data(), (u32)s_blob.size(), t.spillOff, t.spillCheck))
		return;                          // (this one and the rest stay in the heap)
	t.spillLen = (u32)s_blob.size();
	t.spillCount = (u32)t.infos.size();
	keepGates(t);
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
	if (!s_spilling)
		return;
	s_spilling = false;
	if (!s_file.end())
		monitorOnce("topicwrite", "dialogue: %s wasn't written whole", kPath);
	// (a New 3DS: the file back whole, as written; a read that falls short leaves the card to the talks)
	if (!g_profile.smallHeap && s_spilled)
	{
		FILE* f = fopen(kPath, "rb");
		if (f)
		{
			s_all.resize(s_file.size());
			if (fread(s_all.data(), 1, s_all.size(), f) != s_all.size())
				std::vector<u8>().swap(s_all);
			fclose(f);
		}
	}
	logf("dialogue: %d topics' responses on the card (%u KB, %u KB written)%s", s_spilled, (unsigned)(s_file.size() / 1024),
		(unsigned)(s_file.written() / 1024), s_all.empty() ? "" : ", held in memory too");
}

const std::vector<Info>& topicInfos(World& w, const Topic& t)
{
	if (!t.spilled || !t.infos.empty() || t.spillCount == 0)
		return t.infos;
	u64 start = osGetTime();
	std::vector<u8> big;
	const u8* slice = openRead() ? readSlice(t.spillOff, t.spillLen, big) : nullptr;
	if (!slice)
	{
		monitorOnce("topicread", "dialogue: %s's responses couldn't be read from the card", t.name.c_str());
		return t.infos;
	}
	// (not what was put there: the next start writes the file again)
	if (CardSlices::check(slice, t.spillLen) != t.spillCheck)
	{
		monitorOnce("topicdamaged", "dialogue: %s's responses on the card are damaged", t.name.c_str());
		fclose(s_read);
		s_read = nullptr;
		s_winLen = 0;
		s_file.damaged();
		return t.infos;
	}
	// (heap kept: a long talk late in a game on an Old 3DS, its cells' memory first, as a cell read waits for)
	size_t need = (size_t)t.spillCount * (sizeof(Info) + 2 * sizeof(Cond)) + t.spillLen;
	if (heapInUse() + need + (512u << 10) > __ctru_heap_size && w.memoryLowHook)
		w.memoryLowHook(true);
	Reader r{ slice, slice + t.spillLen };
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
	s_talkTopics++;
	s_talkBytes += t.spillLen;
	s_talkMs += osGetTime() - start;
	return t.infos;
}

// Which of the topic's responses on the card could be the speaker's, decoding none that can't: the who-may-say-it
// fields of each are compared as they lie in the slice (the speaker's id, race, class and faction, the actors named),
// and only a response they don't rule out is decoded, into one Info kept for it, and judged by the dialogue's filter
// (judge: 1 it answers, 2 only the speaker's disposition refuses it, 0 no). The first that answers ends it. A talk's
// topic list asked every known topic and decoded all of each to find out (5,000 responses, 670 KB, from a level 10
// save, 30,000 texts interned again): an Old 3DS stood still a second and more each talk. -1: not on the card (in the
// heap: Voice topics, a card that couldn't be written), the caller asks the Infos themselves
// (the responses found for the talk, copied out of the probe: a greeting's, a topic's answer. Their pointers hold until
// the talk's end, topicsRelease)
static std::deque<std::pair<const Topic*, Info>> s_found;

int topicProbe(World& w, const Topic& t, int ref, const std::function<int(const Info&)>& judge, const Info** found)
{
	if (found)
		*found = nullptr;
	if (!t.spilled || t.spillCount == 0 || !t.infos.empty() || ref < 0)
		return -1;
	const Ref& speaker = w.refs[ref];
	const ActorDef* sa = speaker.actor >= 0 ? &w.game.actors[speaker.actor] : nullptr;
	if (!sa)
		return 0;
	// (the speaker's, lowercased once a speaker, not once a topic)
	static int s_forRef = -1, s_forActor = -1;
	static std::string s_id, s_race, s_cls, s_fac;
	if (ref != s_forRef || speaker.actor != s_forActor)
	{
		s_forRef = ref;
		s_forActor = speaker.actor;
		s_id = lower(sa->id);
		s_race = lower(sa->race);
		s_cls = lower(sa->cls);
		s_fac = lower(sa->faction);
	}
	u64 start = osGetTime();
	std::vector<u8> big;
	const u8* slice = openRead() ? readSlice(t.spillOff, t.spillLen, big) : nullptr;
	if (!slice)
	{
		monitorOnce("topicread", "dialogue: %s's responses couldn't be read from the card", t.name.c_str());
		return 0;
	}
	if (CardSlices::check(slice, t.spillLen) != t.spillCheck)
	{
		monitorOnce("topicdamaged", "dialogue: %s's responses on the card are damaged", t.name.c_str());
		fclose(s_read);
		s_read = nullptr;
		s_winLen = 0;
		s_file.damaged();
		return 0;
	}
	static Info s_probe;
	Reader r{ slice, slice + t.spillLen };
	u32 n = r.u();
	int result = 0;
	s_talkProbed++;
	for (u32 k = 0; k < n && r.ok; k++)
	{
		const u8* at = r.p;
		for (int f = 0; f < 6; f++)
			r.u();                           // uid, sex, rank, disposition, text, script
		const char* v[6] = {};
		u32 vn[6] = {};
		for (int f = 0; f < 6 && r.ok; f++)
			r.view(v[f], vn[f]);             // whoId, whoRace, whoClass, whoFaction, whoCell, sound
		u32 na = r.u();
		bool actorOk = na == 0;
		for (u32 a = 0; a < na && r.ok; a++)
			actorOk |= (int)r.u() == speaker.actor;
		u32 nc = r.u();
		for (u32 c = 0; c < nc && r.ok; c++)
		{
			r.byte();
			r.byte();
			r.u();
			r.u();
			const char* nm;
			u32 nn;
			r.view(nm, nn);
		}
		if (!r.ok)
			break;
		s_talkWalked++;
		// (as filter.cpp's passes rules them out: a creature says only what names it, and asks no race, class or
		// faction; "ffff" is no faction: ruled out for a speaker who has one)
		bool out = !actorOk || (sa->creature && vn[0] == 0 && na == 0) || (vn[0] && !sameText(v[0], vn[0], s_id));
		if (!out && !sa->creature)
			out = (vn[1] && !sameText(v[1], vn[1], s_race)) || (vn[2] && !sameText(v[2], vn[2], s_cls))
				|| (vn[3] && (sameText(v[3], vn[3], "ffff") ? !s_fac.empty() : !sameText(v[3], vn[3], s_fac)));
		if (out)
			continue;
		Reader one{ at, r.end };
		s_probe.actors.clear();
		s_probe.conds.clear();
		if (!getInfo(one, s_probe))
			break;
		s_talkDecoded++;
		int j = judge(s_probe);
		if (j == 1)
		{
			result = 1;
			if (found)
			{
				s_found.emplace_back(&t, s_probe);
				*found = &s_found.back().second;
			}
			break;
		}
		if (j == 2)
			result = 2;
	}
	if (!r.ok)
		monitorOnce("topicdamaged", "dialogue: %s's responses on the card are damaged", t.name.c_str());
	s_talkMs += osGetTime() - start;
	return result;
}

// Whether the response is one of the topic's own, in its list or a copy the probe made of one (not one of Info
// Refusal's, found for another topic): what the journal's Topics index and a topic's "heard already" go by
bool topicOwns(const Topic& t, const Info* info)
{
	if (!t.infos.empty() && info >= &t.infos.front() && info <= &t.infos.back())
		return true;
	for (const auto& f : s_found)
		if (&f.second == info)
			return f.first == &t;
	return false;
}

unsigned topicsTalkReads()
{
	return s_talkReads;
}

unsigned topicsTalkDecoded()
{
	return s_talkDecoded;
}

void topicsRelease()
{
	if (s_talkTopics || s_talkProbed)
		logf("dialogue: the talk read %u topics back whole (%u KB) and probed %u (%u responses walked, %u decoded), %u reads of the card, %llu ms",
			(unsigned)s_talkTopics, (unsigned)(s_talkBytes / 1024), (unsigned)s_talkProbed, (unsigned)s_talkWalked,
			(unsigned)s_talkDecoded, (unsigned)s_talkReads, (unsigned long long)s_talkMs);
	s_talkTopics = s_talkBytes = s_talkReads = s_talkProbed = s_talkWalked = s_talkDecoded = 0;
	s_talkMs = 0;
	s_found.clear();
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
