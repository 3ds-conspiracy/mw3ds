#pragma once

// Dialogue responses and their conditions (OpenMW's ESM::DialInfo)
#include <3ds/types.h>
#include <string>
#include <unordered_map>
#include "components/esm3/loadscpt.hpp"
#include "components/misc/cardtext.hpp"

struct Cond
{
	char kind;          // '1' function, '2' global, '3' local, '4' journal, '5' item, '6' dead, 'C' not local
	int func;           // function code for kind '1'
	Atom name;          // lowercase variable / journal / item name
	char op;            // '0' ==, '1' !=, '2' >, '3' >=, '4' <, '5' <=
	float value;
};

struct Info
{
	std::vector<int> actors;          // older data: the actors that may say it
	// Who may say it (lowercase; empty / -1 = anyone): speaker id, race, class, faction ("ffff": none),
	// cell name prefix, sex, least rank in the faction
	Atom whoId, whoRace, whoClass, whoFaction, whoCell;
	int whoSex = -1, whoRank = -1;
	CardText text;                    // (read from the card when said)
	Atom sound;
	std::vector<Cond> conds;
	int disposition = 0;
	// Its result script, the converter's JSON of it on the card, compiled when the line is said: a fifth of the 20000
	// lines have one, and as trees in the heap they and the empty ones took 2 MB
	CardText script;
	int uid = -1;                     // its number in load order (ClearInfoActor remembers it)
};
