// Text typed on the system keyboard: names of made spells and items, the journal search, a barter offer
#include <algorithm>
#include <array>
#include <functional>
#include <sys/stat.h>
#include <tex3ds.h>
#include <citro2d.h>
#include "mwbase/session.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "components/debug/debuglog.hpp"
#include <malloc.h>

// What was typed on the system keyboard: the name of the spell / enchanted item being made (a cancel keeps the
// name), or the journal's search (empty: everything again)
void Session::setText(bool confirmed, const std::string& text)
{
	int kind = wantText;
	wantText = TEXT_NONE;
	if (!confirmed)
		return;
	bool blank = text.find_first_not_of(' ') == std::string::npos;
	if (kind == TEXT_MAKENAME && !blank)
	{
		makeName = text;
		logf("making: name %s", makeName.c_str());
	}
	else if (kind == TEXT_CONSOLE && !blank)
	{
		consoleBack = 0.0f;
		console.run(w, *this, text);
	}
	else if (kind == TEXT_SEARCH)
	{
		journalSearch = blank ? "" : text;
		list2 = UiList();
		logf("journal: search \"%s\"", journalSearch.c_str());
	}
	// the class being made: its name can't be left blank (OpenMW's OK waits for one), its description can
	else if (kind == TEXT_CLASSNAME && !blank)
	{
		classDraft.name = text;
		logf("chargen: class name %s", text.c_str());
	}
	else if (kind == TEXT_CLASSDESC)
	{
		classDraft.desc = blank ? "" : text;
		logf("chargen: class description \"%s\"", classDraft.desc.c_str());
	}
	// the gold offered in a barter, typed on the number pad (OpenMW's balance field takes any number)
	else if (kind == TEXT_OFFER && !blank && screen == SCR_BARTER)
	{
		haggleOffer = std::max(0, atoi(text.c_str()));
		logf("barter: offer typed %d", haggleOffer);
	}
}

std::string Session::textNow()
{
	if (wantText == TEXT_CLASSNAME || wantText == TEXT_CLASSDESC)
		return wantText == TEXT_CLASSNAME ? classDraft.name : classDraft.desc;
	if (wantText == TEXT_OFFER)
		return std::to_string(barterHaggled(abs(barterBalance())));
	return wantText == TEXT_SEARCH ? journalSearch : wantText == TEXT_CONSOLE ? consoleDraft : makeName;
}
