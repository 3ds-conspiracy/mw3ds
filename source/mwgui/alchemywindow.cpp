// The alchemy screen a mortar and pestle opens
#include <cmath>
#include <functional>

#include "mwgui/windowmanagerimp.hpp"
#include "mwbase/session.hpp"
#include "mwmechanics/alchemyhelpers.hpp"

// Ingredients on the left as the inventory shows them (pictures, counts), on the right the (up to four) chosen in their
// slots, the apparatus that is carried with its quality, and the effects the potion will have (those the player's
// Alchemy knows; the rest "?"), Create / Close. Morrowind's window shows pictures in its slots; ours had names on buttons
void Session::drawAlchemy()
{
	header(w.game.gmst("salchemy", "Alchemy"));
	float q[4];
	bestApparatus(w, q);
	s_alchemyWorld = &w;
	std::vector<std::string> ids;
	std::vector<UiGridItem> cells;
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
					cells.push_back({ o->iconIx, it.count - used, 0, o->name.str() });
				}
			}
	if (cells.empty())
		uiTextCentered(78, 100, 0.45f, col::textDim, "No ingredients");
	int pick = uiItemGrid(craftGrid, 4, 24, 148, 160, cells);
	if (craftGrid.selected >= 0 && craftGrid.selected < (int)cells.size())
		uiTextCentered(78, 188, 0.36f, col::text, cells[craftGrid.selected].label);
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
	// The chosen ones in their four slots: tap to put back
	uiText(160, 22, 0.32f, col::textDim, w.game.gmstLabel("singredients", "Ingredients"));
	for (size_t k = 0; k < 4; k++)
	{
		const Object* o = k < alchemySlots.size() ? w.game.object(alchemySlots[k]) : nullptr;
		float x = 160 + k * 39;
		uiFrame(x, 34, 36, 36);
		if (o && !uiIcon(o->iconIx, x + 2, 36, 32.0f))
			uiTextCentered(x + 18, 47, 0.3f, col::text, o->name.str().substr(0, 5));
		if (o && uiHit(x, 34, 36, 36))
		{
			alchemySlots.erase(alchemySlots.begin() + k);
			playSound(-1, "Menu Click");
			break;
		}
	}
	// The apparatus carried (the best of each kind), its quality under it; dim where there is none
	uiText(160, 72, 0.32f, col::textDim, w.game.gmstLabel("sapparatus", "Apparatus"));
	static const char* kTool[4] = { "Mortar", "Alembic", "Calcinator", "Retort" };
	for (int k = 0; k < 4; k++)
	{
		const Object* best = nullptr;
		for (auto& it : w.inventory)
			if (const Object* o = w.game.object(it.id))
				if (o->recType == REC_APPA && o->subtype == k && (!best || o->quality > best->quality))
					best = o;
		float x = 160 + k * 39;
		uiFrame(x, 84, 30, 30);
		if (best && !uiIcon(best->iconIx, x + 1, 85, 28.0f))
			uiTextCentered(x + 15, 94, 0.28f, col::text, kTool[k]);
		char ql[16];
		snprintf(ql, sizeof(ql), best ? "%.1f" : "-", q[k]);
		uiTextCentered(x + 15, 116, 0.3f, col::textDim, best ? ql : "-");
	}
	// The potion-to-be's effects, in order (OpenMW's Created Effects)
	uiText(160, 130, 0.32f, col::textDim, w.game.gmstLabel("screatedeffects", "Created Effects"));
	float y = 142;
	size_t index = 0;
	for (auto& e : commonEffects(w, alchemySlots))
	{
		bool seen = alchemyEffectKnown(index++);      // (the potion-to-be's effects in order, as OpenMW's window)
		uiText(162, y, 0.38f, seen ? col::text : col::textDim, seen ? effectLabel(e.effect, e.skill, e.attribute) : "?");
		y += 13;
		if (y > 190)
			break;
	}
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
