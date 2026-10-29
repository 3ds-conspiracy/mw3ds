// Travel services (silt strider, boat, guild guide), and arriving
#include <algorithm>
#include <array>
#include <functional>
#include <sys/stat.h>
#include <tex3ds.h>
#include <citro2d.h>
#include "mwbase/session.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "mwsound/soundmanagerimp.hpp"
#include "components/debug/debuglog.hpp"
#include <malloc.h>
#include <cstdio>
#include "mwgui/windowmanagerimp.hpp"

// Called by the main loop after it has shown the loading screen
void Session::finishTravel()
{
	int dest = travelCell;
	travelCell = -1;
	loadScreens++;
	// (an Old 3DS's dynamic view starts short again in the new place and opens out while there is room)
	if (g_profile.viewStartsShort)
		viewScale = viewDrawn = g_profile.viewScaleMin;
	if (g_profile.cleanAtDoors)
		cleanForLoad();
	w.player.inertia[0] = w.player.inertia[1] = 0.0f;     // (a jump's take-off doesn't come along: doors, travel)
	// Voices of the cell we leave stop with it
	for (auto& r : w.refs)
		if (r.voiceChannel >= 0)
		{
			audioStop(r.voiceChannel);
			r.voiceChannel = -1;
		}
	sayUntil.clear();
	subtitle.clear();
	stopLoopSounds();
	target = -1;
	u64 start = osGetTime();
	// Followers (a follow package, summons; not escorts) within 800 units come along (OpenMW's getFollowers): not
	// one whose script says stayoutside when it is outdoors and the way leads in; one fighting the player stops
	// instead and stays
	std::vector<int> followers;
	bool toExterior = !w.cells[dest].interior;
	for (int i : w.loadedActors)
		if (w.followsPlayer(i) && w.refs[i].aiPackage != AIPKG_ESCORT)
		{
			Ref& f = w.refs[i];
			float* stay = f.script >= 0 ? w.scripts[f.script].local("stayoutside") : nullptr;
			float dx = f.pos[0] - w.player.feet[0], dy = f.pos[1] - w.player.feet[1], dz = f.pos[2] - w.player.feet[2];
			bool taken = followerTaken(dx * dx + dy * dy + dz * dz, stay && *stay == 1.0f, !w.cells[w.placeOf(i)].interior, toExterior);
			logf("travel: follower %s %.0f units away%s", f.id.c_str(), sqrtf(dx * dx + dy * dy + dz * dz), taken ? "" : ", stays");
			if (!taken)
				continue;
			if (f.ai == AI_COMBAT)
			{
				f.ai = AI_IDLE;
				continue;
			}
			followers.push_back(i);
		}
	// Outdoors the cells to load follow the player's position
	w.player.feet[0] = travelPos[0];
	w.player.feet[1] = travelPos[1];
	if (!w.enterCell(dest))
	{
		logf("travel: failed to load %s (linear free %lu KB)", w.cells[dest].name.c_str(), linearSpaceFree() / 1024);
		notify("Could not load " + w.cells[dest].name + " (out of memory?). See log.txt.");
		return;
	}
	if (g_profile.cleanAtDoors && !w.streamJob)
		trimSkeletons();                 // (the place left: its people's skeletons)
	if (travelToSpawn)
	{
		travelToSpawn = false;
		const float* sp = w.here().spawn;
		if (sp[0] != 0.0f || sp[1] != 0.0f)
		{
			memcpy(travelPos, sp, sizeof(travelPos));
			travelYaw = w.here().yaw;
			w.player.feet[0] = sp[0];
			w.player.feet[1] = sp[1];
		}
		else
			travelPos[2] = 2000.0f;   // no spawn spot: dropped onto the ground from the middle
	}
	float eye[3] = { travelPos[0], travelPos[1], travelPos[2] + PLAYER_EYE_HEIGHT };
	Scene scene = w.scene();
	playerSpawn(w.player, scene, eye, travelYaw, 0.0f);
	// Nothing under the feet outdoors: the ground may be a neighbouring cell's that would stream in after the player
	// had fallen. Those near the arrival load now, still behind the loading screen, and the player is set down again
	if (!w.player.onGround && !w.cells[w.current].interior && w.loadCellsNear(travelPos, 2048.0f) > 0)
	{
		scene = w.scene();
		playerSpawn(w.player, scene, eye, travelYaw, 0.0f);
	}
	for (size_t k = 0; k < followers.size(); k++)
	{
		// Just ahead of the player, side by side (behind is the door, often a wall)
		float back = -72.0f, side = ((int)k - (int)followers.size() / 2) * 64.0f + (k % 2 ? 40.0f : -40.0f);
		float fx = sinf(w.player.yaw), fy = cosf(w.player.yaw);
		float pos[3] = { w.player.feet[0] - fx * back + fy * side, w.player.feet[1] - fy * back - fx * side,
			w.player.feet[2] };
		int c = w.current;
		if (!w.cells[c].interior)
		{
			int g = w.gridCell((int)floorf(pos[0] / 8192.0f), (int)floorf(pos[1] / 8192.0f));
			if (g >= 0 && w.cells[g].live)
				c = g;
		}
		w.relocate(followers[k], c, pos, w.player.yaw);
	}
	// Still behind the loading screen: what would otherwise be read from the card in play (the weapon's draw, its sound
	// after an Old 3DS's door emptied the sound cache; the race menu's face on the dock)
	warmWeapon();
	preloadRacePreview();
	fade = 1.0f;
	fadeRate = 1.0f;                     // (back over a second, as OpenMW's TravelWindow: fadeScreenIn(1))
	if (!autotest)
		save();
	logf("travel: %s in %llu ms, feet %.0f %.0f %.0f", w.cellName().c_str(), osGetTime() - start,
		w.player.feet[0], w.player.feet[1], w.player.feet[2]);
}

// OpenMW: the distance from the player to the destination (3D) / fTravelMult, times one more for each follower who
// comes along, at the caravaner's barter price; a guide standing indoors (the Mages Guild) charges fMagesGuildTravel
int Session::travelPrice(int ref, const TravelDest& d)
{
	float dx = d.pos[0] - w.player.feet[0], dy = d.pos[1] - w.player.feet[1], dz = d.pos[2] - w.player.feet[2];
	int followers = 0;
	for (int i : w.loadedActors)
		if (w.refs[i].aiPackage == AIPKG_FOLLOW && w.refs[i].aiTarget == "player" && !w.refs[i].dead && w.active(i))
			followers++;
	return travelPriceFor(ref, sqrtf(dx * dx + dy * dy + dz * dz), followers, w.cells[w.current].interior);
}

// The dialogue's Travel button: the speaker's destinations
void Session::openTravel()
{
	barterRef = dlg.ref;
	list2 = UiList();
	screen = SCR_TRAVEL;
	playSound(-1, "Menu Click");
}

// The travel list's Go on destination `sel`: pay, and leave next frame (finishTravel). False if refused.
bool Session::travelGo(int sel)
{
	const ActorDef& def = w.game.actors[w.refs[barterRef].actor];
	const TravelDest& d = def.travel[sel];
	int price = travelPrice(barterRef, d);
	int dest = d.hasGrid ? w.gridCell(d.grid[0], d.grid[1]) : w.cellIndex(d.cell);
	if (w.itemCount("gold_001") < price)
	{
		notify("You don't have enough gold.");
		return false;
	}
	if (dest < 0)
	{
		notify(d.cell + " is not converted yet.");
		return false;
	}
	w.removeItem("gold_001", price);
	w.refs[barterRef].gold += price;
	// OpenMW: whole hours of the flat distance from the player, and only from outdoors
	float dx = d.pos[0] - w.player.feet[0], dy = d.pos[1] - w.player.feet[1];
	if (!w.cells[w.current].interior)
	{
		float hours = (float)(int)(sqrtf(dx * dx + dy * dy) / w.game.gmstf("ftraveltimemult", 16000.0f));
		w.gameHour += hours;
		rechargeItems(hours * 3600.0f / w.timescale());
	}
	travelCell = dest;
	w.pcTraveling = 1;                   // (GetPCTraveling: up until the first scripts after the trip have run)
	memcpy(travelPos, d.pos, sizeof(travelPos));
	travelYaw = d.rot[2];
	logf("travel: %s to %s for %d gold", def.name.c_str(), d.cell.c_str(), price);
	closeScreen();
	return true;
}

void Session::drawTravel()
{
	const Ref& r = w.refs[barterRef];
	const ActorDef& def = w.game.actors[r.actor];
	int gold = w.itemCount("gold_001");
	header(w.game.gmst("stravel", "Travel") + " - " + def.name, "Gold: " + std::to_string(gold));
	std::vector<std::string> rows;
	for (auto& d : def.travel)
		rows.push_back(w.game.cellLabel(d.cell) + "   " + std::to_string(travelPrice(barterRef, d)) + "g");
	uiList(list2, 4, 24, 312, 176, rows, 0.5f, true);
	int sel = list2.selected < (int)def.travel.size() ? list2.selected : -1;
	int b = buttonRow({ "Go", w.game.gmst("sdone", "Done") }, focus);
	if (b == 0 && sel >= 0)
		travelGo(sel);
	else if (b == 1 || (uiIn().down & KEY_B))
	{
		barterRef = -1;
		screen = SCR_DIALOGUE;
	}
}
