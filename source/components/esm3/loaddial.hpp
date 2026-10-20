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
	std::vector<Info> infos;
};

struct JournalEntry { int index; CardText text; bool finished; bool restart = false; };   // (text: read from the card when shown)
struct Journal { std::string name, title; std::vector<JournalEntry> entries; };
