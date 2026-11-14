// The order items are listed in: the inventory, a container, a merchant's goods (OpenMW's SortFilterItemModel)
#include <algorithm>
#include <cstring>
#include "mwgui/windowmanagerimp.hpp"

// By kind: weapons, armor, clothing, potions, ingredients, apparatus, books, lights, misc, lockpicks, repair tools,
// probes (OpenMW's getTypeOrder)
static int typeOrder(RecName t)
{
	switch (t)
	{
	case REC_WEAP: return 0;
	case REC_ARMO: return 1;
	case REC_CLOT: return 2;
	case REC_ALCH: return 3;
	case REC_INGR: return 4;
	case REC_APPA: return 5;
	case REC_BOOK: return 6;
	case REC_LIGH: return 7;
	case REC_MISC: return 8;
	case REC_LOCK: return 9;
	case REC_REPA: return 10;
	case REC_PROB: return 11;
	default: return 12;
	}
}

// Its enchantment as a sort key: constant effect 101, else the charge left in percent, -1 not enchanted
static int chargeKey(const GameData& g, const ItemListed& it)
{
	if (!it.o || it.o->ench.empty())
		return -1;
	auto sp = g.spells.find(it.o->ench);
	if (sp == g.spells.end())
		return -1;
	if (sp->second.type == ENCH_CONSTANT)
		return 101;
	if (sp->second.charge <= 0 || it.charge < 0.0f)
		return 100;
	return (int)(it.charge / sp->second.charge * 100.0f);
}

// Its condition, for things that wear (weapons, armor) or are used up (lockpicks, probes, repair tools); -1 none
static int conditionKey(const ItemListed& it)
{
	if (!it.o)
		return -1;
	int full = it.o->health > 0 ? it.o->health
		: (it.o->recType == REC_LOCK || it.o->recType == REC_PROB || it.o->recType == REC_REPA) ? it.o->uses : 0;
	if (full <= 0)
		return -1;
	return it.condition >= 0 ? it.condition : full;
}

// OpenMW's Compare: the player's equipped things first, then by kind, by name, enchanted before plain (constant effect
// first, then more charge before less), and better condition first
bool listedBefore(const GameData& g, const ItemListed& a, const ItemListed& b)
{
	if (a.equipped != b.equipped)
		return a.equipped;
	int ta = a.o ? typeOrder(a.o->recType) : 12, tb = b.o ? typeOrder(b.o->recType) : 12;
	if (ta != tb)
		return ta < tb;
	if (a.o && b.o)
	{
		int byName = strcasecmp(a.o->name.c_str(), b.o->name.c_str());
		if (byName != 0)
			return byName < 0;
	}
	int ca = chargeKey(g, a), cb = chargeKey(g, b);
	if (ca != cb)
		return ca > cb;
	int ha = conditionKey(a), hb = conditionKey(b);
	if (ha >= 0 && hb >= 0 && ha != hb)
		return ha > hb;
	return false;
}

void sortListed(const GameData& g, std::vector<int>& order, const std::function<ItemListed(int)>& item)
{
	std::stable_sort(order.begin(), order.end(), [&](int x, int y) { return listedBefore(g, item(x), item(y)); });
}
