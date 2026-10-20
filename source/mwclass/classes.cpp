// Each type's rules picked by the record's type (OpenMW's Class::get(ptr), the classes registered in classes.cpp)
#include <cstdio>

#include "mwbase/session.hpp"
#include "mwclass/classes.hpp"

void MWClass::activate(Session& s, int ref)
{
	switch (s.w.refs[ref].recType)
	{
	case REC_NPC_: Npc::activate(s, ref); return;
	case REC_CREA: Creature::activate(s, ref); return;
	case REC_DOOR: s.useDoor(ref); return;
	case REC_CONT: Container::activate(s, ref); return;
	case REC_BOOK: Book::activate(s, ref); return;
	case REC_LIGH: Light::activate(s, ref); return;
	case REC_ACTI: case REC_STAT: return;
	default: s.pickUp(ref); return;
	}
}

void MWClass::toolTipStats(Session& s, const Object& o, const InventoryItem* it)
{
	switch (o.recType)
	{
	case REC_WEAP: Weapon::toolTipStats(s, o, it); return;
	case REC_ARMO: Armor::toolTipStats(s, o, it); return;
	case REC_LOCK: case REC_PROB: case REC_REPA: Lockpick::toolTipStats(s, o, it); return;
	case REC_APPA: Apparatus::toolTipStats(s, o); return;
	default: return;
	}
}

void MWClass::toolTipContents(Session& s, const Object& o)
{
	switch (o.recType)
	{
	case REC_ALCH: Potion::toolTipContents(s, o); return;
	case REC_INGR: Ingredient::toolTipContents(s, o); return;
	case REC_BOOK: Book::toolTipContents(s, o); return;
	default: return;
	}
}

void MWClass::conditionTip(Session& s, const Object& o, const InventoryItem* it)
{
	if (o.health <= 0)
		return;
	char buf[96];
	int cond = it && it->condition >= 0 ? it->condition : o.health;
	snprintf(buf, sizeof(buf), "Condition: %d/%d", cond, o.health);
	s.tip.emplace_back(buf, col::text);
}
