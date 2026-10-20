#pragma once

// A record's type as a number (OpenMW's ESM::RecNameInts): the converted data names it by its tag ("ARMO"); the
// engine compares this instead, set from the tag wherever one is (recTypeOf). REC_OTHER: any tag not listed
#include <cstring>

enum RecName : unsigned char
{
	REC_OTHER = 0,
	REC_ACTI, REC_ALCH, REC_APPA, REC_ARMO, REC_BOOK, REC_CLOT, REC_CONT, REC_CREA, REC_DOOR,
	REC_INGR, REC_LIGH, REC_LOCK, REC_MISC, REC_NPC_, REC_PROB, REC_REPA, REC_STAT, REC_WEAP,
};

inline RecName recTypeOf(const char* tag)
{
	static const char* const kTags[] = { "ACTI", "ALCH", "APPA", "ARMO", "BOOK", "CLOT", "CONT", "CREA", "DOOR", "INGR", "LIGH", "LOCK", "MISC", "NPC_", "PROB", "REPA", "STAT", "WEAP" };
	for (int i = 0; i < (int)(sizeof(kTags) / sizeof(kTags[0])); i++)
		if (strcmp(tag, kTags[i]) == 0)
			return (RecName)(REC_ACTI + i);
	return REC_OTHER;
}
