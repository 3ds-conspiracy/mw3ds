// The enchanting screen: at an enchanter, or by the player's own hand
#include <cmath>
#include <functional>

#include "mwgui/windowmanagerimp.hpp"
#include "mwbase/session.hpp"
#include "mwmechanics/enchantinghelpers.hpp"

void Session::openEnchanting(int enchanter)
{
	enchanterRef = enchanter;
	makeEffects.clear();
	makeSel = -1;
	makeName.clear();
	makeNameItem.clear();
	enchantItem = enchantGem = -1;
	enchantType = -1;
	enchantPick = 0;
	list2 = UiList();
	std::vector<int> items, gems;
	enchantChoices(items, gems);
	if (!items.empty())
		enchantItem = items[0];
	if (!gems.empty())
		enchantGem = gems[0];
	screen = SCR_ENCHANT;
}

// The new item's name starts as the chosen item's own name, again whenever another item is chosen (OpenMW's
// EnchantingDialog::setItem); a name typed in is kept while the item stays
void Session::syncEnchantName()
{
	const std::string id = enchantItem >= 0 && enchantItem < (int)w.inventory.size() ? w.inventory[enchantItem].id : "";
	if (id == makeNameItem)
		return;
	const Object* o = id.empty() ? nullptr : w.game.object(id);
	makeName = o ? o->name : "";
	makeNameItem = id;
}

// A slot as Morrowind's window has one: a framed box with the chosen thing's picture in it (empty: what goes there), its
// caption, name and a note beside it; a tap anywhere on it is the slot's
static bool craftSlot(float x, float y, float w, int icon, const std::string& caption, const std::string& name,
	const std::string& note)
{
	uiFrame(x, y, 36, 36);
	if (icon < 0 || !uiIcon(icon, x + 2, y + 2, 32.0f))
		uiTextCentered(x + 18, y + 13, 0.3f, col::textDim, "?");
	uiText(x + 40, y + 1, 0.32f, col::textDim, caption);
	uiText(x + 40, y + 12, 0.4f, col::text, name);
	if (!note.empty())
		uiText(x + 40, y + 25, 0.3f, col::textDim, note);
	return uiHit(x, y, w, 36);
}

// What each cast type does, under its button (the button alone named it and left the player guessing)
static const char* kCastHelp[4] = { "Used up when cast", "Casts on whoever it strikes", "Cast at will from its charge",
	"Always on while worn" };

void Session::drawEnchanting()
{
	int gold = w.itemCount("gold_001");
	header(w.game.gmst("senchanting", "Enchanting"), "Gold " + std::to_string(gold));
	std::vector<int> items, gems;
	enchantChoices(items, gems);
	auto indexIn = [](const std::vector<int>& v, int x) {
		for (size_t k = 0; k < v.size(); k++)
			if (v[k] == x)
				return (int)k;
		return -1;
	};
	if (indexIn(items, enchantItem) < 0)
		enchantItem = items.empty() ? -1 : items[0];
	if (indexIn(gems, enchantGem) < 0)
		enchantGem = gems.empty() ? -1 : gems[0];
	int noFocus = -1;
	// Choosing the item or the soul gem: the candidates' pictures in a grid, as the inventory shows them
	if (enchantPick)
	{
		const std::vector<int>& from = enchantPick == 1 ? items : gems;
		uiText(6, 24, 0.4f, col::textDim, enchantPick == 1 ? "Choose the item to enchant" : "Choose a filled soul gem");
		std::vector<UiGridItem> cells;
		for (int i : from)
		{
			const Object* o = w.game.object(w.inventory[i].id);
			std::string label = o ? o->name.str() : w.inventory[i].id;
			if (enchantPick == 2)
				for (auto& a : w.game.actors)
					if (lower(a.id) == w.inventory[i].soul)
						label = a.name + " (" + std::to_string(w.soulValue(w.inventory[i])) + ")";
			cells.push_back({ o ? o->iconIx : -1, w.inventory[i].count, 0, label });
		}
		int pick = uiItemGrid(craftGrid, 4, 40, 312, 140, cells);
		if (craftGrid.selected >= 0 && craftGrid.selected < (int)cells.size())
			uiTextCentered(160, 186, 0.4f, col::text, cells[craftGrid.selected].label);
		if (pick >= 0 && pick < (int)from.size())
		{
			(enchantPick == 1 ? enchantItem : enchantGem) = from[pick];
			enchantPick = 0;
			playSound(-1, "Menu Click");
		}
		else if (buttonRow({ w.game.gmst("sback", "Back") }, noFocus) == 0 || (uiIn().down & KEY_B))
		{
			enchantPick = 0;
			playSound(-1, "Menu Click");
		}
		return;
	}
	const Object* item = enchantItem >= 0 ? w.game.object(w.inventory[enchantItem].id) : nullptr;
	int soul = enchantGem >= 0 ? w.soulValue(w.inventory[enchantGem]) : 0;
	int types = castTypesFor(item);
	if (enchantType < 0 || !(types & (1 << enchantType)))
		for (int t = 0; t < 4; t++)
			if (types & (1 << t))
			{
				enchantType = t;
				break;
			}
	syncEnchantName();
	// The item and the soul gem, each in its slot with its picture: a tap chooses another from a grid
	if (craftSlot(4, 24, 154, item ? item->iconIx : -1, "Item", item ? makeName : "Nothing to enchant",
			items.size() > 1 ? "Tap to choose another" : "") && !items.empty())
	{
		enchantPick = 1;
		craftGrid = UiGrid();
		craftGrid.selected = std::max(0, indexIn(items, enchantItem));
		playSound(-1, "Menu Click");
	}
	const Object* gem = enchantGem >= 0 ? w.game.object(w.inventory[enchantGem].id) : nullptr;
	std::string soulName = "No filled soul gem";
	if (enchantGem >= 0)
	{
		soulName = w.game.gmstLabel("ssoulgem", "Soul");
		for (auto& a : w.game.actors)
			if (lower(a.id) == w.inventory[enchantGem].soul)
				soulName = a.name;
	}
	if (craftSlot(162, 24, 154, gem ? gem->iconIx : -1, w.game.gmstLabel("ssoulgem", "Soul"), soulName,
			enchantGem >= 0 ? "Soul " + std::to_string(soul) : "") && !gems.empty())
	{
		enchantPick = 2;
		craftGrid = UiGrid();
		craftGrid.selected = std::max(0, indexIn(gems, enchantGem));
		playSound(-1, "Menu Click");
	}
	static const char* kCastKeys[4] = { "sitemcastonce", "sitemcastwhenstrikes", "sitemcastwhenused", "sitemcastconstant" };
	if (uiButton(4, 64, 154, 18, enchantType >= 0 ? w.game.gmstLabel(kCastKeys[enchantType], kCastNames[enchantType]) : "-") && types)
	{
		for (int k = 1; k <= 4; k++)
			if (types & (1 << ((enchantType + k) % 4)))
			{
				enchantType = (enchantType + k) % 4;
				break;
			}
		// Constant effects are on the wearer; strikes land on who's hit
		for (auto& e : makeEffects)
			e.range = enchantType == 3 ? 0 : enchantType == 1 && e.range == 0 ? 1 : e.range;
	}
	if (enchantType >= 0)
		uiText(6, 84, 0.3f, col::textDim, kCastHelp[enchantType]);
	// Ranges this cast type allows: constant self only, on strike touch / target
	int ranges = enchantType == 3 ? 1 : enchantType == 1 ? 6 : 7;
	std::vector<SpellEffect> known = knownEffects(), usable;
	std::vector<std::string> rows;
	std::vector<int> icons;
	for (auto& e : known)
	{
		SpellEffect n = newEffect(e, ranges);
		int f = effectFlags(e.effect);
		bool ok = (n.range == 0 && (f & 0x40) && (ranges & 1)) || (n.range == 1 && (ranges & 2)) || (n.range == 2 && (ranges & 4));
		if (!ok)
			continue;
		usable.push_back(n);
		rows.push_back(effectLabel(e.effect, e.skill, e.attribute));
		auto me = w.game.magicEffects.find(e.effect);
		icons.push_back(me != w.game.magicEffects.end() ? me->second.iconIx : -1);
	}
	int pick = uiList(list2, 4, 98, 130, 106, rows, 0.4f, true, nullptr, &icons);
	if (pick >= 0 && pick < (int)usable.size() && makeEffects.size() < 4)
	{
		makeEffects.push_back(usable[pick]);
		makeSel = makeEffects.size() - 1;
		askEffectTarget();
		playSound(-1, "Menu Click");
	}
	effectEditor(makeEffects, makeSel, ranges, 92, 3);
	// The numbers OpenMW's window shows: points against what the soul holds, the cast cost, the charge, the price or chance
	EnchantCalc c = enchantCalc();
	char line[64];
	snprintf(line, sizeof(line), "Enchantment %.0f / %.0f", c.points, c.capacity);
	uiText(164, 63, 0.34f, c.points > c.capacity ? col::health : col::textDim, line);
	snprintf(line, sizeof(line), "Cast %d", enchantType == 3 ? 0 : (int)enchantCastCost(c.points));
	uiText(270, 63, 0.34f, col::textDim, line);
	snprintf(line, sizeof(line), "Charge %d", soul);
	uiText(164, 75, 0.34f, col::textDim, line);
	if (enchanterRef >= 0)
		snprintf(line, sizeof(line), "Price %d", c.price);
	else
		snprintf(line, sizeof(line), "Chance %d%%", std::max(0, (int)c.chance));
	uiText(240, 75, 0.34f, col::textDim, line);
	// (the new item's name is in its slot: the first button renames it, no longer a second copy of it)
	// (at an enchanter it is bought, as OpenMW's Buy; by the player's own hand, made)
	std::string make = enchanterRef >= 0 ? w.game.gmst("sbuy", "Buy") + " (" + std::to_string(c.price) + ")" : "Enchant";
	int b = buttonRow({ w.game.gmst("srename", "Rename"), make, w.game.gmst("sclose", "Close") }, noFocus);
	if (b == 0)
	{
		playSound(-1, "Menu Click");
		wantText = TEXT_MAKENAME;
	}
	else if (b == 1)
		enchantConfirm();
	else if (b == 2 || (uiIn().down & KEY_B))
	{
		playSound(-1, "Menu Click");
		makeEffects.clear();
		makeSel = -1;
		makeName.clear();
		makeNameItem.clear();
		screen = enchanterRef >= 0 ? SCR_DIALOGUE : SCR_INVENTORY;
	}
}
