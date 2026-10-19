#include <algorithm>
#include <array>
#include <functional>
#include <sys/stat.h>
#include <tex3ds.h>
#include <citro2d.h>
#include "mwrender/objectpaging.hpp"
#include "mwrender/animation.hpp"
#include "mwbase/session.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "mwsound/soundmanagerimp.hpp"
#include "platform/gamma.hpp"
#include "platform/linear.hpp"
#include "components/debug/debuglog.hpp"
#include "platform/memstat.hpp"
#include <malloc.h>
#include "mwbase/sessionhelpers.hpp"

static const float kLightBounds = 1.65f;     // OpenMW's light bounds multiplier: a light reaches this x its radius

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

// Per-frame time of each part of the update, summed until main logs them once a second
float g_profParts[PROF_COUNT];

static u64 profTick()
{
	return svcGetSystemTick();
}

static void profAdd(int part, u64& since)
{
	u64 now = svcGetSystemTick();
	g_profParts[part] += (now - since) * 1000.0f / SYSCLOCK_ARM11;
	since = now;
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
	revealLocalMap();
	// A named place outdoors, once visited, is on the map
	if (w.current >= 0 && w.current != mapCell)
	{
		mapCell = w.current;
		if (!w.cells[mapCell].interior)
		{
			w.mapKnown.insert(lower(w.cells[mapCell].name));
			w.mapCells.insert(World::mapCellKey(w.cells[mapCell].gx, w.cells[mapCell].gy));
		}
	}
	// The journal opens on its newest page
	if (screen != SCR_JOURNAL)
	{
		journalPage = 0;
		journalPicking = false;
	}
	// The map screen starts centred on the player and gives its detail tiles back when it closes
	if (screen != SCR_MAP)
	{
		mapViewSet = false;
		freeMapTiles();
		mapDoorTip = -1;
		mapLocal = (w.current >= 0 && w.cells[w.current].interior) || mapLocalOutdoors;
	}
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
		// Corprus: note when it was caught; a day later it is worse (the numbers follow)
		bool corprus = w.pcHasCorprus();
		if (corprus && w.corprusSince < 0.0f)
			w.corprusSince = w.gameHour;
		else if (!corprus && w.corprusSince > -1.5f)
			w.corprusSince = -1.0f;                // (-2: cured, kept)
		if (corprus && w.corprusLevel() != corprusShown)
		{
			if (corprusShown > 0)
				notify("Your Corprus disease grows worse.");
			corprusShown = w.corprusLevel();
			PlayerStats& s = w.stats;
			float hp = s.health, mp = s.magicka, fp = s.fatigue;
			w.recomputeStats();
			s.health = fminf(s.healthMax, hp);
			s.magicka = fminf(s.magickaMax, mp);
			s.fatigue = fminf(s.fatigueMax, fp);
		}
	}

	if (!menuOpen() && !wantName && !pendingMenus.empty())
	{
		ScriptMenu m = pendingMenus.front();
		pendingMenus.erase(pendingMenus.begin());
		switch (m)
		{
		case SMENU_NAME: wantName = true; break;
		case SMENU_RACE: openScreen(SCR_RACE); break;
		case SMENU_CLASS: openScreen(SCR_CLASS_METHOD); break;
		case SMENU_BIRTH: openScreen(SCR_BIRTH); break;
		case SMENU_REVIEW: openScreen(SCR_REVIEW); break;
		case SMENU_REST: openRest(false); break;
		case SMENU_REST_BED: openRest(true); break;
		}
	}

	bool menu = menuOpen() || wantName;
	PlayerInput in = inRaw;
	// Paralyzed: no moving, no fighting
	if (w.effectTotal(45) > 0.0f)
	{
		in.moveX = in.moveY = 0.0f;
		in.jump = in.attack = in.cast = false;
	}
	if (w.forceSneak || testSneak)
		in.sneak = true;
	w.player.waterWalk = w.effectTotal(2) > 0.0f;
	w.player.slowFall = w.effectTotal(11) > 0.0f;
	w.player.levitate = w.effectTotal(10);
	w.player.swimBoost = w.effectTotal(1);
	// Encumbrance: the load (plus Burden, less Feather) against fEncumbranceStrMult x Strength; over it
	// the player can't move, under it the load slows them (fEncumberedMoveEffect)
	{
		float weight = w.effectTotal(7) - w.effectTotal(8);
		for (auto& it : w.inventory)
			if (const Object* o = w.game.object(it.id))
				weight += o->weight * it.count;
		float capacity = w.game.gmstf("fencumbrancestrmult", 5.0f) * w.stats.attributes[ATTR_STRENGTH];
		float load = capacity > 0.0f ? fmaxf(0.0f, weight) / capacity : 0.0f;
		bool over = load > 1.0f;
		if (over && !overEncumbered)
			notify(w.game.gmst("snotifymessage59", "You are carrying too much to move."));
		overEncumbered = over;
		// (OpenMW: over the limit nothing moves, levitating or not)
		w.player.loadSpeed = over ? 0.0f : 1.0f - w.game.gmstf("fencumberedmoveeffect", 0.3f) * fminf(1.0f, load);
		playerLoad = fminf(1.0f, load);
		// Storms (ash, blight, blizzard) blow out from Red Mountain: walking into them is slower,
		// by fStromWalkMult x the angle between the wind and the way the player goes (OpenMW)
		int wx = w.weatherNow;
		if ((wx == 6 || wx == 7 || wx == 9) && w.current >= 0 && !w.cells[w.current].interior
			&& (fabsf(in.moveX) + fabsf(in.moveY)) > 0.1f)
		{
			float sx = w.player.feet[0] - 20480.0f, sy = w.player.feet[1] - 69632.0f;     // from Red Mountain
			float sl = sqrtf(sx * sx + sy * sy);
			float yaw = w.player.yaw, mx = sinf(yaw) * in.moveY + cosf(yaw) * in.moveX,
				my = cosf(yaw) * in.moveY - sinf(yaw) * in.moveX;
			float ml = sqrtf(mx * mx + my * my);
			if (sl > 1.0f && ml > 0.01f)
			{
				float c = fmaxf(-1.0f, fminf(1.0f, (sx * mx + sy * my) / (sl * ml)));
				w.player.loadSpeed *= 1.0f - w.game.gmstf("fstromwalkmult", 0.25f) * (acosf(c) / 3.14159265f);
			}
		}

		// Speed from the stats (OpenMW's reading of Morrowind): walking by the Speed attribute between
		// fMinWalkSpeed and fMaxWalkSpeed, running that times fBaseRunMultiplier plus Athletics; swimming
		// and sneaking fractions of it; the jump by Acrobatics, Jump, the load, running and fatigue
		const PlayerStats& st = w.stats;
		float speedAttr = (float)st.attributes[ATTR_SPEED], athletics = (float)st.skills[8];
		float walk = walkSpeedFor((int)speedAttr);
		float run = runSpeedFor((int)speedAttr, (int)athletics);          // (mwmechanics/formulas.cpp)
		w.player.runSpeed = run;
		// Levitating: fMinFlySpeed + 0.01 x (Speed + Levitate) x (fMaxFlySpeed - fMinFlySpeed) (OpenMW's getMaxSpeed)
		w.player.flySpeed = w.game.gmstf("fminflyspeed", 5.0f) + 0.01f * (speedAttr + w.player.levitate)
			* (w.game.gmstf("fmaxflyspeed", 60.0f) - w.game.gmstf("fminflyspeed", 5.0f));
		w.player.swimFactor = (0.01f * athletics * w.game.gmstf("fswimrunathleticsmult", 0.1f)
			+ w.game.gmstf("fswimrunbase", 0.5f));
		w.player.sneakFactor = walk * w.game.gmstf("fsneakspeedmultiplier", 0.75f) / fmaxf(1.0f, run);
		bool running = sqrtf(in.moveX * in.moveX + in.moveY * in.moveY) > 0.75f;      // (full tilt)
		// in the air the pad steers fJumpMoveBase + fJumpMoveMult x Acrobatics / 100 of the run speed, at most all of it
		w.player.airControl = fminf(1.0f, w.game.gmstf("fjumpmovebase", 0.5f) + w.game.gmstf("fjumpmovemult", 0.5f) * st.skills[20] / 100.0f);
		w.player.jumpSpeed = jumpSpeedFor(st.skills[20], w.effectTotal(9), load, running, fatigueTermOf(st.fatigue, st.fatigueMax));
	}
	if (w.current >= 0 && !w.cells[w.current].interior)
		memcpy(w.lastOutside, w.player.feet, sizeof(w.lastOutside));
	// Invisibility ends when the player attacks or casts
	if ((in.attack || in.cast) && !menu)
		for (size_t i = 0; i < w.effects.size();)
			if (w.effects[i].effect == 39)
				w.effects.erase(w.effects.begin() + i);
			else
				i++;
	if (menu || !w.controlsEnabled)
	{
		in.moveX = in.moveY = 0.0f;
		in.jump = false;
		if (menu)
			in.lookX = in.lookY = in.gyroYaw = in.gyroPitch = 0.0f;
	}
	if (!w.jumpingEnabled)
		in.jump = false;
	if (w.controlsOff & 1u)
		in.lookX = in.lookY = in.gyroYaw = in.gyroPitch = 0.0f;        // DisablePlayerLooking
	{
		bool jumped = in.jump && w.player.onGround && !w.player.flying;
		Scene scene = w.scene();
		profAdd(PROF_OTHER, t);
		playerUpdate(w.player, scene, in, dt);
		profAdd(PROF_PLAYER, t);
		if (jumped)
			useSkill(20, 0);          // Acrobatics
		// Under water: fHoldBreathTime to come up, then fSuffocationDamage a second (Water Breathing: never)
		{
			Cell& here = w.here();
			bool under = w.player.swimming && here.hasWater() && playerEyeZ(w.player) < here.waterZ;
			if (!under || w.effectTotal(0) > 0.0f)
				breath = -1.0f;
			else
			{
				if (breath < 0.0f)
					breath = w.game.gmstf("fholdbreathtime", 20.0f);
				breath -= dt;
				if (breath <= 0.0f)
				{
					breath = 0.0f;
					w.stats.health -= w.game.gmstf("fsuffocationdamage", 3.0f) * dt;
					hurtFlashOngoing();
				}
			}
		}
		// A hard landing hurts (OpenMW's reading of Morrowind): past fFallDamageDistanceMin, less with
		// Acrobatics and Jump, and teaches Acrobatics
		if (w.player.landedFall > 0.0f)
		{
			float h = w.player.landedFall;
			w.player.landedFall = 0.0f;
			if (h > 4.0f && !w.player.swimming)
				playSound(-1, "DefaultLand");      // OpenMW: every landing makes this one, a jump's too
			float x = w.player.swimming ? 0.0f : fallDamage(h, w.stats.skills[20], w.effectTotal(9));   // (mwmechanics/formulas.cpp)
			if (x > 0.0f)
			{
				float ft = fatigueTermOf(w.stats.fatigue, w.stats.fatigueMax);
				logf("fall: %.0f units, %.1f damage", h, x * (1.0f - 0.25f * ft));
				damagePlayer(x * (1.0f - 0.25f * ft), false);
				// a fall past Acrobatics x fatigue knocks the player down; a softer one trains Acrobatics
				if (x > w.stats.skills[20] * ft)
					w.player.knockTimer = fmaxf(w.player.knockTimer, 2.0f);
				else
					useSkill(20, 1);
			}
		}
		// Athletics: per second spent running (or swimming)
		if (!w.player.flying && (fabsf(in.moveX) + fabsf(in.moveY)) > 0.5f)
			useSkill(8, w.player.swimming ? 1 : 0, dt);
		// Fatigue spent (fFatigue*Base + load x fFatigue*Mult): running at full tilt, swimming, sneaking
		// per second, and each jump
		{
			float tilt = sqrtf(in.moveX * in.moveX + in.moveY * in.moveY);
			const char* kind = nullptr;
			if (w.player.swimming && tilt > 0.2f)
				kind = tilt > 0.75f ? "swimrun" : "swimwalk";
			else if (w.player.sneaking && tilt > 0.2f)
				kind = "sneak";
			else if (tilt > 0.75f && w.player.onGround && !w.player.flying && w.player.levitate <= 0.0f)
				kind = "run";
			if (kind)
			{
				static const float defBase[4] = { 5.0f, 2.5f, 2.0f, 1.5f }, defMult[4] = { 2.0f, 4.0f, 1.5f, 2.0f };
				int k = !strcmp(kind, "run") ? 0 : !strcmp(kind, "swimrun") ? 1 : !strcmp(kind, "sneak") ? 2 : 3;
				// (the names as fixed texts: gmstf's cache keys on the text's address)
				static const char* const kBase[4] = { "ffatiguerunbase", "ffatigueswimrunbase", "ffatiguesneakbase", "ffatigueswimwalkbase" };
				static const char* const kMult[4] = { "ffatiguerunmult", "ffatigueswimrunmult", "ffatiguesneakmult", "ffatigueswimwalkmult" };
				float cost = w.game.gmstf(kBase[k], defBase[k]) + playerLoad * w.game.gmstf(kMult[k], defMult[k]);
				w.stats.fatigue = fmaxf(0.0f, w.stats.fatigue - cost * dt);
			}
			if (w.player.jumpedNow)
				w.stats.fatigue = fmaxf(0.0f, w.stats.fatigue - (w.game.gmstf("ffatiguejumpbase", 5.0f)
					+ playerLoad * w.game.gmstf("ffatiguejumpmult", 0.0f)));
			w.player.jumpedNow = false;
		}
	}

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
	profAdd(PROF_STREAM, t);

	float eye[3] = { w.player.feet[0], w.player.feet[1], playerEyeZ(w.player) };
	float cp = cosf(w.player.pitch);
	float dir[3] = { sinf(w.player.yaw) * cp, cosf(w.player.yaw) * cp, sinf(w.player.pitch) };
	memcpy(listener, eye, sizeof(eye));
	listener[3] = w.player.yaw;

	// Telekinesis reaches farther (magnitude in feet, 22 units each)
	target = menu ? -1 : worldPick(w, eye, dir, kReach + w.effectTotal(59) * 22.0f);
	// (GoldenEye: on a tap of A, let go before it aimed; the press itself is kept from the screens either way)
	bool goldenEye = g_controlLayout == CONTROLS_GOLDENEYE;
	if (!menu && ((uiIn().down & KEY_A) || (goldenEye && in.activate)))
	{
		uiConsume(KEY_A);
		if ((goldenEye ? in.activate : true) && target >= 0 && w.controlsEnabled)
			playerActivate(target);
	}

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
		castSpell();
	if (in.quick >= 0 && !menu && w.controlsEnabled && !playerDead)
		useQuickKey(in.quick);
	profAdd(PROF_OTHER, t);
	MARK("scripts");
	scriptsRun(w, *this, dt);
	profAdd(PROF_SCRIPTS, t);
	if (!menuOpen())
	{
		MARK("monitors");
		monitorActors(dt);
		MARK("greetings");
		greetings(dt);
	}

	// Mouths follow the loudness of what each actor is saying (a flap if there's no audio)
	for (LoadedCell* l : w.loaded)
	for (auto& a : l->actors.actors)
	{
		if (a.ref < 0)
			continue;                        // detached (World::detachActor): not drawn, nobody's mouth
		Ref& r = w.refs[a.ref];
		float level = 0.0f;
		if (r.voiceChannel >= 0 && !audioPlaying(r.voiceChannel))
			r.voiceChannel = -1;             // (a finished line's channel goes to other sounds: stopping it later would cut them)
		if (r.voiceChannel >= 0)
			level = audioLoudness(r.voiceChannel);
		else if (!sayDone(a.ref))
			level = 0.2f + 0.15f * sinf(w.time * 14.0f);
		a.talkLevel += (level - a.talkLevel) * fminf(1.0f, dt * 20.0f);
	}

	// Footsteps while walking; OpenMW's Npc::getSoundIdFromSndGen picks the sound: none flying, "Swim" while
	// swimming, "FootWater" with the feet under the surface, else the ground's
	float moving = fabsf(in.moveX) + fabsf(in.moveY);
	bool wading = false;
	if (w.current >= 0)
	{
		Cell& stepCell = w.here();
		wading = stepCell.hasWater() && w.player.feet[2] < stepCell.waterZ;
	}
	if (!menu && !w.player.flying && (w.player.swimming || w.player.onGround) && moving > 0.2f)
	{
		stepTimer -= dt;
		if (stepTimer <= 0.0f)
		{
			const char* side = stepLeft ? "Left" : "Right";
			playSound(-1, std::string(w.player.swimming ? "Swim " : wading ? "FootWater" : "FootBare") + side);
			stepLeft = !stepLeft;
			stepTimer = w.player.swimming ? 0.65f : 0.42f;
		}
	}
	else
		stepTimer = 0.1f;

	updateAmbience();
	updateLoopSounds();
	updateWeatherEffects(dt);
	updateMusic(dt);
	updateSky();
	updateDress(dt);
	// The region's ambient sounds outdoors in fair weather (Morrowind.ini: 1 to 5 s between; each by its chance)
	if (w.current >= 0 && !w.cells[w.current].interior && w.weatherNow <= 3 && (ambientTimer -= dt) <= 0.0f)
	{
		ambientTimer = 1.0f + (rand() % 400) / 100.0f;
		auto rg = w.game.regions.find(w.cells[w.current].region);
		if (rg != w.game.regions.end() && !rg->second.ambient.empty())
		{
			int total = 0;
			for (auto& a : rg->second.ambient)
				total += a.second;
			int roll = rand() % 100;
			if (roll < total)
				for (auto& a : rg->second.ambient)
				{
					if (roll < a.second)
					{
						playSound(-1, a.first);
						break;
					}
					roll -= a.second;
				}
		}
	}
	// A held light burns down (OpenMW's updateEquippedLight): its time left (the stack's charge; -1 a fresh one) less the
	// frame's; one that burns out is gone, and the next of the stack is lit
	for (size_t i = 0; i < w.inventory.size(); i++)
	{
		InventoryItem& it = w.inventory[i];
		const Object* o = it.equipped ? w.game.object(it.id) : nullptr;
		if (!o || o->type != "LIGH" || o->lightTime <= 0.0f)
			continue;
		float left = (it.charge < 0.0f ? o->lightTime : it.charge) - dt;
		if (left > 0.0f)
		{
			it.charge = left;
			break;
		}
		logf("light: %s burned out", it.id.c_str());
		it.charge = -1.0f;
		if (--it.count <= 0)
			w.inventory.erase(w.inventory.begin() + i);
		break;
	}
	{
		// A carried light lights the way (the equipped one, held at the hand)
		float radius = 0.0f, rgb[3] = { 0, 0, 0 };
		for (auto& it : w.inventory)
			if (it.equipped)
				if (const Object* o = w.game.object(it.id))
					if (o->type == "LIGH" && o->lightRadius > 0)
					{
						radius = (float)o->lightRadius;
						for (int k = 0; k < 3; k++)
							rgb[k] = o->lightColor[k] / 255.0f * 0.8f;
						break;
					}
		float at[3] = { w.player.feet[0] + sinf(w.player.yaw) * 20.0f, w.player.feet[1] + cosf(w.player.yaw) * 20.0f,
			w.player.feet[2] + 100.0f };
		rendererSetCarriedLight(at, radius, rgb);
	}
	// (people's torches light only people, not the world around: the console can't spare the vertex work; the player's
	// light is the one live light)
	{
		// People are lit by the lamps near where they stand now and by the torches others hold out near them (the
		// converter lit them where they were placed): the light on a standing body's facing side, 1 / pi of it, at
		// OpenMW's falloff (convert_cell.py light_tint, light_falloff), x 64 as their tint. CPU only: one tint a person
		struct Lamp { float pos[3]; float radius; float rgb[3]; int holder; };
		static std::vector<Lamp> lamps;
		lamps.clear();
		for (LoadedCell* l : w.loaded)
			for (int i : l->lightRefs)
			{
				const Ref& r = w.refs[i];
				if (!r.visible() || !r.obj)
					continue;
				float sign = (r.obj->flags & 4) ? -1.0f : 1.0f;      // (LHDT 0x4: a negative light)
				Lamp L = { { r.pos[0], r.pos[1], r.pos[2] }, (float)r.obj->lightRadius,
					{ sign * r.obj->lightColor[0] / 255.0f, sign * r.obj->lightColor[1] / 255.0f, sign * r.obj->lightColor[2] / 255.0f }, -1 };
				lamps.push_back(L);
			}
		w.forLoadedActors([&](int i) {
			const Actor* a = w.actorOf(i);
			if (!a || !a->torchOut)
				return;
			const Ref& r = w.refs[i];
			const Object* npc = w.game.object(r.id);
			for (size_t k = 0; npc && k < npc->items.size(); k++)
				if (const Object* o = w.game.object(npc->items[k].second))
					if (o->type == "LIGH" && o->lightRadius > 0 && (o->flags & 2))
					{
						Lamp L = { { r.pos[0], r.pos[1], r.pos[2] + 100.0f }, (float)o->lightRadius,
							{ o->lightColor[0] / 255.0f, o->lightColor[1] / 255.0f, o->lightColor[2] / 255.0f }, i };
						lamps.push_back(L);
						break;
					}
		});
		w.forLoadedActors([&](int i) {
			Actor* a = w.actorOf(i);
			if (!a)
				return;
			const Ref& r = w.refs[i];
			float at[3] = { r.pos[0], r.pos[1], r.pos[2] + 64.0f }, sum[3] = { 0.0f, 0.0f, 0.0f };
			for (const Lamp& L : lamps)
			{
				if (L.holder == i)
					continue;                   // (its own torch: Actor::heldLight)
				float d2 = 0.0f;
				for (int k = 0; k < 3; k++)
					d2 += (L.pos[k] - at[k]) * (L.pos[k] - at[k]);
				float bounds = kLightBounds * L.radius;
				if (d2 >= bounds * bounds)
					continue;
				float d = fmaxf(sqrtf(d2), 1.0f), x = fminf(fmaxf((d / bounds - 0.75f) / 0.25f, 0.0f), 1.0f);
				float f = (L.radius / (3.0f * d)) * (1.0f - x * x) * (1.0f - x * x) * (1.0f / 3.14159265f) * 128.0f;
				for (int k = 0; k < 3; k++)
					sum[k] += f * L.rgb[k];
			}
			for (int k = 0; k < 3; k++)
				a->lampNow[k] = fminf(fmaxf(sum[k], 0.0f), 255.0f);
		});
	}
	// spell visuals age and go
	// A hit visual is attached to its target's own node (OpenMW: Animation::addEffect), so it goes where the target goes
	for (size_t i = 0; i < vfx.size();)
		if ((vfx[i].age += dt) >= vfx[i].life && !vfx[i].world)
			vfx.erase(vfx.begin() + i);
		else
		{
			if (vfx[i].follow == -1)
			{
				memcpy(vfx[i].pos, w.player.feet, sizeof(vfx[i].pos));
				vfx[i].yaw = w.player.yaw;
			}
			else if (vfx[i].follow >= 0 && vfx[i].follow < (int)w.refs.size())
			{
				memcpy(vfx[i].pos, w.refs[vfx[i].follow].pos, sizeof(vfx[i].pos));
				vfx[i].yaw = w.refs[vfx[i].follow].rot[2];
			}
			if (!vfx[i].bone.empty())
				vfxBonePos(vfx[i].follow, vfx[i].bone, vfx[i].pos);   // (else it stays where it was last)
			if (vfx[i].until >= 0.0f && vfx[i].age > vfx[i].until)
				vfx[i].age = vfx[i].until;
			i++;
		}
	if (!playerDead && w.stats.health <= 0.0f)
		playerDies();
	// fades back to clear after travel; scripts can hold it (FadeOut) or bring it back slowly (FadeIn)
	if (fade > fadeTarget)
		fade = fmaxf(fadeTarget, fade - dt * fadeRate);
	else if (fade < fadeTarget)
		fade = fminf(fadeTarget, fade + dt * fadeRate);
	if (fade == fadeTarget && fadeTarget == 0.0f)
		fadeRate = 2.5f;
	for (size_t i = 0; i < notes.size();)
		if (w.time > notes[i].until)
			notes.erase(notes.begin() + i);
		else
			i++;
	audioUpdate();
}
