#pragma once

// Dialogue topics and journals (OpenMW's ESM::Dialogue)
#include <3ds/types.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include "components/esm3/loadinfo.hpp"

enum TopicType { TOPIC_TOPIC = 0, TOPIC_VOICE = 1, TOPIC_GREETING = 2, TOPIC_PERSUASION = 3 };

struct Topic
{
	std::string name, lower;
	int type;
	// Its responses. Outside a conversation only the Voice topics' are in the heap: the others are on the card
	// (spilled) and read back while someone talks (mwdialogue/topicstore.cpp: topicInfos)
	mutable std::vector<Info> infos;
	bool spilled = false;
	u32 spillOff = 0, spillLen = 0, spillCount = 0;
};

struct JournalEntry { int index; CardText text; bool finished; bool restart = false; };   // (text: read from the card when shown)
struct Journal { std::string name, title; std::vector<JournalEntry> entries; };
