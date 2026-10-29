#include "mwdialogue/dialoguemanagerimp.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <cstring>

#include "components/debug/debuglog.hpp"
#include "components/interpreter/interpreter.hpp"
#include "extern/cjson/cJSON.h"
#include "mwworld/worldimp.hpp"
#include "mwdialogue/dialoguehelpers.hpp"

// Whether the speaker had talked to the player when this conversation began (OpenMW caches it for the whole
// talk, so "Talked to PC = 0" lines still fit after the greeting); s_talkRef: whose, -1 when no talk is open
int s_talkRef = -1;
bool s_talked = false;

int g_dialogueClothValue = -1;

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
		{ "%pcclass", w.playerClass() ? w.playerClass()->name : className(w, w.stats.cls) },     // (a made class has no record)
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
			names.push_back(lower(w.game.topicKeyword(t.name)));
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
		std::string link = explicitTopic(w.game, text.substr(at + 1, end - at - 1), nullptr);
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
		std::string who = sp.actor >= 0 ? w.game.actors[sp.actor].name.str() : sp.id.str();
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
	// (its result script from the card, compiled now: Info::script)
	Node script;
	if (!info.script.empty())
		if (cJSON* json = cJSON_Parse(info.script.str().c_str()))
		{
			scriptBlockFromJson(json, script, nullptr);
			cJSON_Delete(json);
		}
	scriptRunSnippet(w, host, script, d.ref);
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
		std::string line = (sp.actor >= 0 ? w.game.actors[sp.actor].name.str() : sp.id.str()) + ": " + dialogueSubstitute(w, info->text, d.ref);
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
