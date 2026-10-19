// Picking things up
#include <algorithm>
#include <array>
#include <functional>
#include <sys/stat.h>
#include <tex3ds.h>
#include <citro2d.h>
#include "mwrender/objectpaging.hpp"
#include "mwrender/animation.hpp"
#include "mwbase/session.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "mwsound/soundmanagerimp.hpp"
#include "platform/gamma.hpp"
#include "platform/linear.hpp"
#include "components/debug/debuglog.hpp"
#include "platform/memstat.hpp"
#include <malloc.h>
#include "mwbase/sessionhelpers.hpp"

void Session::pickUp(int ref)
{
	Ref& r = w.refs[ref];
	const std::string& t = r.type;
	const char* sound = "Item Misc Up";
	if (t == "BOOK") sound = "Item Book Up";
	else if (t == "ALCH") sound = "Item Potion Up";
	else if (t == "INGR") sound = "Item Ingredient Up";
	else if (t == "WEAP") sound = "Item Weapon Shortblade Up";
	else if (t == "CLOT") sound = "Item Clothes Up";
	else if (t == "ARMO") sound = "Item Armor Medium Up";
	else if (t == "LOCK") sound = "Item Lockpick Up";
	else if (t == "PROB") sound = "Item Probe Up";
	else if (t == "REPA") sound = "Item Repair Up";
	else if (t == "APPA") sound = "Item Apparatus Up";
	else if (r.idLower.rfind("gold_", 0) == 0) sound = "Item Gold Up";
	playSound(-1, sound);
	if (w.ownedByOther(ref))
		takeOwned(ref, r.id, r.count > 0 ? r.count : 1);
	w.pickUp(ref);
}
