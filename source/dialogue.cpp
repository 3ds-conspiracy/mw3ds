#include "dialogue.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <cstring>

#include "log.h"
#include "script.h"
#include "world.h"

static bool compare(char op, float a, float b)
{
	switch (op)
	{
	case '0': return a == b;
	case '1': return a != b;
	case '2': return a > b;
	case '3': return a >= b;
	case '4': return a < b;
	case '5': return a <= b;
	}
	return false;
}

// Whether the speaker had talked to the player when this conversation began (OpenMW caches it for the whole
// talk, so "Talked to PC = 0" lines still fit after the greeting); s_talkRef: whose, -1 when no talk is open
static int s_talkRef = -1;
static bool s_talked = false;

int g_dialogueClothValue = -1;

// Dialogue filter functions ('1' conditions), numbered as in the construction set
static float filterFunction(World& w, const Cond& c, const Ref& speaker, int ref, int choice)
{
	const ActorDef* a = speaker.actor >= 0 ? &w.game.actors[speaker.actor] : nullptr;
	int f = c.func;
	if (f == 10) return w.stats.attributes[ATTR_STRENGTH];
	if (f >= 11 && f <= 37) return w.stats.skills[f - 11];
	if (f >= 51 && f <= 57) return w.stats.attributes[f - 50];
	switch (f)
	{
	case 50: return (float)choice;
	case 63:                                                         // talked to PC: as it was when this talk began
		return (ref == s_talkRef ? s_talked : speaker.talkedToPC) ? 1.0f : 0.0f;
	case 3: return (float)speaker.reputation + (a ? a->reputation : 0);
	case 5: return (float)w.pcReputation;
	case 43: return (float)w.bounty;                // PC crime level
	case 39: return a && w.pcExpelled.count(lower(a->faction)) ? 1.0f : 0.0f;   // PC expelled
	case 40: return w.pcHasDisease(3) ? 1.0f : 0.0f;                 // PC common disease
	case 41: return w.pcHasDisease(2) ? 1.0f : 0.0f;                 // PC blight disease
	case 58: return w.pcHasCorprus() ? 1.0f : 0.0f;                  // PC corprus
	case 60: return w.effectTotal(133) > 0.0f ? 1.0f : 0.0f;        // PC vampire: the Vampirism effect, not a script variable
	case 59: return (float)w.weatherNow;                             // weather (0 clear .. 9 blizzard)
	case 66: return (float)std::min(speaker.friendlyHits, 4);        // friendly hit (OpenMW stops at 4)
	case 65:                                                         // creature targeted: fighting a creature
	{
		if (!speaker.ally && !(speaker.aiPackage == AIPKG_FOLLOW && speaker.aiTarget == "player"))
			return 0.0f;
		for (int i : w.loadedActors)
		{
			const Ref& o = w.refs[i];
			if (o.ai == AI_COMBAT && !o.dead && !o.ally && o.actor >= 0 && w.game.actors[o.actor].creature
				&& hypotf(o.pos[0] - speaker.pos[0], o.pos[1] - speaker.pos[1]) < 1500.0f)
				return 1.0f;
		}
		return 0.0f;
	}
	case 49:                                                         // alarmed: fighting, or a guard come to arrest
		return speaker.ai == AI_COMBAT || (speaker.alarmed && w.bounty > 0) ? 1.0f : 0.0f;   // (paying clears it)
	case 62: return speaker.attacked ? 1.0f : 0.0f;                  // attacked
	case 71:                                                         // should attack: in combat, or fight term >= 100
		return w.shouldAttackHook ? (w.shouldAttackHook(ref) ? 1.0f : 0.0f)
			: (speaker.fight >= 0 ? speaker.fight : a ? a->fight : 30) >= 80 ? 1.0f : 0.0f;
	case 0: case 1:                                                  // lowest / highest reaction of the speaker's faction to the player's
	{
		std::string fac = a ? lower(a->faction) : "";
		if (fac.empty())
			return 0.0f;
		int value = 0;
		for (auto& pf : w.pcRank)
		{
			int re = w.factionReaction(fac, pf.first);
			if (f == 0 ? re < value : re > value)
				value = re;
		}
		return (float)value;
	}
	case 2:
	{
		// Rank requirement: +1 the player meets the next rank's attributes and skills, +2 its reputation
		std::string f = a ? lower(a->faction) : "";
		auto fit = w.game.factions.find(f);
		if (fit == w.game.factions.end())
			return 0.0f;
		const FactionDef& fd = fit->second;
		int next = w.pcRankIn(f) + 1;
		if (next >= 10 || next >= (int)fd.ranks.size())
			return 0.0f;
		const int* rq = fd.reqs[next];
		const PlayerStats& s = w.stats;
		bool attrs = (fd.attrs[0] < 0 || s.attributes[fd.attrs[0]] >= rq[0]) && (fd.attrs[1] < 0 || s.attributes[fd.attrs[1]] >= rq[1]);
		std::vector<int> sk;
		for (int k : fd.skills)
			if (k >= 0 && k < 27)
				sk.push_back(s.skills[k]);
		std::sort(sk.rbegin(), sk.rend());
		bool skills = sk.empty() || (sk[0] >= rq[2] && (sk.size() < 2 || sk[1] >= rq[3]));
		auto rep = w.pcFacRep.find(f);
		bool reputation = (rep != w.pcFacRep.end() ? rep->second : 0) >= rq[4];
		return (attrs && skills ? 1.0f : 0.0f) + (reputation ? 2.0f : 0.0f);
	}
	case 46: return a && !a->faction.empty() && w.pcRankIn(lower(a->faction)) >= 0 ? 1.0f : 0.0f;   // same faction
	case 47: return a && !a->faction.empty() ? (float)(w.pcRankIn(lower(a->faction)) - a->rank) : 0.0f;   // the player's rank (-1: none) less theirs
	case 67: return (float)(speaker.fight >= 0 ? speaker.fight : a ? a->fight : 30);          // fight
	case 68: return (float)(speaker.hello >= 0 ? speaker.hello : a ? a->hello : 30);          // hello
	case 44: return a && !a->creature && (a->female != 0) == w.stats.female ? 1.0f : 0.0f;       // (a creature: never)
	case 45: return a && !a->creature && lower(a->race) == lower(w.stats.race) ? 1.0f : 0.0f;
	case 38: return w.stats.female ? 1.0f : 0.0f;
	case 6: return (float)w.stats.level;
	case 61: return a ? (float)a->level : 1.0f;
	case 4: return speaker.healthMax > 0.0f ? (float)(int)(speaker.health * 100.0f / speaker.healthMax) : 100.0f;   // health percent (whole)
	case 7: return w.stats.healthMax > 0.0f ? (float)(int)(w.stats.health * 100.0f / w.stats.healthMax) : 100.0f;   // PC health percent
	case 64: return w.stats.health;
	case 8: return w.stats.magicka;
	case 9: return w.stats.fatigue;
	case 48: return !w.awarenessHook || w.awarenessHook(ref) ? 1.0f : 0.0f;   // detected: the speaker notices the player
	case 69: return (float)(speaker.alarm >= 0 ? speaker.alarm : a ? a->alarm : 0);           // alarm
	case 70: return (float)(speaker.flee >= 0 ? speaker.flee : a ? a->flee : 30);             // flee
	case 42:
	{
		// clothing modifier: value of what is worn (armor except the shield, and clothing; not weapons, ammo or lights)
		if (g_dialogueClothValue >= 0)
			return (float)g_dialogueClothValue;
		float sum = 0.0f;
		for (auto& it : w.inventory)
			if (it.equipped)
				if (const Object* o = w.game.object(it.id))
					if (o->type == "CLOT" || (o->type == "ARMO" && o->subtype != 8))
						sum += o->value;
		return sum;
	}
	default: return 0.0f;   // crime, disease, vampirism, faction standing, weather: none in this level
	}
}

// invert: Service Refusal's disposition is a ceiling (they refuse below it; 0 = always), as OpenMW has it
// 0: no; 1: yes; 2: it fits but for the speaker's disposition (OpenMW's infoRefusal)
static int passes(World& w, const Info& info, int ref, int choice, bool invert = false)
{
	const Ref& speaker = w.refs[ref];
	const ActorDef* sa = speaker.actor >= 0 ? &w.game.actors[speaker.actor] : nullptr;
	if (!sa)
		return 0;
	// creatures (Dagoth Endus, the Puzzle Canal dremora, Creeper) say only what names them as speaker
	if (sa->creature && info.whoId.empty() && info.actors.empty())
		return 0;
	if (!info.actors.empty())
	{
		bool actorOk = false;
		for (int a : info.actors)
			if (a == speaker.actor)
				actorOk = true;
		if (!actorOk)
			return 0;
	}
	// Who may say it
	if (!info.whoId.empty() && lower(sa->id) != info.whoId)
		return 0;
	// A creature named as the speaker skips the race, class, faction, rank and sex filters (OpenMW's testActor)
	if (!sa->creature)
	{
		if (!info.whoRace.empty() && lower(sa->race) != info.whoRace)
			return 0;
		if (!info.whoClass.empty() && lower(sa->cls) != info.whoClass)
			return 0;
		if (!info.whoFaction.empty() && (info.whoFaction == "ffff" ? !sa->faction.empty() : lower(sa->faction) != info.whoFaction))
			return 0;
		// Rank: the speaker's own in their faction (-1 without one); not asked of "no faction" lines
		if (info.whoRank >= 0 && info.whoFaction != "ffff" && (sa->faction.empty() ? -1 : sa->rank) < info.whoRank)
			return 0;
		if (info.whoSex >= 0 && (sa->female ? 1 : 0) != info.whoSex)
			return 0;
	}
	// The cell is the player's (a prefix of its name)
	if (!info.whoCell.empty() && (w.current < 0 || lower(w.cells[w.current].name).compare(0, info.whoCell.size(), info.whoCell) != 0))
		return 0;
	if (info.uid >= 0 && !w.clearedInfos.empty() && w.clearedInfos.count(speaker.idLower + "|" + std::to_string(info.uid)))
		return 0;                  // ClearInfoActor: said once, not again
	const ScriptInstance* si = speaker.script >= 0 ? &w.scripts[speaker.script] : nullptr;
	for (const Cond& c : info.conds)
	{
		float value = 0.0f;
		switch (c.kind)
		{
		case '1':
			if (c.func == 50 && choice < 0)
				return 0;     // choice responses only answer a choice
			if (c.func == 59 && w.cells[w.placeOf(ref)].interior)
				return 0;     // weather is not asked indoors
			value = filterFunction(w, c, speaker, ref, choice);
			break;
		case '2':
		{
			auto g = w.globals.find(c.name);
			if (g == w.globals.end())
				continue;     // a global that does not exist is ignored
			value = g->second;
			break;
		}
		case '3':
		{
			int idx = si ? si->script->localIndex(c.name) : -1;
			if (idx < 0)
				return 0;
			value = si->locals[idx];
			break;
		}
		case 'C':
		{
			// NotLocal: the opposite of the local test (no script or no such variable: passes)
			int idx = si ? si->script->localIndex(c.name) : -1;
			if (idx >= 0 && compare(c.op, si->locals[idx], c.value))
				return 0;
			continue;
		}
		case '4': value = (float)w.getJournal(c.name); break;
		case 'F':
		{
			// The player must be in this faction, at least at this rank
			int rank = w.pcRankIn(c.name);
			if (rank < 0)
				return 0;
			value = (float)rank;
			break;
		}
		case 'G':
		{
			// The player's rank in the speaker's faction
			value = !sa->faction.empty() ? (float)w.pcRankIn(lower(sa->faction)) : -1.0f;
			break;
		}
		case '5': value = (float)w.itemCount(c.name); break;
		// Not ID / faction / class / race / cell: 1 when the speaker's differs (the operator and value are not used)
		case '7': value = lower(sa->id) != c.name ? 1.0f : 0.0f; break;
		case '8': value = lower(sa->faction) != c.name ? 1.0f : 0.0f; break;
		case '9': value = lower(sa->cls) != c.name ? 1.0f : 0.0f; break;
		case 'A': value = lower(sa->race) != c.name ? 1.0f : 0.0f; break;
		case 'B': value = lower(w.cells[w.placeOf(ref)].name).compare(0, c.name.size(), c.name) != 0 ? 1.0f : 0.0f; break;
		case '6':
		{
			auto dead = w.deadCounts.find(c.name);
			value = dead != w.deadCounts.end() ? (float)dead->second : 0.0f;
			break;
		}
		default: return 0;
		}
		if (c.kind == '7' || c.kind == '8' || c.kind == '9' || c.kind == 'A' || c.kind == 'B')
		{
			if (value == 0.0f)
				return 0;
			continue;
		}
		if (!compare(c.op, value, c.value))
			return 0;
	}
	// Disposition last: a response only the speaker's mood refuses falls back to "Info Refusal". Creatures
	// don't ask. invert: Service Refusal's disposition is a ceiling (they refuse below it; 0 = always)
	if (!sa->creature)
	{
		int disp = w.disposition(ref);
		if (invert ? info.disposition != 0 && disp >= info.disposition : disp < info.disposition)
			return 2;
	}
	return 1;
}

// The first response that fits; with refusal, a topic whose responses fit but for the speaker's disposition
// answers with the "Info Refusal" topic's instead (OpenMW's search(topic, true): topic lists and learning too)
const Info* dialogueFindInfo(World& w, const Topic& topic, int ref, int choice, bool invert, bool refusal)
{
	bool refused = false;
	for (const Info& info : topic.infos)
	{
		int r = passes(w, info, ref, choice, invert);
		if (r == 1)
			return &info;
		refused |= r == 2;
	}
	if (refused && refusal)
		if (const Topic* t = w.game.topic("Info Refusal"))
			return dialogueFindInfo(w, *t, ref, choice, invert, false);
	return nullptr;
}

static std::string raceName(World& w, const std::string& id)
{
	for (auto& r : w.game.races)
		if (lower(r.id) == lower(id))
			return r.name;
	return id;
}

static std::string className(World& w, const std::string& id)
{
	for (auto& c : w.game.classes)
		if (lower(c.id) == lower(id))
			return c.name;
	return id;
}

// OpenMW's fixDefinesDialog: % or ^ and a name. Someone with no faction (or a rank out of range) gives a
// plain "%"; the player's next rank stops at the highest; any global's name gives its value
std::string dialogueSubstitute(World& w, const std::string& text, int ref)
{
	const ActorDef* a = ref >= 0 && w.refs[ref].actor >= 0 ? &w.game.actors[w.refs[ref].actor] : nullptr;
	std::string fac = a ? lower(a->faction) : "";
	int pcRank = fac.empty() ? -1 : w.pcRankIn(fac);
	std::string nextRank = fac.empty() ? "%" : w.game.rankName(fac, std::min(9, pcRank + 1));
	struct { const char* key; std::string value; } vars[] = {
		{ "%nextpcrank", nextRank },
		{ "%pcnextrank", nextRank },
		{ "%pccrimelevel", std::to_string(w.bounty) },
		{ "%pcname", w.stats.name },
		{ "%pcrace", raceName(w, w.stats.race) },
		{ "%pcclass", className(w, w.stats.cls) },
		{ "%pcrank", fac.empty() ? "%" : w.game.rankName(fac, std::max(0, pcRank)) },
		{ "%name", a ? a->name : "" },
		{ "%race", a ? raceName(w, a->race) : "" },
		{ "%class", a ? className(w, a->cls) : "" },
		{ "%faction", fac.empty() ? "%" : w.game.factionName(a->faction) },
		{ "%rank", fac.empty() || a->rank < 0 || a->rank > 9 ? "%" : w.game.rankName(a->faction, a->rank) },
		{ "%cell", w.cellName() },
	};
	std::string out;
	for (size_t i = 0; i < text.size(); i++)
	{
		bool replaced = false;
		if (text[i] == '%' || text[i] == '^')
		{
			for (auto& v : vars)
			{
				size_t len = strlen(v.key) - 1;
				if (lower(text.substr(i + 1, len)) == v.key + 1)
				{
					out += v.value;
					i += len;
					replaced = true;
					break;
				}
			}
			if (!replaced)
			{
				// A global's name: the longest that fits
				const std::pair<const std::string, float>* best = nullptr;
				for (auto& g : w.globals)
					if ((!best || g.first.size() > best->first.size()) && !g.first.empty()
						&& lower(text.substr(i + 1, g.first.size())) == g.first)
						best = &g;
				if (best)
				{
					char num[32];
					snprintf(num, sizeof(num), "%g", best->second);
					out += num;
					i += best->first.size();
					replaced = true;
				}
			}
		}
		if (!replaced && (text[i] == '%' || text[i] == '^'))
		{
			// Unmatched: printed as it is, and the character after it is never an escape (OpenMW's fixDefines)
			out += text[i];
			if (i + 1 < text.size())
				out += text[++i];
		}
		else if (!replaced)
			out += text[i];
	}
	return out;
}

// What may come before a keyword (OpenMW's word separators)
static const char kWordStarts[] = { '\n', '\r', ' ', '\t', '\'', '"', '(', '[', 0 };

std::vector<KeywordMatch> dialogueFindKeywords(const std::string& text, const std::vector<std::string>& keywords)
{
	std::string l = lower(text);
	std::vector<KeywordMatch> found, out;
	for (size_t i = 0; i < l.size(); i++)
	{
		if (i > 0 && !strchr(kWordStarts, l[i - 1]))
			continue;
		for (size_t k = 0; k < keywords.size(); k++)
		{
			const std::string& kw = keywords[k];
			if (!kw.empty() && kw[0] == l[i] && l.compare(i, kw.size(), kw) == 0)
				found.push_back({ i, i + kw.size(), (int)k });
		}
	}
	// Overlaps: the longest of the first run of overlapping matches goes out, what it overlaps goes, again
	while (!found.empty())
	{
		size_t best = 0, bestSize = 0;
		for (size_t m = 0; m < found.size(); m++)
		{
			size_t size = found[m].end - found[m].begin;
			if (size > bestSize)
			{
				bestSize = size;
				best = m;
			}
			if (m + 1 == found.size() || found[m].end <= found[m + 1].begin)
				break;
		}
		KeywordMatch kw = found[best];
		out.push_back(kw);
		std::vector<KeywordMatch> rest;
		for (auto& m : found)
			if (!(m.begin < kw.end && m.end > kw.begin))
				rest.push_back(m);
		found.swap(rest);
	}
	std::sort(out.begin(), out.end(), [](const KeywordMatch& a, const KeywordMatch& b) { return a.begin < b.begin; });
	return out;
}

// Topics named in what someone says become known, when the speaker has something to say about them
// (OpenMW's addTopicsFromText; the names searched are every dialogue's, so a greeting or a voice
// name can take a stretch of text that a shorter topic would otherwise have)
// Topics someone named who had nothing to say about them (not learned, as in OpenMW): who named them last,
// for the tests to tell "never named" from "named by someone with no answer"
static std::map<std::string, std::string> s_namedUnlearned;

// OpenMW's updateDialogueGlobals, at the start of a talk and whenever the topics are worked out again: what the
// guards' lines test about the bounty and whether the player can pay it
void dialogueUpdateGlobals(World& w)
{
	int gold = w.itemCount("gold_001");
	int discount = (int)(w.bounty * w.game.gmstf("fcrimegolddiscountmult", 0.9f));
	int turnIn = (int)(w.bounty * w.game.gmstf("fcrimegoldturninmult", 1.0f));
	if (w.bounty > 0)
	{
		discount = std::max(1, discount);
		turnIn = std::max(1, turnIn);
	}
	w.globals["pchascrimegold"] = w.bounty <= gold ? 1.0f : 0.0f;
	w.globals["pchasgolddiscount"] = discount <= gold ? 1.0f : 0.0f;
	w.globals["crimegolddiscount"] = (float)discount;
	w.globals["crimegoldturnin"] = (float)turnIn;
	w.globals["pchasturnin"] = turnIn <= gold ? 1.0f : 0.0f;
}

// Whether info is one of the topic's own (not one of Info Refusal's)
static bool inTopic(const Topic& t, const Info* info)
{
	return !t.infos.empty() && info >= &t.infos.front() && info <= &t.infos.back();
}

// The topics a raw text names (OpenMW's parseHyperText): "@name#" links are always a hit, the text between them
// is searched for keywords
static std::vector<const Topic*> topicsNamedIn(World& w, const std::string& text)
{
	static std::vector<std::string> names;
	static std::vector<const Topic*> named;
	if (named.size() != w.game.topics.size() || (!named.empty() && named[0] != &w.game.topics[0]))
	{
		names.clear();
		named.clear();
		for (auto& t : w.game.topics)
		{
			names.push_back(t.lower);
			named.push_back(&t);
		}
	}
	std::vector<const Topic*> out;
	auto scan = [&](const std::string& part) {
		for (auto& m : dialogueFindKeywords(part, names))
			out.push_back(named[m.keyword]);
	};
	size_t pos = 0;
	for (;;)
	{
		size_t at = text.find('@', pos);
		size_t end = at == std::string::npos ? at : text.find('#', at);
		if (end == std::string::npos)
		{
			if (pos < text.size())
				scan(text.substr(pos));
			break;
		}
		if (at != pos)
			scan(text.substr(pos, at - pos));
		std::string link = lower(text.substr(at + 1, end - at - 1));
		while (!link.empty() && link.back() == '\x7f')
			link.pop_back();                      // (a link's pseudo asterisks)
		for (const Topic* t : named)
			if (t->lower == link)
			{
				out.push_back(t);
				break;
			}
		pos = end + 1;
	}
	return out;
}

static void learnTopics(World& w, const std::string& text, int speaker)
{
	dialogueUpdateGlobals(w);
	for (const Topic* named : topicsNamedIn(w, text))
	{
		const Topic& t = *named;
		if (t.type != TOPIC_TOPIC || w.knownTopics.count(t.lower))
			continue;
		if (dialogueFindInfo(w, t, speaker, -1, false, true))
		{
			w.knownTopics.insert(t.lower);
			logf("dialogue: learned %s", t.name.c_str());
		}
		else
			s_namedUnlearned[t.lower] = w.refs[speaker].id;
	}
}

std::string dialogueNamedBy(const std::string& topicLower)
{
	auto it = s_namedUnlearned.find(topicLower);
	return it == s_namedUnlearned.end() ? "" : it->second;
}

// header: the title shown above the line; logTopic: the topic whose Topics index entry it joins ("" for none:
// greetings, persuasion, refusals, and what Info Refusal answers)
// learnFirst: the topics it names are learned before its script runs (a choice's answer: OpenMW's questionAnswered)
static void respond(Dialogue& d, World& w, ScriptHost& host, const Info& info, const std::string& header,
	const std::string& logTopic, bool learnFirst = false)
{
	std::string text = dialogueSubstitute(w, info.text, d.ref);
	logf("dialogue: [%s] %.90s", header.c_str(), text.c_str());
	d.history.push_back({ header, text });
	// The journal's Topics index keeps what was said about a topic (not greetings), once each
	if (!logTopic.empty() && !text.empty())
	{
		const Ref& sp = w.refs[d.ref];
		std::string who = sp.actor >= 0 ? w.game.actors[sp.actor].name : sp.id;
		auto& log = w.topicLog[logTopic];
		std::string line = who + ": " + text;
		bool have = false;
		for (auto& l : log)
			have |= l == line;
		if (!have)
			log.push_back(line);
		if (log.size() > 12)
			log.erase(log.begin());
	}
	d.choices.clear();
	if (!info.sound.empty())
		host.say(d.ref, info.sound, "");
	w.currentInfo = info.uid;
	w.currentTopic = logTopic;
	if (learnFirst)
		learnTopics(w, info.text, d.ref);
	scriptRunSnippet(w, host, info.script, d.ref);
	w.currentInfo = -1;
	// Topics named in the line, after its result script (which may set the stage that gives the speaker an
	// answer) and from the line as written, before substitution (OpenMW's order)
	if (!learnFirst)
		learnTopics(w, info.text, d.ref);
	dialogueRefreshTopics(d, w);
	d.revision++;
}

void dialogueRefreshTopics(Dialogue& d, World& w)
{
	dialogueUpdateGlobals(w);
	d.topics.clear();
	for (auto& t : w.game.topics)
	{
		if (t.type != TOPIC_TOPIC || !w.knownTopics.count(t.lower))
			continue;
		if (dialogueFindInfo(w, t, d.ref, -1, false, true))
			d.topics.push_back(t.name);
	}
	// Alphabetical, ignoring case (OpenMW's list)
	std::sort(d.topics.begin(), d.topics.end(), [](const std::string& a, const std::string& b) { return lower(a) < lower(b); });
	d.revision++;
}

// The first Greeting topic with a response that fits (no Info Refusal fallback)
static const Info* greetingInfo(World& w, int ref, const Topic** from)
{
	dialogueUpdateGlobals(w);
	for (int i = 0; i <= 9; i++)
	{
		char name[16];
		snprintf(name, sizeof(name), "Greeting %d", i);
		const Topic* t = w.game.topic(name);
		if (const Info* info = t ? dialogueFindInfo(w, *t, ref, -1) : nullptr)
		{
			*from = t;
			return info;
		}
	}
	return nullptr;
}

std::string dialogueGreetingText(World& w, int ref)
{
	const Topic* t = nullptr;
	const Info* info = greetingInfo(w, ref, &t);
	return info ? dialogueSubstitute(w, info->text, ref) : "";
}

std::string dialogueGreetingTopic(World& w, int ref)
{
	const Topic* t = nullptr;
	return greetingInfo(w, ref, &t) ? t->name : "";
}

const Info* dialogueVoiced(World& w, const Topic& topic, int ref)
{
	// Choice 0, not -1; the live talked-to flag (not the one frozen for a talk)
	int keep = s_talkRef;
	s_talkRef = -1;
	const Info* info = dialogueFindInfo(w, topic, ref, 0);
	s_talkRef = keep;
	return info;
}

int dialogueTopicFlags(World& w, const Dialogue& d, const Topic& topic)
{
	const Info* info = dialogueFindInfo(w, topic, d.ref, -1, false, true);
	if (!info)
		return 0;
	// Exhausted: this response was heard already (the journal's Topics history; an Info Refusal one never is)
	bool exhausted = false;
	if (inTopic(topic, info))
	{
		const Ref& sp = w.refs[d.ref];
		std::string line = (sp.actor >= 0 ? w.game.actors[sp.actor].name : sp.id) + ": " + dialogueSubstitute(w, info->text, d.ref);
		auto log = w.topicLog.find(topic.name);
		if (log != w.topicLog.end())
			for (auto& l : log->second)
				exhausted |= l == line;
	}
	if (!exhausted)
		return !info->whoId.empty() || !info->actors.empty() ? 2 : 0;       // specific: it names this speaker
	// A response that leads to a topic the player doesn't know yet (and the speaker can answer) is not exhausted
	for (const Topic* n : topicsNamedIn(w, info->text))
		if (n->type == TOPIC_TOPIC && !w.knownTopics.count(n->lower) && dialogueFindInfo(w, *n, d.ref, -1, false, true))
			return 0;
	return 1;
}

bool dialogueStart(Dialogue& d, World& w, ScriptHost& host, int ref)
{
	d = Dialogue();
	d.ref = ref;
	// Not with the dead
	if (w.refs[ref].dead)
		return false;
	s_talkRef = ref;
	s_talked = w.refs[ref].talkedToPC;
	dialogueUpdateGlobals(w);
	// Greetings are tried in order Greeting 0 .. Greeting 9; with none that fits there is no conversation
	for (int i = 0; i <= 9; i++)
	{
		char name[16];
		snprintf(name, sizeof(name), "Greeting %d", i);
		const Topic* t = w.game.topic(name);
		if (!t)
			continue;
		if (const Info* info = dialogueFindInfo(w, *t, ref, -1))
		{
			d.open = true;
			w.restockGold(ref);                  // (a merchant's purse, a day after the last reset)
			d.lastTopic = t;
			w.refs[ref].talkedToPC = true;
			respond(d, w, host, *info, "", "");
			return true;
		}
	}
	s_talkRef = -1;
	return false;
}

void dialogueTopic(Dialogue& d, World& w, ScriptHost& host, const std::string& topic)
{
	// Not while a choice is open, and only a Topic-type dialogue (OpenMW's keywordSelected)
	if (!d.choices.empty())
		return;
	const Topic* t = w.game.topic(topic);
	if (!t || t->type != TOPIC_TOPIC)
		return;
	const Info* info = dialogueFindInfo(w, *t, d.ref, -1, false, true);
	if (!info)
		return;
	d.lastTopic = t;
	respond(d, w, host, *info, t->name, inTopic(*t, info) ? t->name : "");
}

// Persuasion and refusals show under OpenMW's titles (sAdmireSuccess ..., sServiceRefusal); a persuasion reply is
// the topic a Choice in it is answered against
bool dialogueReact(Dialogue& d, World& w, ScriptHost& host, const std::string& topic, int choice)
{
	const Topic* t = w.game.topic(topic);
	const Info* info = t ? dialogueFindInfo(w, *t, d.ref, choice, topic == "Service Refusal") : nullptr;
	if (!info)
		return false;
	std::string title;
	if (topic == "Service Refusal")
		title = w.game.gmst("sservicerefusal", "Service Refusal");
	else
	{
		std::string key = "s";
		for (char c : t->name)
			if (c != ' ')
				key += (char)tolower((unsigned char)c);
		title = w.game.gmst(key.c_str(), t->name.c_str());
		d.lastTopic = t;
	}
	respond(d, w, host, *info, title, "");
	return true;
}

void dialogueChoose(Dialogue& d, World& w, ScriptHost& host, int value)
{
	d.choices.clear();
	if (!d.lastTopic)
		return;
	// Only a topic or a greeting is answered again (a persuasion's reply is not)
	if (d.lastTopic->type != TOPIC_TOPIC && d.lastTopic->type != TOPIC_GREETING)
	{
		d.revision++;
		return;
	}
	// The answer joins the Topics index of the topic it answers; what it names is learned before its script runs
	if (const Info* info = dialogueFindInfo(w, *d.lastTopic, d.ref, value, false, true))
		respond(d, w, host, *info, "", d.lastTopic->type == TOPIC_TOPIC && inTopic(*d.lastTopic, info) ? d.lastTopic->name : "", true);
	d.revision++;
}

// OpenMW's updateOriginalDisposition: a script changed their disposition, so what we remembered is void
static void dispositionChanged(Dialogue& d, const Ref& r)
{
	if (r.disposition != d.curDisp)
		d.curDisp = d.origDisp = r.disposition;
}

// A persuasion's result (OpenMW's DialogueManager::persuade): the temporary part moves their disposition now, the
// permanent part is what stays after Goodbye
void dialoguePersuaded(Dialogue& d, World& w, int temp, int perm)
{
	Ref& r = w.refs[d.ref];
	dispositionChanged(d, r);
	if (temp > 0 && perm > 0 && d.origDisp + perm + d.permDisp < 0)
		perm = -(d.origDisp + d.permDisp);
	d.curDisp += temp;
	r.disposition = d.curDisp;
	d.permDisp += perm;
}

void dialogueClose(Dialogue& d, World& w)
{
	// Goodbye: only the permanent part of persuasion's changes stays (OpenMW's goodbyeSelected)
	if (d.ref >= 0 && d.ref < (int)w.refs.size() && (d.permDisp || d.origDisp != d.curDisp))
	{
		Ref& r = w.refs[d.ref];
		if (r.actor >= 0 && !w.game.actors[r.actor].creature)
		{
			dispositionChanged(d, r);
			r.disposition = 0;
			int zero = w.disposition(d.ref, false);
			r.disposition = std::max(-zero, std::min(100 - zero, d.origDisp + d.permDisp));
		}
	}
	d.origDisp = d.curDisp = d.permDisp = 0;
	s_talkRef = -1;
	d.open = false;
	d.choices.clear();
	d.revision++;
}
