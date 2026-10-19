// A smith's repair price
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "mwbase/session.hpp"

// A smith's price for mending an item (OpenMW's formula): the missing condition over
// max condition / value, times fRepairMult, then bartered like a purchase
int Session::repairPrice(int ri, const InventoryItem& it)
{
	const Object* o = w.game.object(it.id);
	if (!o || o->health <= 0)
		return 0;
	float p = fmaxf(1.0f, (float)o->value);
	float r = fmaxf(1.0f, (float)(int)(o->health / p));
	int x = (int)((o->health - itemCondition(it)) / r);
	x = (int)(w.game.gmstf("frepairmult", 1.0f) * x);
	return barterPrice(ri, x < 1 ? 1 : x, true);
}
