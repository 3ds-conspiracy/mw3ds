#include <algorithm>
#include <array>
#include <functional>
#include <sys/stat.h>
#include <tex3ds.h>
#include <citro2d.h>
#include "mwscript/scriptstore.hpp"
#include "mwbase/session.hpp"
#include "components/debug/profile.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "mwsound/soundmanagerimp.hpp"
#include "platform/linear.hpp"
#include "components/debug/debuglog.hpp"
#include <malloc.h>
#include "mwbase/sessionhelpers.hpp"


void Session::shutdown()
{
	// (each part marked and timed: the quit and a save's load run this)
	u64 t0 = osGetTime();
	MARK("shutdown: sounds");
	if (waterChannel >= 0)
		audioStop(waterChannel);
	waterChannel = -1;
	for (auto& r : w.refs)
		if (r.voiceChannel >= 0)
			audioStop(r.voiceChannel);
	MARK("shutdown: hands and body");
	vm.free(w);
	body.free(w);
	u64 t1 = osGetTime();
	MARK("shutdown: cells");
	w.freeAll();
	u64 t2 = osGetTime();
	// what outlives a session in globals: freed now, before a loaded save's session reads its own (the
	// skeletons and sounds of two sessions at once ran out of memory loading a save). The distant land
	// stays: the next session reuses it (distantLoad), its ~20 MB read being the hard part of a reload.
	MARK("shutdown: skeletons and sounds");
	skeletonsFree();
	audioCacheClear();
	u64 t3 = osGetTime();
	MARK("shutdown: game data");
	w.keepGame();
	u64 t4 = osGetTime();
	MARK("shutdown: map");
	if (mapLoaded)
	{
		LinearGuard guard;
		deferredTexDelete(&mapTex);
		mapLoaded = false;
	}
	freeMapTiles();
	logf("shutdown: %llu ms (sounds, hands and body %llu, cells %llu, skeletons and sounds %llu, game data %llu, map %llu)",
		osGetTime() - t0, t1 - t0, t2 - t1, t3 - t2, t4 - t3, osGetTime() - t4);
}

// Per-frame time of each part of the update, summed until main logs them once a second; the hot paths' call counts
// beside them (components/debug/profile.hpp)
float g_profParts[PROF_COUNT];
int g_profCalls[PCALL_COUNT];

bool cadenceOn(int which)
{
	static int off = -1;
	if (off < 0)
	{
		const char* e = getenv("MW3DS_NOCADENCE");
		off = e ? atoi(e) : 0;
	}
	// The cadences that move when a person reacts (a sighting, a greeting a few frames later) are the slow CPU's: a fast
	// one keeps every frame, and the scripted questlines keep their timing. The floor walk and the monitor gate change
	// nothing a person does
	if (!g_profile.slowCpu && which != CAD_FLOOR && which != CAD_MONITOR)
		return false;
	return !(off & which);
}


// The menus that stop the world (OpenMW: a GUI mode stops the simulation). Talk, barter and the other
// services with an NPC don't (he keeps moving, as in Morrowind), nor do rest, travel, death or creation.
bool Session::worldPaused() const
{
	if (dlg.open)
		return false;
	switch (screen)
	{
	case SCR_INVENTORY: case SCR_MAGIC: case SCR_SPELLS: case SCR_STATS: case SCR_MAP: case SCR_JOURNAL:
	case SCR_SAVES: case SCR_OPTIONS: case SCR_AUDIO: case SCR_CONTROLS: case SCR_DISPLAY: case SCR_LOOK: case SCR_CONSOLE: case SCR_GAMEMENU: case SCR_BOOK: case SCR_CONTAINER:
		return true;
	default:
		return false;
	}
}

void Session::update(const PlayerInput& inRaw, float dt)
{
	// Gyro aim's calibration, as it went (main's gyroInput)
	extern int g_gyroCalNotice;
	if (g_gyroCalNotice)
	{
		notify(g_gyroCalNotice > 0 ? "Gyro calibrated." : "The 3DS moved, so the gyro wasn't calibrated: lay it down and choose the speed again.");
		g_gyroCalNotice = 0;
	}
	u64 t = profTick();
	MARK("world update");
	w.pcWeaponDrawn = weaponDrawn;
	w.update(dt);
	profAdd(PROF_WORLD, t);
	updateWindows();
	// Every few seconds: cells the player left far behind give their objects' memory back
	if ((evictTimer -= dt) <= 0.0f)
	{
		evictTimer = 5.0f;
		evictFarCells();
		// An Old 3DS lets go of the animation skeletons no loaded actor uses (they are read again when met)
		if (g_profile.cleanAtDoors && !w.streamJob)
			trimSkeletons();
		// ... and the string pool takes its next block now, while the heap is tidy, not mid-parse
		if (g_profile.cleanAtDoors)
			Atom::prefill();
		updateCorprus();
	}

	openPendingMenu();

	bool menu = menuOpen() || wantName;
	PlayerInput in = inRaw;
	playerControls(in, menu);
	playerMove(in, dt, t);

	// The converted land ends at the walkable bounds
	Cell& here = w.here();
	if (here.bounded() && !w.player.flying)
	{
		const float margin = 64.0f;
		float* f = w.player.feet;
		float cx = fmaxf(here.bounds[0] + margin, fminf(f[0], here.bounds[2] - margin));
		float cy = fmaxf(here.bounds[1] + margin, fminf(f[1], here.bounds[3] - margin));
		if (cx != f[0] || cy != f[1])
		{
			f[0] = cx;
			f[1] = cy;
			if (w.time > boundsNoteUntil)
			{
				notify("The land beyond here is not converted yet.");
				boundsNoteUntil = w.time + 8.0f;
			}
		}
	}
	// Outdoors: follow the player into other grid cells, loading one missing neighbour per frame
	profAdd(PROF_OTHER, t);
	w.streamExterior(false);
	w.retryMissingTextures(dt);
	// (the responses a talk read back from the card go again once it is over)
	if (!dlg.open)
		topicsRelease();
	scriptsTick();
	profAdd(PROF_STREAM, t);

	aimAndActivate(in, menu);

	profAdd(PROF_OTHER, t);
	MARK("combat");
	combatUpdate(in, dt, menu);
	profAdd(PROF_AI, t);
	viewModelUpdate(in, dt);
	MARK("gravity");
	actorGravity(dt);
	MARK("effects");
	updateEffects(dt);
	rechargeItems(dt);
	castUpdate(dt);
	// (a cast, like a blow, plays out before the next: OpenMW has one spellcast animation at a time)
	if (in.cast && !menu && w.controlsEnabled && !playerDead && w.magicEnabled && vm.action != VM_CAST && vm.action != VM_FOLLOW
		&& !castBusy())
	{
		// (a werewolf casts nothing, nor uses quick keys: OpenMW's HUD refuses magic and items in wolf form)
		if (!werewolfRefused())
			castSpell();
	}
	if (in.quick >= 0 && !menu && w.controlsEnabled && !playerDead && !werewolfRefused())
		useQuickKey(in.quick);
	profAdd(PROF_OTHER, t);
	MARK("scripts");
	scriptsRun(w, *this, dt);
	if (w.pcTraveling > 0 && travelCell < 0)
		w.pcTraveling--;
	if (w.pcInJail > 0)
		w.pcInJail--;
	profAdd(PROF_SCRIPTS, t);
	if (!menuOpen())
	{
		MARK("monitors");
		monitorActors(dt);
		MARK("greetings");
		greetings(dt);
	}
	profAdd(PROF_GREET, t);

	updateMouths(dt);

	updateFootsteps(in, menu, dt);
	if (w.werewolfJustChanged)
	{
		w.werewolfJustChanged = false;
		werewolfWitnesses();
	}

	// (the loops' mixes every third frame: a lookup by name and trig per loop, and the ear can't tell)
	if (w.frame % 3 == 0 || !cadenceOn(CAD_LOOPS))
	{
		updateAmbience();
		updateLoopSounds();
	}
	updateWeatherEffects(dt);
	updateMusic(dt);
	profAdd(PROF_SOUND, t);
	updateSky();
	updateDress(dt);
	profAdd(PROF_TAIL, t);
	updateRegionSounds(dt);
	profAdd(PROF_SOUND, t);
	burnHeldLight(dt);
	updateLights();
	ageVfx(dt);
	if (!playerDead && w.stats.health <= 0.0f)
		playerDies();
	updateFadeAndNotes(dt);
	profAdd(PROF_TAIL, t);
	audioUpdate();
	profAdd(PROF_SOUND, t);
}
