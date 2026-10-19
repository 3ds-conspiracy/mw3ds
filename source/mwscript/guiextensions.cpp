// Script functions: menus, rest, the map
#include "components/interpreter/interpreter.hpp"

#include <optional>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_set>

#include "mwworld/worldimp.hpp"
#include "components/interpreter/interpreterhelpers.hpp"

std::optional<float> guiExtensions(Ctx& c, const Node& n, World& w, bool onPlayer, int ref, Ref* r)
{
	switch (n.func)
	{
	case F_MENUMODE: return c.host.menuMode() ? 1.0f : 0.0f;
	case F_ENABLECLASSMENU: c.host.openMenu(SMENU_CLASS); return 0.0f;
	case F_ENABLEBIRTHMENU: c.host.openMenu(SMENU_BIRTH); return 0.0f;
	case F_ENABLESTATREVIEWMENU: c.host.openMenu(SMENU_REVIEW); return 0.0f;
	case F_ENABLERACEMENU: c.host.openMenu(SMENU_RACE); return 0.0f;
	case F_ENABLENAMEMENU: c.host.openMenu(SMENU_NAME); return 0.0f;
	case F_ENABLESTATSMENU: w.menusEnabled |= MENU_STATS; return 0.0f;
	case F_ENABLEINVENTORYMENU: w.menusEnabled |= MENU_INVENTORY; return 0.0f;
	case F_ENABLEMAGICMENU: w.menusEnabled |= MENU_MAGIC; return 0.0f;
	case F_ENABLEMAPMENU: w.menusEnabled |= MENU_MAP; return 0.0f;
	case F_ENABLEREST: w.menusEnabled |= MENU_REST; return 0.0f;
	// From a bed's own script: sleeping is allowed there whatever the cell says
	case F_SHOWRESTMENU:
		// (OpenMW's sleepInBed: a werewolf, enemies about, an owned bed whose trespass someone reported)
		if (c.self >= 0 && c.host.bedRefused(c.self))
			return 0.0f;
		c.host.openMenu(c.self >= 0 ? SMENU_REST_BED : SMENU_REST);
		return 0.0f;
	case F_GETPCSLEEP: return w.pcSleeping ? 1.0f : 0.0f;
	case F_WAKEUPPC:
		// awake at once (GetPCSleep 0): the Sleepers script gives one dream a night, returning after it
		w.wakeUp = true;
		w.pcSleeping = false;
		return 0.0f;
	// ShowMap "Vivec": every exterior place whose name starts with that (Vivec, Vivec, Fred's House ...)
	case F_SHOWMAP:
	{
		std::string part = lower(argStr(c, n, 0));
		if (part.empty())
			return 0.0f;
		w.mapKnown.insert(part);       // (a map label named so)
		for (auto& cl : w.cells)
			if (!cl.interior && !cl.name.empty() && lower(cl.name).compare(0, part.size(), part) == 0)
			{
				w.mapKnown.insert(lower(cl.name));
				w.mapCells.insert(w.mapCellKey(cl.gx, cl.gy));
			}
		return 0.0f;
	}
	// FillMap: every named exterior place on the map, as visited
	case F_FILLMAP:
		for (auto& cl : w.cells)
			if (!cl.interior && cl.name.size())
				w.mapKnown.insert(lower(cl.name.str()));
		return 0.0f;
	default:
		return std::nullopt;
	}
}
