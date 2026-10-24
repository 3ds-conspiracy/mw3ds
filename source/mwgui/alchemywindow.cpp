// The alchemy screen a mortar and pestle opens
#include <cmath>
#include <functional>

#include "mwgui/windowmanagerimp.hpp"
#include "mwbase/session.hpp"
#include "mwmechanics/alchemyhelpers.hpp"

// Ingredients on the left, the (up to four) chosen on the right with the effects their potion will
// have (those the player's Alchemy knows: 1 + Alchemy / 15 effects of each ingredient), Create / Close
void Session::drawAlchemy()
{
	header(w.game.gmst("salchemy", "Alchemy"));
	float q[4];
	bestApparatus(w, q);
	s_alchemyWorld = &w;
	std::vector<std::string> ids, rows;
	for (auto& it : w.inventory)
		if (const Object* o = w.game.object(it.id))
			if (o->recType == REC_INGR)
			{
				int used = 0;
				for (auto& s : alchemySlots)
					used += s == it.id;
				if (it.count > used)
				{
					ids.push_back(it.id);
					rows.push_back(o->name + (it.count - used > 1 ? " (" + std::to_string(it.count - used) + ")" : ""));
				}
			}
	if (rows.empty())
		uiTextCentered(80, 100, 0.45f, col::textDim, "No ingredients");
	int pick = uiList(list2, 4, 24, 150, 180, rows, 0.42f, true);
	if (pick >= 0 && pick < (int)ids.size() && alchemySlots.size() < 4)
	{
		bool dup = false;
		for (auto& s : alchemySlots)
			dup |= s == ids[pick];
		if (!dup)
		{
			alchemySlots.push_back(ids[pick]);
			playSound(-1, "Item Ingredient Up");
		}
	}
	// The chosen ones: tap to put back
	for (size_t k = 0; k < 4; k++)
	{
		const Object* o = k < alchemySlots.size() ? w.game.object(alchemySlots[k]) : nullptr;
		if (uiButton(160, 24 + k * 22, 156, 20, o ? o->name : "-", false, o != nullptr) && o)
		{
			alchemySlots.erase(alchemySlots.begin() + k);
			playSound(-1, "Menu Click");
			break;
		}
	}
	float y = 116;
	size_t index = 0;
	for (auto& e : commonEffects(w, alchemySlots))
	{
		bool seen = alchemyEffectKnown(index++);      // (the potion-to-be's effects in order, as OpenMW's window)
		uiText(162, y, 0.4f, seen ? col::text : col::textDim, seen ? effectLabel(e.effect, e.skill, e.attribute) : "?");
		y += 14;
	}
	char tools[96];
	snprintf(tools, sizeof(tools), "Mortar %.1f  Alembic %.1f", q[0], q[1]);
	uiText(162, 172, 0.36f, col::textDim, tools);
	snprintf(tools, sizeof(tools), "Calcinator %.1f  Retort %.1f", q[2], q[3]);
	uiText(162, 186, 0.36f, col::textDim, tools);
	int noFocus = -1;
	int b = buttonRow({ w.game.gmst("screate", "Create"), w.game.gmst("sclose", "Close") }, noFocus);
	if (b == 0 && alchemySlots.size() >= 2)
		brewPotion();
	else if (b == 1 || (uiIn().down & KEY_B))
	{
		playSound(-1, "Menu Click");
		alchemySlots.clear();
		screen = SCR_INVENTORY;
	}
}
