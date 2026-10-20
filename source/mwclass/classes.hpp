#pragma once

// The rules each kind of object has of its own, as OpenMW's MWClass::<Type> keeps them: what activating one does and
// the lines its tooltip adds. A type's file is OpenMW's (npc.cpp, container.cpp, weapon.cpp ...), the type the
// record's recType (components/esm/defs.hpp). Checks that only ask what something is stay where they are, as in
// OpenMW (ptr.getType() == ESM::REC_...)

struct Session;
struct Object;
struct InventoryItem;

namespace MWClass
{
	// Activating a reference by its type (OpenMW's Class::activate), once Session::activate has let it through
	void activate(Session& s, int ref);
	// The tooltip's lines for an item's type (OpenMW's Class::getToolTipInfo): its stats, above weight and value; what
	// it holds or is, below them
	void toolTipStats(Session& s, const Object& o, const InventoryItem* it);
	void toolTipContents(Session& s, const Object& o);
	// "Condition: now/full" for what wears (weapons, armor)
	void conditionTip(Session& s, const Object& o, const InventoryItem* it);

	namespace Actor
	{
		void search(Session& s, int ref);              // its body or its pockets in the container screen
		void talk(Session& s, int ref);                // face its head and start the conversation
	}
	namespace Npc { void activate(Session& s, int ref); }
	namespace Creature { void activate(Session& s, int ref); }
	namespace Container { void activate(Session& s, int ref); }
	namespace Light { void activate(Session& s, int ref); }
	namespace Book
	{
		void activate(Session& s, int ref);
		void toolTipContents(Session& s, const Object& o);
	}
	namespace Weapon { void toolTipStats(Session& s, const Object& o, const InventoryItem* it); }
	namespace Armor { void toolTipStats(Session& s, const Object& o, const InventoryItem* it); }
	namespace Lockpick { void toolTipStats(Session& s, const Object& o, const InventoryItem* it); }   // probes and repair tools too
	namespace Apparatus { void toolTipStats(Session& s, const Object& o); }
	namespace Potion { void toolTipContents(Session& s, const Object& o); }
	namespace Ingredient { void toolTipContents(Session& s, const Object& o); }
}
