// EXPECT checks: compare a value with the case's, and the value parts (expect*.cpp) in order
#include <optional>
#include <3ds.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <queue>
#include <tuple>
#include "testdrive/testdrive.hpp"
#include "platform/gamma.hpp"
#include "mwbase/session.hpp"
#include "components/debug/debuglog.hpp"
#include "mwsound/soundmanagerimp.hpp"
#include "platform/console.hpp"

extern const char* g_playerBlock;
#include "mwgui/windowmanagerimp.hpp"
#include "mwphysics/physicssystem.hpp"
#include "platform/linear.hpp"
#include "platform/memstat.hpp"
#include "components/files/zfile.hpp"
#include <malloc.h>
#include "testdrive/testhelpers.hpp"
#include "testdrive/expecthelpers.hpp"

bool numberOf(Session& s, const std::string& what, const std::string& arg, float& v)
{
	for (auto part : { numberOfStats, numberOfVisuals, numberOfRules, numberOfInput, numberOfRefs, numberOfItems, numberOfWorld, numberOfState })
		if (std::optional<bool> r = part(s, what, arg, v))
			return *r;
	return false;
}

std::map<std::string, float> s_snap;

// EXPECT:journal:<quest>:<op>:<n>   EXPECT:dead:<id>   EXPECT:alive:<id>   EXPECT:item:<id>:<op>:<n>
// EXPECT:global:<name>:<op>:<v>     EXPECT:rank:<faction>:<op>:<n>   EXPECT:cell:<name start>   EXPECT:talking
// EXPECT:spell:<id> (the player has it: spells, abilities, diseases)
// (op: ge gt eq ne lt le; underscores for spaces)
bool TestDriver::expect(Session& s, const std::string& spec, bool quiet)
{
	std::vector<std::string> a = split(spec);
	World& w = s.w;
	bool ok = false;
	char got[96] = "";
	const std::string& what = a[0];
	if (what == "journal" && a.size() >= 4)
	{
		int v = w.getJournal(a[1]);
		ok = compare((float)v, a[2], (float)atof(a[3].c_str()));
		snprintf(got, sizeof(got), "%d", v);
	}
	else if ((what == "dead" || what == "alive") && a.size() >= 2)
	{
		int ri = s.testFindRef(a[1]);
		if (ri < 0)
			ri = w.findRefAnywhere(spaced(a[1]));
		bool dead = ri >= 0 && w.refs[ri].dead;
		if (what == "dead" && ri >= 0 && !dead)
		{
			// testFindRef prefers a living one; another with the id may be the one that was killed
			const std::string want = w.refs[ri].idLower;
			w.forLoadedRefs([&](int i) {
				if (w.active(i) && w.refs[i].idLower == want && w.refs[i].dead)
					dead = true;
			});
		}
		ok = ri >= 0 && dead == (what == "dead");
		snprintf(got, sizeof(got), "%s", ri < 0 ? "no such ref" : dead ? "dead" : "alive");
	}
	else if (what == "item" && a.size() >= 4)
	{
		int n = std::max(w.itemCount(a[1]), w.itemCount(spaced(a[1])));
		ok = compare((float)n, a[2], (float)atof(a[3].c_str()));
		snprintf(got, sizeof(got), "%d", n);
	}
	else if (what == "global" && a.size() >= 4)
	{
		auto g = w.globals.find(lower(a[1]));
		float v = g == w.globals.end() ? 0.0f : g->second;
		ok = compare(v, a[2], (float)atof(a[3].c_str()));
		snprintf(got, sizeof(got), "%g", v);
	}
	else if (what == "rank" && a.size() >= 4)
	{
		int r = w.pcRankIn(lower(spaced(a[1])));
		ok = compare((float)r, a[2], (float)atof(a[3].c_str()));
		snprintf(got, sizeof(got), "%d", r);
	}
	else if ((what == "cell" || what == "notcell") && a.size() >= 2)
	{
		ok = cellNameMatches(w.cellName(), spaced(a[1])) == (what == "cell");
		snprintf(got, sizeof(got), "%.80s", w.cellName().c_str());
	}
	// EXPECT:spell:<id>: the player knows it (a disease: has it); nospell: doesn't
	else if ((what == "spell" || what == "nospell") && a.size() >= 2)
	{
		std::string want = lower(spaced(a[1]));
		bool has = false;
		for (auto& sp : w.stats.spells)
			has |= lower(sp) == want || lower(sp) == lower(a[1]);
		ok = has == (what == "spell");
		snprintf(got, sizeof(got), "%s", has ? "known" : "not known");
	}
	// EXPECT:selspell:<id>: that spell is the one ready to cast
	else if (what == "selspell" && a.size() >= 2)
	{
		ok = w.stats.selectedSpell == lower(spaced(a[1]));
		snprintf(got, sizeof(got), "%.80s", w.stats.selectedSpell.c_str());
	}
	else if ((what == "button" || what == "nobutton") && a.size() >= 2)
	{
		// EXPECT:button:<label> / nobutton:<label>: the screen shows (or doesn't) a button so labelled (% for spaces)
		std::string want = lower(spaced(a[1])), all;
		bool found = false;
		for (const std::string& l : uiLastButtons())
		{
			found |= lower(l) == want;
			all += (all.empty() ? "" : ", ") + l;
		}
		ok = what == "button" ? found : !found;
		snprintf(got, sizeof(got), "%.80s", all.empty() ? "no buttons" : all.c_str());
	}
	else if (what == "vmgroup" && a.size() >= 2)
	{
		// EXPECT:vmgroup:<group>: the first-person arms play that animation group now (PickProbe, 1hChop ...)
		const char* g = "";
		if (s.vm.ready)
		{
			const Skeleton& sk = s.vm.set.skeletons[0];
			int k = s.vm.set.actors[0].group;
			g = k >= 0 && k < (int)sk.groups.size() ? sk.groups[k].name : "";
		}
		ok = lower(g) == lower(a[1]);
		snprintf(got, sizeof(got), "%s", *g ? g : "none");
	}
	else if (what == "following" && a.size() >= 2)
	{
		// EXPECT:following:<npc>: their follow (or escort) package has started: they saw us and come along (a scripted
		// follower starts only on seeing the player near; what an UNTIL waits for before an ESCORT walk)
		int ri = s.testFindRef(spaced(a[1]));
		if (ri < 0)
			ri = s.testFindRef(a[1]);
		ok = ri >= 0 && w.refs[ri].aiActive && (w.refs[ri].aiPackage == AIPKG_FOLLOW || w.refs[ri].aiPackage == AIPKG_ESCORT);
		snprintf(got, sizeof(got), "%s", ri < 0 ? "no such ref" : ok ? "following" : "not yet");
	}
	else if (what == "talkready")
	{
		// EXPECT:talkready: a topic or a choice has had its answer: the talk open with a line said, or closed by it
		// (a Goodbye); what an UNTIL after a TOPIC / CHOICE waits for instead of a fixed second
		ok = !s.dlg.open || !s.dlg.history.empty();
		snprintf(got, sizeof(got), "%s", !s.dlg.open ? "talk closed" : ok ? "answered" : "nothing said yet");
	}
	else if (what == "talking")
	{
		ok = s.dlg.open;
		snprintf(got, sizeof(got), "%s", ok ? "talking" : "not talking");
	}
	else if (what == "soul" && a.size() >= 3)
	{
		// EXPECT:soul:<gem>:<creature>: carries that gem holding that creature's soul ("none": an empty one)
		std::string want = a[2] == "none" ? "" : lower(spaced(a[2]));
		ok = false;
		std::string souls;
		for (auto& it : w.inventory)
			if (lower(it.id) == lower(a[1]))
			{
				ok |= lower(it.soul) == want || lower(it.soul) == lower(a[2]);
				souls += (souls.empty() ? "" : ",") + (it.soul.empty() ? std::string("none") : it.soul);
			}
		snprintf(got, sizeof(got), "%.80s", souls.empty() ? "no such gem" : souls.c_str());
	}
	else if (what == "madeeffect" && a.size() >= 2)
	{
		// EXPECT:madeeffect:<effect>[:<cast type>]: the last item made carries an enchantment with it
		const InventoryItem* it = lastMade(w);
		const SpellDef* en = it ? w.enchantmentOf(*it) : nullptr;
		ok = false;
		if (en)
			for (auto& e : en->effects)
				ok |= e.effect == atoi(a[1].c_str());
		if (en && a.size() >= 3)
			ok &= en->type == ENCH_ONCE + atoi(a[2].c_str());
		snprintf(got, sizeof(got), "%s", !it ? "no made item carried" : !en ? "no enchantment"
			: (std::to_string(en->effects.size()) + " effects, type " + std::to_string(en->type - ENCH_ONCE)).c_str());
	}
	else if (what == "tipline" && a.size() >= 2)
	{
		// EXPECT:tipline:<text>: the top screen's tooltip has a line holding that text (underscores for spaces)
		std::string want = lower(spaced(a[1]));
		ok = false;
		for (auto& l : s.tip)
			ok |= lower(l.first).find(want) != std::string::npos;
		snprintf(got, sizeof(got), "%.80s", s.tip.empty() ? "no tooltip" : (s.tip[0].first + (s.tip.size() > 1 ? " / " + s.tip[1].first : "")).c_str());
	}
	else if (what == "refweapon" && a.size() >= 3)
	{
		// EXPECT:refweapon:<ref>:<weapon id or "none">: the weapon that actor fights with
		int ri = findRef(s, a[1], false);
		std::string wpn = ri >= 0 && w.refs[ri].actor >= 0 ? w.game.actors[w.refs[ri].actor].weapon : "";
		ok = a[2] == "any" ? !wpn.empty() : a[2] == "none" ? wpn.empty() : wpn == lower(spaced(a[2]));
		snprintf(got, sizeof(got), "%.80s", ri < 0 ? "no such ref" : wpn.empty() ? "none" : wpn.c_str());
	}
	else if (what == "maptip" && a.size() >= 2)
	{
		// EXPECT:maptip:<cell>: the local map shows where the tapped door leads, the world map the tapped place's name
		// ("none": no marker tapped)
		auto name = [&](int id) {
			if (s.mapLocal)
				return id >= 0 && id < (int)w.refs.size() ? w.refs[id].destCell.str() : std::string();
			return id >= 0 && id < (int)w.game.map.labels.size() ? w.game.map.labels[id].text : std::string();
		};
		std::string tip = name(s.mapLocal ? s.mapDoorTip : s.mapPlaceTip);
		ok = a[1] == "none" ? tip.empty() : lower(tip) == lower(spaced(a[1]));
		snprintf(got, sizeof(got), "%.80s", tip.empty() ? "none" : tip.c_str());
	}
	else if (what == "journaltopic" && a.size() >= 2)
	{
		// EXPECT:journaltopic:<topic>: the topic open in the journal's Topics index ("none": the index itself, "any": some topic)
		ok = a[1] == "none" ? s.journalTopic.empty() : a[1] == "any" ? !s.journalTopic.empty() : lower(s.journalTopic) == lower(spaced(a[1]));
		snprintf(got, sizeof(got), "%.80s", s.journalTopic.empty() ? "none" : s.journalTopic.c_str());
	}
	else if (what == "journalsearch" && a.size() >= 2)
	{
		// EXPECT:journalsearch:<text>: what the journal's search holds ("none": nothing, all shown)
		ok = a[1] == "none" ? s.journalSearch.empty() : lower(s.journalSearch) == lower(spaced(a[1]));
		snprintf(got, sizeof(got), "%.80s", s.journalSearch.empty() ? "none" : s.journalSearch.c_str());
	}
	else if ((what == "makename" || what == "madename" || what == "madespellname") && a.size() >= 2)
	{
		// EXPECT:makename:<name>: the name in the spellmaking / enchanting screen; madename: the last enchanted
		// item carried; madespellname: the last spell made (underscores for spaces)
		std::string name;
		const InventoryItem* it = what == "madename" ? lastMade(w) : nullptr;
		const Object* o = it ? w.game.object(it->id) : nullptr;
		auto sp = what == "madespellname" && !w.madeSpells.empty() ? w.game.spells.find(w.madeSpells.back()) : w.game.spells.end();
		if (what == "makename")
			name = s.makeName;
		else if (o)
			name = o->name;
		else if (sp != w.game.spells.end())
			name = sp->second.name;
		ok = name == spaced(a[1]);
		snprintf(got, sizeof(got), "%.80s", name.empty() ? "no name" : name.c_str());
	}
	else if (what == "said" && a.size() >= 2)
	{
		// EXPECT:said:<text, underscores for spaces>: the last line said in this conversation holds it
		std::string want = lower(spaced(a[1])), line = s.dlg.history.empty() ? "" : lower(s.dlg.history.back().text);
		ok = line.find(want) != std::string::npos;
		snprintf(got, sizeof(got), "%.80s", line.empty() ? "nothing said" : line.c_str());
	}
	else if (what == "greeting" && a.size() >= 3)
	{
		// EXPECT:greeting:<npc>:<text>: the line they would greet with now (as said) holds the text
		int ri = findRef(s, a[1]);
		std::string want = lower(spaced(a[2])), line = ri >= 0 ? lower(dialogueGreetingText(w, ri)) : "";
		ok = ri >= 0 && line.find(want) != std::string::npos;
		snprintf(got, sizeof(got), "%.80s", ri < 0 ? "speaker not loaded" : line.empty() ? "no greeting" : line.c_str());
	}
	else if ((what == "topiclisted" || what == "topicnotlisted") && a.size() >= 2)
	{
		// EXPECT:topiclisted:<topic>: the open conversation offers the topic; topicnotlisted: it does not
		if (!s.dlg.open)
		{
			// TALK found the speaker disabled or elsewhere (a quest enables them, they wander): there is no list
			// to read; the answer: checks still test the filter
			logf("expect: SKIP %s (no conversation open)", spec.c_str());
			return true;
		}
		std::string want = lower(spaced(a[1]));
		bool listed = false;
		for (auto& t : s.dlg.topics)
			listed |= lower(t) == want;
		ok = listed == (what == "topiclisted");
		snprintf(got, sizeof(got), "%s", listed ? "listed" : "not listed");
	}
	else if (what == "topicflag" && a.size() >= 5)
	{
		// EXPECT:topicflag:<topic>:<exhausted|specific>:<op>:<0|1>: the flag the open talk's topic list gives it
		const Topic* t = w.game.topic(spaced(a[1]));
		int flags = t && s.dlg.open ? dialogueTopicFlags(w, s.dlg, *t) : 0;
		float v = (flags & (a[2] == "exhausted" ? 1 : 2)) ? 1.0f : 0.0f;
		ok = t && s.dlg.open && compare(v, a[3], (float)atof(a[4].c_str()));
		snprintf(got, sizeof(got), "%s", !t ? "no such topic" : !s.dlg.open ? "not talking" : v > 0.0f ? "set" : "not set");
	}
	else if (what == "learned" && a.size() >= 2)
	{
		// EXPECT:learned:<topic>: the player knows the topic (listed or not)
		ok = w.knownTopics.count(lower(spaced(a[1]))) > 0;
		snprintf(got, sizeof(got), "%s", ok ? "known" : "not known");
	}
	else if (what == "talked" && a.size() >= 2)
	{
		// EXPECT:talked:<npc>[:<op>:<0|1>]: the NPC's real talked-to-player flag (alone: it is set)
		int ri = findRef(s, a[1]);
		bool talked = ri >= 0 && w.refs[ri].talkedToPC;
		ok = ri >= 0 && (a.size() >= 4 ? compare(talked ? 1.0f : 0.0f, a[2], (float)atof(a[3].c_str())) : talked);
		snprintf(got, sizeof(got), "%s", ri < 0 ? "speaker not loaded" : talked ? "talked" : "not talked");
	}
	else if (what == "greetingtopic" && a.size() >= 3)
	{
		// EXPECT:greetingtopic:<npc>:[<eq|ne>:]<topic>: the Greeting topic that wins for them now
		int ri = findRef(s, a[1]);
		bool op = a.size() >= 4 && (a[2] == "eq" || a[2] == "ne");
		std::string want = lower(spaced(a[op ? 3 : 2])), topic = ri >= 0 ? lower(dialogueGreetingTopic(w, ri)) : "";
		ok = ri >= 0 && (topic == want) == (!op || a[2] == "eq");
		snprintf(got, sizeof(got), "%s", ri < 0 ? "speaker not loaded" : topic.empty() ? "no greeting" : topic.c_str());
	}
	else if (what == "voiced" && a.size() >= 5)
	{
		// EXPECT:voiced:<npc>:<topic>:<eq|ne>:<text>: the response a voiced reaction would pick (choice 0, no Info
		// Refusal fallback) holds the text (eq) or does not (ne); no answer holds nothing
		const Topic* t = w.game.topic(spaced(a[2]));
		int ri = findRef(s, a[1]);
		std::string want = lower(spaced(a[4])), line;
		if (t && ri >= 0)
			if (const Info* info = dialogueVoiced(w, *t, ri))
				line = lower(info->text);
		ok = t && ri >= 0 && (line.find(want) != std::string::npos) == (a[3] == "eq");
		snprintf(got, sizeof(got), "%.80s", !t ? "no such topic" : ri < 0 ? "speaker not loaded" : line.empty() ? "no answer" : line.c_str());
	}
	else if ((what == "answer" && a.size() >= 5) || (what == "answerchoice" && a.size() >= 6))
	{
		// EXPECT:answer:<topic>:<npc>:<eq|ne>:<text>: what the filter picks for that speaker (topics fall back to
		// Info Refusal as clicking does) holds the text (eq) or does not (ne); no answer holds nothing
		// EXPECT:answerchoice:<topic>:<npc>:<n>:<eq|ne>:<text>: the same with the filter's choice set to n (Service
		// Refusal: the service number; it is asked inverted and with no Info Refusal fallback)
		bool withChoice = what == "answerchoice";
		const Topic* t = w.game.topic(spaced(a[1]));
		int ri = findRef(s, a[2]);
		int choice = withChoice ? atoi(a[3].c_str()) : -1;
		const std::string& op = a[withChoice ? 4 : 3];
		std::string want = lower(spaced(a[withChoice ? 5 : 4])), line;
		if (t && ri >= 0)
		{
			bool refusal = t->name == "Service Refusal";
			if (const Info* info = dialogueFindInfo(w, *t, ri, choice, refusal, !refusal))
				line = lower(info->text);
		}
		bool has = line.find(want) != std::string::npos;
		ok = t && ri >= 0 && has == (op == "eq");
		snprintf(got, sizeof(got), "%.80s", !t ? "no such topic" : ri < 0 ? "speaker not loaded" : line.empty() ? "no answer" : line.c_str());
	}
	else if (what == "answerid" && a.size() >= 4)
	{
		// EXPECT:answerid:<topic>:<npc>:[<op>:]<n>: the 0-based position of the response the filter picks within its
		// topic (an Info Refusal one counts 1000 + its place there); -1 for no answer
		const Topic* t = w.game.topic(spaced(a[1]));
		int ri = findRef(s, a[2]);
		bool op = a.size() >= 5;
		int at = -1;
		if (t && ri >= 0)
			if (const Info* info = dialogueFindInfo(w, *t, ri, -1, t->name == "Service Refusal", t->name != "Service Refusal"))
			{
				const Topic* ref = w.game.topic("Info Refusal");
				if (!t->infos.empty() && info >= &t->infos.front() && info <= &t->infos.back())
					at = (int)(info - &t->infos.front());
				else if (ref && !ref->infos.empty() && info >= &ref->infos.front() && info <= &ref->infos.back())
					at = 1000 + (int)(info - &ref->infos.front());
			}
		ok = t && ri >= 0 && compare((float)at, op ? a[3] : "eq", (float)atof(a[op ? 4 : 3].c_str()));
		snprintf(got, sizeof(got), "%s", !t ? "no such topic" : ri < 0 ? "speaker not loaded" : std::to_string(at).c_str());
	}
	else if (what == "message" && a.size() >= 2)
	{
		// EXPECT:message:<text, underscores for spaces>: a message box or a notice on screen holds it
		std::string want = lower(spaced(a[1]));
		ok = false;
		for (auto& m : s.messages)
			ok |= lower(m.text).find(want) != std::string::npos;
		for (auto& nt : s.notes)
			ok |= lower(nt.text).find(want) != std::string::npos;
		snprintf(got, sizeof(got), "%s", ok ? "shown" : "not shown");
	}
	else if (what == "screen" && a.size() >= 2)
	{
		ok = (int)s.screen == screenByName(a[1]);
		snprintf(got, sizeof(got), "screen %d", (int)s.screen);
	}
	else if ((what == "doorpick" || what == "doorpickclosed") && a.size() >= 2)
	{
		// EXPECT:doorpick:<door>: the crosshair, aimed from the player's eyes at the middle of the door as it is drawn now
		// (turned about its origin by its opening angle), picks that door; doorpickclosed: aimed at where it stood closed,
		// it does not (an open door is activated where it is, not where it was)
		int ri = findRef(s, a[1], false);
		if (ri < 0)
			return false;
		const Ref& r = w.refs[ri];
		float mid[3], to[3];
		for (int k = 0; k < 3; k++)
			mid[k] = (r.boxMin[k] + r.boxMax[k]) * 0.5f - r.pos[k];
		if (what == "doorpick")
			doorSwing(r, -r.doorAngle, mid, mid);       // (drawn turned by -doorAngle about its own up: mwrender/renderingmanager.cpp)
		for (int k = 0; k < 3; k++)
			to[k] = r.pos[k] + mid[k];
		float eye[3] = { w.player.feet[0], w.player.feet[1], playerEyeZ(w.player) };
		float d[3] = { to[0] - eye[0], to[1] - eye[1], to[2] - eye[2] }, len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		for (int k = 0; k < 3; k++)
			d[k] /= len > 0.0f ? len : 1.0f;
		int hit = worldPick(w, eye, d, 1000.0f);
		ok = what == "doorpick" ? hit == ri : hit != ri;
		snprintf(got, sizeof(got), "door angle %.2f, picked %s", r.doorAngle, hit < 0 ? "nothing" : hit == ri ? "the door" : w.refs[hit].id.c_str());
	}
	else if ((what == "refgroup" || what == "refhasgroup") && a.size() >= 3)
	{
		// EXPECT:refgroup:<ref>:<group>: that actor plays a group whose name starts with it;
		// EXPECT:refhasgroup:<ref>:<group>: its skeleton has that group (the converted data carries it)
		int ri = findRef(s, a[1]);
		const Actor* ac = ri >= 0 ? w.actorOf(ri) : nullptr;
		const Skeleton* sk = ac ? &actorSkeleton(*w.actorsOf(ri), ac->skeleton) : nullptr;
		const char* g = sk && ac->group >= 0 && ac->group < (int)sk->groups.size() ? sk->groups[ac->group].name : "";
		if (what == "refgroup")
			ok = g[0] && strncmp(g, a[2].c_str(), a[2].size()) == 0;
		else
			ok = sk && actorFindGroup(*sk, a[2].c_str()) >= 0;
		snprintf(got, sizeof(got), "%s", !ac ? "not loaded" : what == "refgroup" ? (g[0] ? g : "no group") : ok ? "has it" : "hasn't");
	}
	else if (what == "bodygroup" && a.size() >= 2)
	{
		// EXPECT:bodygroup:<group>: in third person, the player's body plays a group whose name starts with it
		// ("Attack1h" also takes its follow-throughs "Attack1hM" / "Attack1hS")
		const char* g = s.thirdPerson ? s.body.playing() : "";
		ok = g[0] && strncmp(g, a[1].c_str(), a[1].size()) == 0;
		snprintf(got, sizeof(got), "%s", !s.thirdPerson ? "first person" : g[0] ? g : "no group");
	}
	else if (what == "bodyloop" && a.size() >= 2)
	{
		// EXPECT:bodyloop:<group>: in third person the body plays that group inside its loop, Loop Start to Loop Stop (in
		// the air the jump goes round there; its landing, Loop Stop to Stop, waits for the ground: OpenMW's jump states)
		const char* g = s.thirdPerson ? s.body.playing() : "";
		float t = 0.0f, t0 = 0.0f, t1 = 0.0f;
		bool have = g[0] && strncmp(g, a[1].c_str(), a[1].size()) == 0 && s.body.loopTimes(t, t0, t1);
		ok = have && t >= t0 - 0.001f && t <= t1 + 0.001f;     // (equal keys: held at that frame)
		if (have)
			snprintf(got, sizeof(got), "%s at %.3f, loop %.3f to %.3f", g, t, t0, t1);
		else
			snprintf(got, sizeof(got), "%s", !s.thirdPerson ? "first person" : g[0] ? g : "no group");
	}
	else if (what == "bodyweapon" && a.size() >= 2)
	{
		// EXPECT:bodyweapon:<piece>: in third person, the player's body holds that piece on its weapon bone
		// (w_iron_longsword_25); EXPECT:bodyweapon:none: nothing
		const std::string& p = s.body.weapon;
		ok = s.thirdPerson && (a[1] == "none" ? p.empty() : p == a[1]);
		snprintf(got, sizeof(got), "%s", !s.thirdPerson ? "first person" : p.empty() ? "none" : p.c_str());
	}
	else
	{
		bool arg = hasArg(what);
		std::string key = what + ":" + (arg && a.size() >= 2 ? a[1] : "");
		size_t op = arg ? 2 : 1;
		float v = 0.0f;
		bool known = a.size() > op + 1 && numberOf(s, what, arg ? a[1] : "", v);
		bool snapped = true;
		float target = known ? wanted(a[op + 1], key, snapped) : 0.0f;
		ok = known && snapped && compare(v, a[op], target);
		if (!known)
			snprintf(got, sizeof(got), "unknown check");
		else if (!snapped)
			snprintf(got, sizeof(got), "%g, nothing snapped for %s", v, key.c_str());
		else
			snprintf(got, sizeof(got), "%g, wanted %s %g", v, a[op].c_str(), target);
	}
	if (!quiet)
		logf("expect: %s %s (got %s)", ok ? "PASS" : "FAIL", spec.c_str(), got);
	if (!quiet && !ok)
		g_testFailed = true;
	return ok;
}
