// Script functions: cells (GetPCCell, CellChanged, COC, COE)
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

std::optional<float> cellExtensions(Ctx& c, const Node& n, World& w, bool onPlayer, int ref, Ref* r)
{
	switch (n.func)
	{
	case F_GETPCCELL: return w.current >= 0 && lower(displayCellName(w)).rfind(lower(argStr(c, n, 0)), 0) == 0 ? 1.0f : 0.0f;
	// The player's cell changed since scripts last ran (a door, or outdoors a new grid cell)
	case F_CELLCHANGED: return w.current != w.scriptCell ? 1.0f : 0.0f;
	// ---- The console's own (OpenMW registers them for scripts too)
	// COC "cell" / CenterOnCell: the player to that cell, a named exterior or an interior (where a door into it lands)
	case F_COC: case F_CENTERONCELL:
		if (!c.host.centerOnCell(argStr(c, n, 0)))
			c.host.report("Cell " + argStr(c, n, 0) + " is not found");
		return 0.0f;
	// COE x y / CenterOnExterior: the player to the middle of that exterior cell, on the ground
	case F_COE: case F_CENTERONEXTERIOR:
		if (!c.host.centerOnExterior((int)argNum(c, n, 0), (int)argNum(c, n, 1)))
			c.host.report("Exterior cell " + std::to_string((int)argNum(c, n, 0)) + ", " + std::to_string((int)argNum(c, n, 1))
				+ " is not in the level");
		return 0.0f;
	default:
		return std::nullopt;
	}
}
