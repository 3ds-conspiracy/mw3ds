#pragma once

#include <string>
#include <utility>
#include <vector>

#include "game.h"

struct World;
struct ScriptHost;

struct DialogueEntry
{
	std::string header;     // topic name, empty for greetings / choice answers
	std::string text;
};

struct Dialogue
{
	bool open = false;
	int ref = -1;                 // speaker reference
	std::vector<DialogueEntry> history;
	std::vector<std::string> topics;                         // available now, display names
	std::vector<std::pair<std::string, int>> choices;        // pending Choice options
	const Topic* lastTopic = nullptr;                        // re-evaluated when a choice is made
	bool goodbye = false;                                    // Goodbye was called: only "Goodbye" remains
	int revision = 0;                                        // bumps whenever the UI must relayout
	// Persuasion this talk (OpenMW's DialogueManager): their base disposition as the talk began, as it stands,
	// and the permanent part of the changes (what stays after Goodbye)
	int origDisp = 0, curDisp = 0, permDisp = 0;
};

// false when the speaker has no greeting that fits (or is dead): there is no conversation then
bool dialogueStart(Dialogue& d, World& w, ScriptHost& host, int ref);
void dialogueTopic(Dialogue& d, World& w, ScriptHost& host, const std::string& topic);
void dialogueChoose(Dialogue& d, World& w, ScriptHost& host, int value);
// A reaction without a heading (Admire Success, Service Refusal ...): false when none fits the speaker
// choice: what Choice conditions read (a refused service passes its number, as OpenMW does)
bool dialogueReact(Dialogue& d, World& w, ScriptHost& host, const std::string& topic, int choice = -1);
void dialogueClose(Dialogue& d, World& w);
// A persuasion's temporary and permanent disposition changes (see Dialogue::permDisp)
void dialoguePersuaded(Dialogue& d, World& w, int temp, int perm);
void dialogueRefreshTopics(Dialogue& d, World& w);
// Where keywords (lowercase) are named in text, the way OpenMW's KeywordSearch::highlightKeywords finds them
// (Morrowind's hyperlinks): a keyword starting the text or following a space, tab, line break, quote, ( or [,
// however the word goes on ("cave rat" in "cave rats"); of overlapping ones the longest. Sorted by position.
struct KeywordMatch { size_t begin, end; int keyword; };
std::vector<KeywordMatch> dialogueFindKeywords(const std::string& text, const std::vector<std::string>& keywords);
// Who last named a topic without having an answer for it ("" when nobody did): for the tests
std::string dialogueNamedBy(const std::string& topicLower);

// First response of `topic` the speaker may give now (also used for Hello voice lines); refusal: fall back to
// "Info Refusal" when only the speaker's disposition stands in the way (topics clicked, listed and learned)
const Info* dialogueFindInfo(World& w, const Topic& topic, int ref, int choice, bool invert = false, bool refusal = false);
std::string dialogueSubstitute(World& w, const std::string& text, int ref);
// OpenMW's updateDialogueGlobals (PCHasCrimeGold ...): done at the start of a talk and with each topic list
void dialogueUpdateGlobals(World& w);
// What the speaker would greet with now, as said ("" when no greeting fits): for the tests
std::string dialogueGreetingText(World& w, int ref);
// The Greeting topic that wins for the speaker now ("" when none fits): for the tests
std::string dialogueGreetingTopic(World& w, int ref);
// What a voiced reaction (Hello, Hit, Flee ...) would say: the topic searched with choice 0 and the live
// talked-to flag, no Info Refusal fallback (OpenMW's say)
const Info* dialogueVoiced(World& w, const Topic& topic, int ref);
// OpenMW's actor-known topic flags for the speaker of an open talk: 1 exhausted, 2 specific (for the tests)
int dialogueTopicFlags(World& w, const Dialogue& d, const Topic& topic);
// A test sets the Clothing Modifier by hand: -1 reads what is worn
extern int g_dialogueClothValue;
