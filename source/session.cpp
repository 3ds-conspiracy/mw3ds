#include <algorithm>
#include <array>
#include <functional>
#include <sys/stat.h>
#include <tex3ds.h>
#include <citro2d.h>
#include "distant.h"
#include "actors.h"
#include "session.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "audio.h"
#include "linear.h"
#include "log.h"

static const float kReach = 192.0f;          // iMaxActivateDist

bool Session::start(const char* dataDir, const std::string& startCell, const char* savePathIn)
{
	savePath = savePathIn;
	loadSettings();
	audioSetScales(effectsVolume / 100.0f, musicVolume / 100.0f);
	rendererLoadCaustics(dataDir);
	std::string savedCell = savePath && startCell.empty() ? World::savedCell(savePath) : "";
	if (!savedCell.empty())
	{
		if (!w.load(dataDir, savedCell))
			return false;
		if (w.cellIndex(savedCell) == w.current && w.applySave(savePath))
		{
			startGameScripts();          // saves from before Main ran get it now
			playMusic(false);
			notify("Game loaded.");
			return true;
		}
		logf("save: could not continue in %s, starting a new game", savedCell.c_str());
		w.freeAll();
		w = World();
	}
	if (!w.load(dataDir, startCell))
		return false;
	giveItems(w.game.startItems);
	startGameScripts();
	if (!w.isStartCell() || !w.game.chargen)
	{
		skipChargen();
		playMusic(false);
		return true;
	}
	// State the boat part of character creation leaves behind (script "CharGen")
	w.globals["chargenstate"] = 10.0f;
	if (!autotest)
		playMovie("mw_intro");
	// CharGen's DisablePlayerControls / Jumping / ViewSwitch / VanityMode / Fighting / Magic: the player lies still in
	// the hold until the walking guard's script (CharGenWalkNPC) gives the controls back after the movement message
	w.controlsEnabled = false;
	w.jumpingEnabled = false;
	w.controlsOff |= 2u | 4u;
	w.fightingEnabled = false;
	w.magicEnabled = false;
	w.menusEnabled = 0;
	Scene scene = w.scene();
	playerSpawn(w.player, scene, w.here().spawn, w.here().yaw, 0.0f);
	// Waking in the prison ship's hold (the level's start cell, where CharGen's PositionCell puts the
	// player): Jiub's script asks the name, the guards' scripts bring the race menu and the way out.
	// A level built before the ship was in (start cell: the census office) still opens both menus
	if (lower(w.cellName()).find("prison ship") == std::string::npos)
	{
		pendingMenus.push_back(SMENU_NAME);
		pendingMenus.push_back(SMENU_RACE);
	}
	// the intro movie's sound plays on the music channel; the music starts when it ends (stopMovie)
	if (!moviePlaying())
		playMusic(false);
	return true;
}

bool Session::playMovie(const std::string& name)
{
	std::string stem = lower(name);
	size_t dot = stem.rfind(".bik");
	if (dot != std::string::npos)
		stem.resize(dot);
	for (auto& ch : stem)
		if (!isalnum((unsigned char)ch) && ch != '_')
			ch = '_';
	stopMovie();
	char path[256];
	snprintf(path, sizeof(path), "%s/movies/%s.mwv", w.dataDir, stem.c_str());
	FILE* f = testNoMovies ? nullptr : fopen(path, "rb");
	if (!f)
	{
		logf("movie: no %s", path);
		return false;
	}
	char magic[4];
	u32 hdr[4];
	if (fread(magic, 4, 1, f) != 1 || memcmp(magic, "MWV1", 4) != 0 || fread(hdr, 4, 4, f) != 4)
	{
		fclose(f);
		return false;
	}
	movieFile = f;
	movieFrames = hdr[0];
	movieFps100 = hdr[1] ? hdr[1] : 1000;
	movieW = hdr[2];
	movieH = hdr[3];
	movieFrame = -1;
	movieTime = 0.0f;
	std::string snd = "movie_" + stem + ".snd";
	char spath[256];
	snprintf(spath, sizeof(spath), "%s/music/%s", w.dataDir, snd.c_str());
	struct stat st;
	if (stat(spath, &st) == 0)
		audioMusicPlay({ snd }, 0.8f);
	logf("movie: %s, %u frames", stem.c_str(), movieFrames);
	return true;
}

void Session::movieUpdate(float dt)
{
	if (!movieFile)
		return;
	movieTime += dt;
	int want = (int)(movieTime * movieFps100 / 100.0f);
	if (want >= (int)movieFrames)
	{
		stopMovie();
		return;
	}
	// catch up to the frame due (skipping any a slow frame left behind)
	while (movieFrame < want)
	{
		u32 size = 0;
		if (fread(&size, 4, 1, movieFile) != 1 || size > (1 << 20))
		{
			stopMovie();
			return;
		}
		movieFrame++;
		if (movieFrame < want)
		{
			fseek(movieFile, size, SEEK_CUR);
			continue;
		}
		std::vector<u8> buf(size);
		if (fread(buf.data(), 1, size, movieFile) != size)
		{
			stopMovie();
			return;
		}
		LinearGuard guard;
		if (movieTexOk)
			deferredTexDelete(&movieTex);
		Tex3DS_Texture t = Tex3DS_TextureImport(buf.data(), size, &movieTex, nullptr, false);
		movieTexOk = t != nullptr;
		if (t)
		{
			Tex3DS_TextureFree(t);
			C3D_TexSetFilter(&movieTex, GPU_LINEAR, GPU_LINEAR);
		}
	}
}

void Session::movieDraw()
{
	if (!movieTexOk || !movieW || !movieH)
		return;
	Tex3DS_SubTexture sub = { (u16)movieW, (u16)movieH, 0.0f, 1.0f, (float)movieW / movieTex.width,
		1.0f - (float)movieH / movieTex.height };
	C2D_Image img = { &movieTex, &sub };
	float k = 240.0f / movieH;
	C2D_DrawImageAt(img, (400.0f - movieW * k) / 2.0f, 0.0f, 0.5f, nullptr, k, k);
}

void Session::stopMovie()
{
	if (movieFile)
	{
		fclose(movieFile);
		movieFile = nullptr;
		playMusic(false);
	}
	if (movieTexOk)
	{
		LinearGuard guard;
		deferredTexDelete(&movieTex);
		movieTexOk = false;
	}
}

// The game's own global scripts, as Morrowind starts them for a new game: Main always runs (it
// re-rolls Random100 every frame); at New Game it starts Startup (disables some hundred quest people
// and things until their quests bring them in) and VampireCheck (the blood diseases' third night).
// Main's CharGen start is left out: character creation here is the menus'. A save without Main
// (made before it ran) gets all three once.
void Session::startGameScripts()
{
	for (auto& s : w.scripts)
		if (s.ref < 0 && s.item.empty() && s.script && lower(s.script->name) == "main")
			return;
	logf("script: starting Main, Startup, VampireCheck");
	w.startGlobalScript("Main");
	w.startGlobalScript("Startup");
	auto pv = w.globals.find("pcvampire");
	if (pv == w.globals.end() || pv->second == 0.0f)
		w.startGlobalScript("VampireCheck");
}

// Starting somewhere other than the Census office: character creation counts as done, with
// the first playable class and birthsign, and the player stands outside a door into this cell
// (the office's exit door when there is one).
// Adds items to the inventory and wears / wields them (one weapon, clothes and armor)
// Morrowind's difficulty (OpenMW's reading): x = 1% of the slider; hits and harmful spells on the player
// scale by 1 + fDifficultyMult x (or / when easier), the player's own by the mirror of it
float Session::difficultyScale(bool toPlayer) const
{
	float term = 0.01f * difficulty, m = w.game.gmstf("fdifficultymult", 5.0f);
	float x = toPlayer ? (term > 0.0f ? m * term : term / m) : (term > 0.0f ? -term / m : m * -term);
	return fmaxf(0.0f, 1.0f + x);
}

// Third person: the camera 220 units behind and a little above the head, pulled in front of walls;
// the face preview looks at the player's face from in front
void Session::viewCamera(RenderCamera& cam)
{
	if (!thirdPerson && !previewFace)
		return;
	const Player& p = w.player;
	float fx = sinf(p.yaw), fy = cosf(p.yaw);
	float head[3] = { p.feet[0], p.feet[1], p.feet[2] + (previewFace ? 118.0f : 100.0f) };
	float want = previewFace ? 70.0f : 220.0f;
	float dir = previewFace ? 1.0f : -1.0f;       // the preview stands in front, facing back
	float cp = cosf(p.pitch), sp = sinf(p.pitch);
	float to[3] = { head[0] + dir * fx * cp * want, head[1] + dir * fy * cp * want, head[2] - dir * sp * want + (previewFace ? 0.0f : 20.0f) };
	// walls between the head and the camera pull it in
	float best = 1.0f;
	for (LoadedCell* l : w.loaded)
	{
		float t;
		if (collisionRaycast(l->cell.collision, head, to, &t) && t < best)
			best = t;
	}
	float d = fmaxf(20.0f, want * best - 12.0f);
	thirdDistance = thirdDistance <= 0.0f ? d : (d < thirdDistance ? d : thirdDistance + (d - thirdDistance) * 0.1f);
	for (int k = 0; k < 3; k++)
		cam.pos[k] = head[k] + (to[k] - head[k]) * (thirdDistance / want);
	if (previewFace)
	{
		cam.yaw = p.yaw + 3.14159265f;
		cam.pitch = 0.0f;
	}
}

void Session::giveItems(const std::vector<std::pair<int, std::string>>& items)
{
	for (auto& it : items)
	{
		const Object* o = w.game.object(it.second);
		if (!o)
			continue;
		w.addItem(it.second, it.first);
		bool wear = o->type == "CLOT" || o->type == "ARMO";
		if (o->type == "WEAP")
		{
			wear = true;
			for (auto& inv : w.inventory)
				if (inv.equipped && w.game.object(inv.id) && w.game.object(inv.id)->type == "WEAP")
					wear = false;
		}
		if (wear)
			for (auto& inv : w.inventory)
				if (inv.id == it.second)
					inv.equipped = true;
	}
}

// A new character's own spells: the PC Start spells their class and race make castable (autocalc)
void Session::givePlayerStartSpells()
{
	w.recomputeStats();
	int attrs[8], skills[27];
	for (int k = 0; k < 8; k++)
		attrs[k] = w.stats.attributes[k];
	for (int k = 0; k < 27; k++)
		skills[k] = w.stats.skills[k];
	int n = 0;
	for (auto& id : w.game.autoCalcSpells(attrs, skills, true))
	{
		std::string l = lower(id);
		if (std::find(w.stats.spells.begin(), w.stats.spells.end(), l) == w.stats.spells.end())
		{
			w.stats.spells.push_back(l);
			n++;
		}
	}
	logf("chargen: %d starting spells", n);
}

void Session::skipChargen()
{
	giveItems(w.game.skipChargenItems);
	w.globals["chargenstate"] = -1.0f;
	w.controlsEnabled = w.jumpingEnabled = w.fightingEnabled = w.magicEnabled = true;
	w.controlsOff = 0;
	w.menusEnabled = MENU_STATS | MENU_INVENTORY | MENU_MAGIC | MENU_MAP | MENU_REST;
	for (auto& c : w.game.classes)
		if (c.playable)
		{
			w.stats.cls = c.id;
			break;
		}
	if (!w.game.birthsigns.empty())
		w.stats.birthsign = w.game.birthsigns[0].id;
	w.recomputeStats();
	givePlayerStartSpells();

	// A level without character creation has its own starting spot; otherwise stand outside a door
	int door = -1;
	for (size_t i = 0; i < w.refs.size() && w.game.chargen; i++)
	{
		const Ref& r = w.refs[i];
		if (!r.hasDest || lower(r.destCell) != lower(w.cellName()) || r.cell == w.current)
			continue;
		if (door < 0 || r.idLower == "chargen exit door")
			door = i;
	}
	auto town = w.game.townSpawns.find(lower(w.cellName()));
	if (door < 0 && town != w.game.townSpawns.end())
	{
		// A town's street, in front of one of its doors (the converter picked it)
		const std::vector<float>& t = town->second;
		float eye[3] = { t[0], t[1], t[2] + PLAYER_EYE_HEIGHT };
		w.player.feet[0] = eye[0];
		w.player.feet[1] = eye[1];
		w.streamExterior(true);
		Scene scene = w.scene();
		playerSpawn(w.player, scene, eye, t[3], 0.0f);
	}
	else if (door >= 0)
	{
		const Ref& r = w.refs[door];
		float eye[3] = { r.destPos[0], r.destPos[1], r.destPos[2] + PLAYER_EYE_HEIGHT };
		w.player.feet[0] = eye[0];
		w.player.feet[1] = eye[1];
		w.streamExterior(true);
		Scene scene = w.scene();
		playerSpawn(w.player, scene, eye, r.destRot[2], 0.0f);
	}
	else
	{
		Scene scene = w.scene();
		playerSpawn(w.player, scene, w.here().spawn, w.here().yaw, 0.0f);
	}
	logf("start: %s, chargen skipped, feet %.0f %.0f %.0f", w.cellName().c_str(),
		w.player.feet[0], w.player.feet[1], w.player.feet[2]);
}

void Session::shutdown()
{
	if (waterChannel >= 0)
		audioStop(waterChannel);
	waterChannel = -1;
	for (auto& r : w.refs)
		if (r.voiceChannel >= 0)
			audioStop(r.voiceChannel);
	vm.free(w);
	body.free(w);
	w.freeAll();
	// what outlives a session in globals: freed now, before a loaded save's session reads its own (the
	// skeletons and sounds of two sessions at once ran out of memory loading a save). The distant land
	// stays: the next session reuses it (distantLoad), its ~20 MB read being the hard part of a reload.
	skeletonsFree();
	audioCacheClear();
	w.keepGame();
	if (mapLoaded)
	{
		LinearGuard guard;
		deferredTexDelete(&mapTex);
		mapLoaded = false;
	}
	freeMapTiles();
}

bool Session::save()
{
	if (!savePath)
		return false;
	bool ok = w.saveGame(savePath);
	notify(ok ? "Game saved." : "Saving failed.");
	return ok;
}

// Shore ambience: loops while the cell has water, louder the closer the player is to its level
void Session::updateAmbience()
{
	// Only open water counts: the whole world shares one water level, so Balmora's river is "sea"
	// too. The converter measured each cell's share of ground well under the water (a coast is a
	// third or more, a river a few percent); nearby cells over the threshold make the sound, fading
	// with distance.
	float sea = 0.0f;
	for (LoadedCell* l : w.loaded)
	{
		const LevelCell& c = w.cells[l->index];
		if (c.interior)
			continue;
		float dx = (c.gx + 0.5f) * 8192.0f - w.player.feet[0], dy = (c.gy + 0.5f) * 8192.0f - w.player.feet[1];
		float near = fmaxf(0.0f, 1.0f - sqrtf(dx * dx + dy * dy) / 12000.0f);
		sea = fmaxf(sea, fminf(1.0f, fmaxf(0.0f, (c.sea - 0.2f) / 0.3f)) * near);
	}
	bool want = sea > 0.02f && !w.game.waterSound.empty();
	if (!want)
	{
		if (waterChannel >= 0)
		{
			audioStop(waterChannel);
			waterChannel = -1;
		}
		return;
	}
	auto it = w.game.sounds.find(lower(w.game.waterSound));
	if (it == w.game.sounds.end())
		return;
	float height = w.player.feet[2] - w.here().waterZ;
	float volume = it->second.volume * sea * fmaxf(0.0f, fminf(1.0f, 1.0f - height / 900.0f)) * 0.7f;
	if (waterChannel < 0 || !audioPlaying(waterChannel))
		waterChannel = audioPlay(it->second.file, volume, 0.0f, true);
	else
		audioSetMix(waterChannel, volume, 0.0f);
}

// ---- Stereo 3D

// One eye's render time, smoothed over about a second (a stereo frame counts half per eye)
void Session::noteRenderCost(float cpuMs, float gpuMs, bool stereo, float eyeCmd)
{
	float cost = fmaxf(cpuMs, gpuMs) * (stereo ? 0.5f : 1.0f);
	renderCost += (cost - renderCost) * 0.05f;
	// GPU command buffer share of one eye: rises at once, falls slowly
	eyeCmdUse = eyeCmd > eyeCmdUse ? eyeCmd : eyeCmdUse + (eyeCmd - eyeCmdUse) * 0.02f;
}

// Distance to what the crosshair points at: the target when there is one, else the first wall / ground
// along the view (up to 3000)
float Session::aimDepth()
{
	const float kFar = 3000.0f;
	float cp = cosf(w.player.pitch);
	float dir[3] = { sinf(w.player.yaw) * cp, cosf(w.player.yaw) * cp, sinf(w.player.pitch) };
	float from[3] = { w.player.feet[0], w.player.feet[1], playerEyeZ(w.player) };
	float to[3] = { from[0] + dir[0] * kFar, from[1] + dir[1] * kFar, from[2] + dir[2] * kFar };
	float best = kFar;
	for (LoadedCell* l : w.loaded)
	{
		float t;
		if (collisionRaycast(l->cell.collision, from, to, &t))
			best = fminf(best, t * kFar);
	}
	if (target >= 0 && w.active(target))
	{
		const Ref& r = w.refs[target];
		float dx = r.pos[0] - from[0], dy = r.pos[1] - from[1];
		best = fminf(best, sqrtf(dx * dx + dy * dy));
	}
	return fmaxf(best, 20.0f);
}

// 0 or 1: whether this frame should be stereo: the player's choice (menu toggle, slider), whatever
// the frame rate; only a second eye that wouldn't fit the GPU command buffer waits
float Session::stereoTarget(float slider, float dt)
{
	(void)dt;
	if (slider <= 0.0f || stereoMode == STEREO_OFF || w.current < 0)
		return 0.0f;
	// A second eye has to fit in the frame's GPU command buffer next to the first (and the HUDs)
	if (!stereoCmdRoom && eyeCmdUse < 0.3f)
		stereoCmdRoom = true;
	else if (stereoCmdRoom && eyeCmdUse > 0.4f)
		stereoCmdRoom = false;
	return stereoCmdRoom ? 1.0f : 0.0f;
}

// Outdoors, the fog (and with it what is drawn) closes in while a frame's work runs past 30 fps
// and eases back out once there is room again; indoors the view stays whole
void Session::adaptView(float workMs)
{
	// One long frame (a cell arriving) shouldn't shrink the view: each counts at most as a slow one
	workCost += (fminf(workMs, 45.0f) - workCost) * 0.1f;
	if (w.current < 0 || w.cells[w.current].interior)
	{
		viewScale = 1.0f;
		return;
	}
	if (workCost > 34.0f)
		viewScale = fmaxf(kMinViewScale, viewScale - 0.004f);
	else if (workCost < 30.0f)
		viewScale = fminf(1.0f, viewScale + 0.002f);
}

static const char* kSettingsPath = "sdmc:/3ds/mw3ds/settings.txt";
int g_controlLayout = CONTROLS_XBOX;

void Session::loadSettings()
{
	FILE* f = fopen(kSettingsPath, "r");
	if (!f)
		return;
	char line[64];
	while (fgets(line, sizeof(line), f))
	{
		if (strncmp(line, "stereo=on", 9) == 0) stereoMode = STEREO_ON;
		else if (strncmp(line, "stereo=off", 10) == 0) stereoMode = STEREO_OFF;
		else if (strncmp(line, "stereo=auto", 11) == 0) stereoMode = STEREO_ON;
		else if (strncmp(line, "difficulty=", 11) == 0) difficulty = atoi(line + 11);
		else if (strncmp(line, "effects=", 8) == 0) effectsVolume = atoi(line + 8);
		else if (strncmp(line, "music=", 6) == 0) musicVolume = atoi(line + 6);
		else if (strncmp(line, "controls=classic", 16) == 0) g_controlLayout = CONTROLS_CLASSIC;
		else if (strncmp(line, "combat=gbac", 11) == 0) combatMode = COMBAT_GBAC;
		else if (strncmp(line, "numbers=on", 10) == 0) hitIndicators = true;
		else if (strncmp(line, "numbers=off", 11) == 0) hitIndicators = false;
		else if (strncmp(line, "combat=vanilla", 14) == 0) combatMode = COMBAT_VANILLA;
		else if (strncmp(line, "controls=xbox", 13) == 0) g_controlLayout = CONTROLS_XBOX;
	}
	fclose(f);
}

void Session::saveSettings()
{
	FILE* f = fopen(kSettingsPath, "w");
	if (!f)
		return;
	fprintf(f, "stereo=%s\n", stereoMode == STEREO_OFF ? "off" : "on");
	fprintf(f, "difficulty=%d\neffects=%d\nmusic=%d\n", difficulty, effectsVolume, musicVolume);
	fprintf(f, "controls=%s\n", g_controlLayout == CONTROLS_CLASSIC ? "classic" : "xbox");
	fprintf(f, "combat=%s\n", combatMode == COMBAT_GBAC ? "gbac" : "vanilla");
	fprintf(f, "numbers=%s\n", hitIndicators ? "on" : "off");
	fclose(f);
}

// Called by the main loop after it has shown the loading screen
void Session::finishTravel()
{
	int dest = travelCell;
	travelCell = -1;
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
	fade = 1.0f;
	if (!autotest)
		save();
	logf("travel: %s in %llu ms, feet %.0f %.0f %.0f", w.cellName().c_str(), osGetTime() - start,
		w.player.feet[0], w.player.feet[1], w.player.feet[2]);
}

void Session::setName(const std::string& name)
{
	w.stats.name = name.empty() ? "Stranger" : name;
	wantName = false;
	logf("chargen: name %s", w.stats.name.c_str());
}

// What was typed on the system keyboard: the name of the spell / enchanted item being made (a cancel keeps the
// name), or the journal's search (empty: everything again)
void Session::setText(bool confirmed, const std::string& text)
{
	int kind = wantText;
	wantText = TEXT_NONE;
	if (!confirmed)
		return;
	bool blank = text.find_first_not_of(' ') == std::string::npos;
	if (kind == TEXT_MAKENAME && !blank)
	{
		makeName = text;
		logf("making: name %s", makeName.c_str());
	}
	else if (kind == TEXT_SEARCH)
	{
		journalSearch = blank ? "" : text;
		list2 = UiList();
		logf("journal: search \"%s\"", journalSearch.c_str());
	}
}

std::string Session::textNow()
{
	return wantText == TEXT_SEARCH ? journalSearch : makeName;
}

void Session::openScreen(Screen s)
{
	screen = s;
	list = UiList();
	list2 = UiList();
	grid = UiGrid();
	scroll = UiScroll();
	focus = 0;
	containerPutting = false;
	playSound(-1, "Menu Click");
}

void Session::closeScreen()
{
	if (screen == SCR_DIALOGUE || screen == SCR_BARTER || screen == SCR_TRAVEL || screen == SCR_SPELLS
		|| screen == SCR_TRAINING || screen == SCR_PERSUADE || screen == SCR_SPELLMAKE
		|| (screen == SCR_REPAIR && repairRef >= 0)
		|| (screen == SCR_ENCHANT && enchanterRef >= 0))
		dialogueClose(dlg, w);
	barterRef = -1;
	// Walking away from someone's pockets: one last chance they notice
	if (screen == SCR_CONTAINER && pickpocketing && containerRef >= 0 && pickpocketCaught(0.0f))
	{
		logf("pickpocket: caught leaving %s", w.refs[containerRef].id.c_str());
		say(containerRef, "", w.game.gmst("scaughtstealingmessage", "Hey he's stealing my stuff!"));
		crimeSeen(CRIME_PICKPOCKET, 0, containerRef), reportCrime(containerRef, (int)w.game.gmstf("icrimepickpocket", 25));
	}
	pickpocketing = false;
	screen = SCR_NONE;
	bookRef = containerRef = -1;
	bookItem.clear();
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

bool Session::refInUse(int ri) const
{
	if (ri == target || ri == enemyRef || ri == repairRef || ri == barterRef || ri == bookRef || ri == containerRef
		|| ri == arrestingGuard || ri == dlg.ref || sayUntil.count(ri) || w.followsPlayer(ri))
		return true;
	for (auto& p : projectiles)
		if (p.owner == ri)
			return true;
	return false;
}

void Session::evictFarCells(int budget)
{
	int n = w.evictCells(budget, [this](int ri) { return refInUse(ri); });
	if (n)
		logf("world: %d cells' objects out of memory, %d objects in memory, %d cells' changes kept",
			n, w.refsInMemory(), (int)w.cellStates.size());
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
	case SCR_SAVES: case SCR_OPTIONS: case SCR_CONTROLS: case SCR_GAMEMENU: case SCR_BOOK: case SCR_CONTAINER:
		return true;
	default:
		return false;
	}
}

void Session::update(const PlayerInput& inRaw, float dt)
{
	u64 t = profTick();
	MARK("world update");
	w.pcWeaponDrawn = weaponDrawn;
	w.update(dt);
	profAdd(PROF_WORLD, t);
	frameDt = dt;
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
		float run = runSpeedFor((int)speedAttr, (int)athletics);          // (formulas.cpp)
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
			in.lookX = in.lookY = 0.0f;
	}
	if (!w.jumpingEnabled)
		in.jump = false;
	if (w.controlsOff & 1u)
		in.lookX = in.lookY = 0.0f;        // DisablePlayerLooking
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
			float x = w.player.swimming ? 0.0f : fallDamage(h, w.stats.skills[20], w.effectTotal(9));   // (formulas.cpp)
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
				float cost = w.game.gmstf((std::string("ffatigue") + kind + "base").c_str(), defBase[k])
					+ playerLoad * w.game.gmstf((std::string("ffatigue") + kind + "mult").c_str(), defMult[k]);
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
	if (!menu && (uiIn().down & KEY_A))
	{
		uiConsume(KEY_A);
		if (target >= 0 && w.controlsEnabled)
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
	// spell visuals age and go
	for (size_t i = 0; i < vfx.size();)
		if ((vfx[i].age += dt) >= vfx[i].life)
			vfx.erase(vfx.begin() + i);
		else
			i++;
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

// ---- Activation

void Session::playerActivate(int ref)
{
	Ref& r = w.refs[ref];
	// Scripts that test OnActivate take over; they call Activate for the default action. One that stopped asking
	// (a door script that finished, like the Census office's exit) lets the activation through (OpenMW)
	if (r.script >= 0 && w.scripts[r.script].script->usesOnActivate && w.scripts[r.script].suppress)
	{
		w.scripts[r.script].activated = true;
		w.scripts[r.script].buffered = true;
		return;
	}
	activate(ref);
}

// A werewolf can't use things (OpenMW's sWerewolfRefusal)
bool Session::werewolfRefused()
{
	if (!werewolf)
		return false;
	notify(w.game.gmst("swerewolfrefusal", "You cannot use items while in werewolf form."));
	return true;
}

// Reading a book from the pack in a fight is refused (a book on the ground may still be read)
bool Session::readRefused()
{
	if (!enemiesNear())
		return false;
	notify(w.game.gmst("sinventorymessage4", "You cannot read during battle."));
	return true;
}

// ShowRestMenu on a bed (OpenMW's sleepInBed): a werewolf and enemies nearby are refused; an owned bed is a trespass, and
// when someone saw it and reports it the sleep is refused too
bool Session::bedRefused(int bed)
{
	if (werewolf)
	{
		notify(w.game.gmst("swerewolfrefusal", "You cannot use items while in werewolf form."));
		return true;
	}
	if (enemiesNear())
	{
		notify(w.game.gmst("snotifymessage2", "You can't rest here enemies are nearby."));
		return true;
	}
	if (!w.ownedByOther(bed))
		return false;
	int witness = crimeWitness();
	if (witness < 0)
		return false;
	crimeSeen(CRIME_TRESPASS, 0, -1);
	reportCrime(witness, crimeBounty(CRIME_TRESPASS, 0));
	notify(w.game.gmst("snotifymessage64", "You can't sleep in someone else's bed!"));
	return true;
}

void Session::activate(int ref)
{
	Ref& r = w.refs[ref];
	if (!r.visible())
		return;
	// A werewolf is refused by everything but doors (OpenMW's getWerewolfRefusalAction)
	if (r.type != "DOOR" && werewolfRefused())
		return;
	// (a light that can't be carried is part of the scenery: nothing happens)
	if (r.type == "LIGH" && r.obj && !(r.obj->flags & 2))
		return;
	if ((r.type == "NPC_" || r.type == "CREA") && r.dead)
	{
		// Search the body
		containerRef = ref;
		openScreen(SCR_CONTAINER);
		return;
	}
	// Creatures that trade or have words of their own are talked to like people (Creeper, the mudcrab
	// merchant, Vivec, Yagrum Bagarn); other living creatures do nothing
	bool talkingCreature = r.type == "CREA" && r.actor >= 0 && r.ai != AI_COMBAT
		&& ((w.game.actors[r.actor].services & 0x3FFFF) || w.game.speakers.count(r.idLower));
	if (r.type == "CREA" && !r.dead)
		logf("activate: creature %s: services %x, speaker %d, ai %d -> %s", r.idLower.c_str(),
			r.actor >= 0 ? w.game.actors[r.actor].services : 0, (int)w.game.speakers.count(r.idLower), r.ai,
			talkingCreature ? "talk" : "nothing");
	if (r.type == "NPC_" && r.ai == AI_COMBAT)
	{
		notify(w.game.gmst("sactorincombat", "This character is in combat."));
		return;
	}
	if (r.type == "CREA" && !talkingCreature)
		return;
	// Knocked down (not fighting): their pockets are open to anyone, no roll against them. Sneaking up on someone: their
	// pockets, with the pickpocket rolls (OpenMW checks both before talking)
	if (r.type == "NPC_" && r.knockTimer > 0.0f && w.actorEffect(ref, 45) <= 0.0f)
	{
		containerRef = ref;
		pickpocketing = false;
		openScreen(SCR_CONTAINER);
		return;
	}
	if (r.type == "NPC_" && w.player.sneaking && !r.ally)
	{
		containerRef = ref;
		pickpocketing = true;
		openScreen(SCR_CONTAINER);
		return;
	}
	if (r.type == "NPC_" || talkingCreature)
	{
		// Face the speaker's head while talking
		float dx = r.pos[0] - w.player.feet[0], dy = r.pos[1] - w.player.feet[1];
		float dz = r.pos[2] + 115.0f - (w.player.feet[2] + PLAYER_EYE_HEIGHT);
		w.player.yaw = atan2f(dx, dy);
		w.player.pitch = atan2f(dz, fmaxf(1.0f, sqrtf(dx * dx + dy * dy)));
		// With no greeting that fits there is no conversation (OpenMW)
		if (dialogueStart(dlg, w, *this, ref))
		{
			wordsRevision = -1;
			openScreen(SCR_DIALOGUE);
		}
		return;
	}
	if (r.type == "DOOR")
	{
		useDoor(ref);
		return;
	}
	if (r.type == "CONT")
	{
		if (lockGate(ref, "LockedChest") != 2)
			return;
		containerRef = ref;
		openScreen(SCR_CONTAINER);
		// plants and other organic containers open without a chest's creak
		if (!r.obj || !r.obj->organic)
			playSound(ref, "chest open");
		return;
	}
	if (r.type == "BOOK")
	{
		openScreen(SCR_BOOK);
		bookRef = ref;
		playSound(-1, r.obj && r.obj->scroll ? "scroll" : "Book Open");
		return;
	}
	if (r.type == "ACTI" || r.type == "STAT")
		return;
	pickUp(ref);
}

void Session::pickUp(int ref)
{
	Ref& r = w.refs[ref];
	const std::string& t = r.type;
	const char* sound = "Item Misc Up";
	if (t == "BOOK") sound = "Item Book Up";
	else if (t == "ALCH") sound = "Item Potion Up";
	else if (t == "INGR") sound = "Item Ingredient Up";
	else if (t == "WEAP") sound = "Item Weapon Shortblade Up";
	else if (t == "CLOT") sound = "Item Clothes Up";
	else if (t == "ARMO") sound = "Item Armor Medium Up";
	else if (t == "LOCK") sound = "Item Lockpick Up";
	else if (t == "PROB") sound = "Item Probe Up";
	else if (t == "REPA") sound = "Item Repair Up";
	else if (t == "APPA") sound = "Item Apparatus Up";
	else if (r.idLower.rfind("gold_", 0) == 0) sound = "Item Gold Up";
	playSound(-1, sound);
	if (w.ownedByOther(ref))
		takeOwned(ref, r.id, r.count > 0 ? r.count : 1);
	w.pickUp(ref);
}

// The lock and the trap of a door or container, as OpenMW's Door::activate / Container::activate: the key (carried by the
// player) unlocks it, says so, and disarms a trap; with no key a locked one only gives its sound; an unlocked trapped one
// fires its trap instead of opening
int Session::lockGate(int ref, const char* lockedSound)
{
	Ref& r = w.refs[ref];
	bool locked = r.lockLevel > 0, trapped = !r.trap.empty() && !r.disarmed;
	bool key = !r.key.empty() && w.itemCount(lower(r.key)) > 0;
	if (locked && key)
	{
		const Object* k = w.game.object(r.key);
		notify((k ? k->name : r.key) + " " + w.game.gmst("skeyused", "was used to open the lock."));
		r.lockLevel = -r.lockLevel;       // (the level is kept for Lock)
		logf("lock: %s opened with %s", r.id.c_str(), r.key.c_str());
		if (trapped)
		{
			r.disarmed = true;            // the key disarms it too
			trapped = false;
			playSound(ref, "Disarm Trap");
		}
	}
	else if (locked)
	{
		playSound(ref, lockedSound);
		return 0;
	}
	if (trapped)
	{
		springTrap(ref);
		return 1;
	}
	return 2;
}

void Session::useDoor(int ref)
{
	Ref& r = w.refs[ref];
	if (lockGate(ref, "LockedDoor") != 2)
		return;
	if (r.hasDest)
	{
		int dest = r.destUnconverted ? -1 : r.destHasGrid ? w.gridCell(r.destGrid[0], r.destGrid[1]) : w.cellIndex(r.destCell);
		if (dest < 0 && !r.destEnd)
		{
			notify(r.destCell + " is not converted yet.");
			return;
		}
		if (r.obj && !r.obj->openSound.empty())
			playSound(-1, r.obj->openSound);
		if (r.destEnd)
		{
			ended = true;
			openScreen(SCR_END);
			return;
		}
		if (dest == w.current)
		{
			memcpy(w.player.feet, r.destPos, sizeof(r.destPos));
			w.player.yaw = r.destRot[2];
			w.player.pitch = 0.0f;
			w.player.vz = 0.0f;
			w.player.fallTop = w.player.feet[2];   // a door isn't a fall
			w.player.landedFall = 0.0f;
			w.player.inertia[0] = w.player.inertia[1] = 0.0f;
			fade = 1.0f;
		}
		else
		{
			travelCell = dest;
			memcpy(travelPos, r.destPos, sizeof(travelPos));
			travelYaw = r.destRot[2];
		}
		return;
	}
	bool opening = r.doorTarget == 0.0f;
	r.doorTarget = opening ? 1.5708f : 0.0f;
	if (r.obj)
		playSound(ref, opening ? r.obj->openSound : r.obj->closeSound);
}

std::string Session::targetLabel(int ref)
{
	Ref& r = w.refs[ref];
	std::string name = r.obj ? r.obj->name : r.id;
	if (r.actor >= 0)
		name = w.game.actors[r.actor].name + (r.dead ? " (dead)" : "");
	if (r.type == "DOOR" && r.hasDest && lower(r.destCell) != lower(w.cellName()))
		name += " - " + r.destCell;
	if (r.lockLevel > 0)
		name += " - Locked";
	if (r.count > 1)
		name += " (" + std::to_string(r.count) + ")";
	return name;
}

// ---- Sound

void Session::mix3d(const float* pos, float& volume, float& pan, float minDist, float maxDist)
{
	float dx = pos[0] - listener[0], dy = pos[1] - listener[1], dz = pos[2] - listener[2];
	float dist = sqrtf(dx * dx + dy * dy + dz * dz);
	float att = dist <= minDist ? 1.0f : fmaxf(0.0f, 1.0f - (dist - minDist) / fmaxf(1.0f, maxDist - minDist));
	volume *= att;
	float rel = atan2f(dx, dy) - listener[3];
	pan = dist > 1.0f ? sinf(rel) * 0.8f : 0.0f;
}

void Session::playSound(int ref, const std::string& soundId, float volume, float pitch, bool script)
{
	auto it = w.game.sounds.find(lower(soundId));
	if (it == w.game.sounds.end())
		return;
	volume *= it->second.volume;
	float pan = 0.0f;
	if (ref >= 0)
	{
		float pos[3] = { w.refs[ref].pos[0], w.refs[ref].pos[1], w.refs[ref].pos[2] + 50.0f };
		mix3d(pos, volume, pan, it->second.minRange * 20.0f, fmaxf(it->second.maxRange * 20.0f, 600.0f));
	}
	if (volume > 0.01f)
	{
		int ch = audioPlay(it->second.file, volume, pan, false, pitch);
		// A script's sound on a reference is "playing" till it ends
		if (script && ref >= 0 && ch >= 0 && scriptSounds.size() < 16)
			scriptSounds.push_back({ ref, lower(soundId), w.time + audioDuration(it->second.file) / fmaxf(pitch, 0.1f) });
	}
}

void Session::loopSound(int ref, const std::string& soundId, bool start, float volume, float pitch)
{
	std::string id = lower(soundId);
	for (size_t k = 0; k < loopSounds.size(); k++)
		if (loopSounds[k].ref == ref && loopSounds[k].id == id)
		{
			if (start)
				return;                            // already looping
			audioStop(loopSounds[k].channel);
			loopSounds.erase(loopSounds.begin() + k);
			return;
		}
	if (!start || ref < 0)
		return;
	auto it = w.game.sounds.find(id);
	if (it == w.game.sounds.end())
		return;
	// (no channel yet: updateLoopSounds gives it one while it can be heard)
	loopSounds.push_back({ ref, id, -1, volume, pitch });
	updateLoopSounds();
}

bool Session::soundPlaying(int ref, const std::string& soundId)
{
	std::string id = lower(soundId);
	for (auto& l : loopSounds)
		if (l.ref == ref && l.id == id)
			return true;
	for (size_t k = 0; k < scriptSounds.size();)
		if (w.time >= scriptSounds[k].until)
			scriptSounds.erase(scriptSounds.begin() + k);
		else if (scriptSounds[k].ref == ref && scriptSounds[k].id == id)
			return true;
		else
			k++;
	return false;
}

// A loop holds a sound channel only while it can be heard, and only the loudest few do: a town's fires or a
// Dwemer ruin's steam and machinery are dozens of loops, which at a channel each (silent or not) used up all
// of them and left every other sound without one
void Session::updateLoopSounds()
{
	static const size_t kMaxLoops = 12;
	for (size_t k = 0; k < loopSounds.size();)
	{
		LoopSound& l = loopSounds[k];
		if (!w.active(l.ref) || !w.refs[l.ref].visible())
		{
			audioStop(l.channel);
			loopSounds.erase(loopSounds.begin() + k);
			continue;
		}
		k++;
	}
	static std::vector<float> vols, pans, order;
	vols.assign(loopSounds.size(), 0.0f);
	pans.assign(loopSounds.size(), 0.0f);
	for (size_t k = 0; k < loopSounds.size(); k++)
	{
		LoopSound& l = loopSounds[k];
		auto it = w.game.sounds.find(l.id);
		float volume = (it != w.game.sounds.end() ? it->second.volume : 1.0f) * l.volume;
		float pos[3] = { w.refs[l.ref].pos[0], w.refs[l.ref].pos[1], w.refs[l.ref].pos[2] + 50.0f };
		if (it != w.game.sounds.end())
			mix3d(pos, volume, pans[k], it->second.minRange * 20.0f, fmaxf(it->second.maxRange * 20.0f, 600.0f));
		vols[k] = volume;
	}
	order = vols;
	float floorVol = 0.01f;
	if (order.size() > kMaxLoops)
	{
		std::sort(order.begin(), order.end(), std::greater<float>());
		floorVol = fmaxf(floorVol, order[kMaxLoops - 1]);
	}
	for (size_t k = 0; k < loopSounds.size(); k++)
	{
		LoopSound& l = loopSounds[k];
		bool want = vols[k] >= floorVol && vols[k] > 0.01f;
		if (l.channel >= 0 && !audioPlaying(l.channel))
			l.channel = -1;
		if (!want)
		{
			audioStop(l.channel);
			l.channel = -1;
		}
		else if (l.channel < 0)
		{
			auto it = w.game.sounds.find(l.id);
			if (it != w.game.sounds.end())
				l.channel = audioPlay(it->second.file, vols[k], pans[k], true, l.pitch);
		}
		else
			audioSetMix(l.channel, vols[k], pans[k]);
	}
}

// The weather's ambient loop outdoors (the one it's changing to past halfway), thunder in storms
C3D_Tex* Session::skyTex(const std::string& name)
{
	if (name.empty())
		return nullptr;
	auto it = skyTextures.find(name);
	if (it != skyTextures.end())
		return it->second;
	return skyTextures[name] = w.textures.acquire(w.dataDir, name);
}

// Where the sun and the moons are (OpenMW's reading of Morrowind's sky): the sun rises in the east at
// Sunrise Time and sets in the west at Sunset Time; each moon rises Daily Increment hours later every
// day (from 16 Last Seed), turns 15 x Speed degrees an hour across the sky tilted by Axis Offset, and
// goes through its 8 phases 3 days each, full on the first day; they fade in and out by the hour.
// Clouds, rain and storms hide them.
void Session::updateSky()
{
	g_skyBillboards.clear();
	g_starAlpha = 0.0f;
	if (w.current < 0 || w.cells[w.current].interior)
		return;
	const SkyBodiesDef& sky = w.game.skyBodies;
	GameDate d = w.date();
	float h = d.hour;
	float clear = w.weatherNow <= 1 ? 1.0f : w.weatherNow <= 3 ? 0.35f : 0.1f;   // clear / cloudy, foggy / overcast, worse
	auto toDir = [](float angleDeg, float tiltDeg, float out[3]) {
		float a = angleDeg * 0.0174533f, t = tiltDeg * 0.0174533f;
		out[0] = cosf(a);
		out[1] = -sinf(a) * sinf(t);
		out[2] = sinf(a) * cosf(t);
	};
	// the sun
	float st = (h - sky.sunrise) / fmaxf(1.0f, sky.sunset - sky.sunrise);
	if (st > -0.05f && st < 1.05f)
	{
		SkyBillboard sun = {};
		toDir(st * 180.0f, 30.0f, sun.dir);
		sun.size = 150.0f;
		sun.tex = skyTex(sky.sun);
		float edge = fminf(1.0f, fminf(st + 0.05f, 1.05f - st) * 8.0f);
		u8 a = (u8)(255 * clear * fmaxf(0.0f, edge));
		sun.rgba = 0x00FFFFFF | ((u32)a << 24);
		g_skyBillboards.push_back(sun);
		SkyBillboard glare = sun;
		glare.size = 420.0f;
		glare.tex = skyTex(sky.glare);
		glare.additive = true;
		glare.rgba = ((u32)(a * 0.45f) << 24) | 0x2F5FDE;     // Sun Glare Fader Color 222, 95, 39 (as ABGR)
		g_skyBillboards.push_back(glare);
	}
	// the moons
	for (int m = 0; m < 2; m++)
	{
		const MoonDef& md = sky.moons[m];
		auto riseHour = [&](int days) { return md.dailyIncrement + fmodf((days - 1 + 16) * md.dailyIncrement, 24.0f); };
		auto rotation = [&](float hours) { return 15.0f * md.speed * hours; };
		int day = d.daysPassed;
		float riseToday = riseHour(day), angle = 0.0f;
		if (h < riseToday)
		{
			float riseYesterday = riseHour(day - 1);
			if (riseYesterday < 24.0f)
				angle = rotation(h + 24.0f - riseYesterday);
		}
		else
			angle = rotation(h - riseToday);
		if (angle <= 0.0f || angle >= 180.0f)
			continue;
		int phase = h < riseToday ? (day / 3) % 8 : ((day + 1) / 3) % 8;
		float alpha = 0.0f;
		if (h >= md.fadeInFinish || h <= md.fadeOutStart)
			alpha = 1.0f;
		else if (h > md.fadeInStart && h < md.fadeInFinish)
			alpha = (h - md.fadeInStart) / fmaxf(0.01f, md.fadeInFinish - md.fadeInStart);
		else if (h > md.fadeOutStart && h < md.fadeOutFinish)
			alpha = 1.0f - (h - md.fadeOutStart) / fmaxf(0.01f, md.fadeOutFinish - md.fadeOutStart);
		alpha *= clear * fminf(1.0f, fminf(angle, 180.0f - angle) / 8.0f);
		if (alpha <= 0.01f)
			continue;
		SkyBillboard mb = {};
		toDir(angle, md.axisOffset, mb.dir);
		mb.size = md.size * 2.2f;
		mb.tex = skyTex(m == 0 ? sky.masser[phase] : sky.secunda[phase]);
		mb.rgba = 0x00FFFFFF | ((u32)(alpha * 255) << 24);
		g_skyBillboards.push_back(mb);
	}
	// the stars: from an hour after sunset to two before sunrise, fading over two hours
	float night = 0.0f;
	float startH = sky.sunset + 1.0f, endH = sky.sunrise - 2.0f;
	if (h >= startH)
		night = fminf(1.0f, (h - startH) / 2.0f);
	else if (h <= endH)
		night = 1.0f;
	else if (h < endH + 2.0f)
		night = 1.0f - (h - endH) / 2.0f;
	g_starAlpha = night * clear;
	g_starTexture = skyTex(sky.stars);
}

void Session::updateWeatherEffects(float dt)
{
	bool outside = w.current >= 0 && !w.cells[w.current].interior && !w.game.weatherTypes.empty();
	int wt = w.weatherShown();       // (OpenMW: the old weather's loops until the new one's rain threshold)
	std::string want;
	if (outside && wt < (int)w.game.weatherTypes.size())
	{
		want = w.game.weatherTypes[wt].sound;
		if (!want.empty() && !w.game.sounds.count(want))
			want = w.game.sounds.count("ashstorm") && (wt == WEATHER_BLIGHT || wt == WEATHER_ASH) ? "ashstorm" : "";
	}
	if (want != weatherSound || (weatherChannel >= 0 && !audioPlaying(weatherChannel)))
	{
		if (weatherChannel >= 0)
			audioStop(weatherChannel);
		weatherChannel = -1;
		weatherSound = want;
		auto it = w.game.sounds.find(want);
		if (it != w.game.sounds.end())
			weatherChannel = audioPlay(it->second.file, it->second.volume * 0.7f, 0.0f, true);
	}
	if (outside && wt == WEATHER_THUNDER && (thunderIn -= dt) <= 0.0f)
	{
		thunderIn = 8.0f + rand() % 20;
		playSound(-1, "thunder" + std::to_string(rand() % 4));
		fade = fmaxf(fade, 0.0f);            // (no flash: bright frames hurt on the 3D screen)
	}
}

// Rain (streaks) and ash / blight (drifting motes) over the top screen, as dense as the weather is in
void Session::drawPrecipitation()
{
	if (w.current < 0 || w.cells[w.current].interior || w.game.weatherTypes.empty())
		return;
	// OpenMW: the falling stuff fades out of the old weather up to the new one's rain threshold, then in
	float thr = w.game.weatherTypes[std::min(w.weatherNext, (int)w.game.weatherTypes.size() - 1)].rainThreshold;
	thr = thr > 0.0f ? thr : 0.5f;
	auto amount = [&](int kind) {
		if (w.weatherNow == w.weatherNext)
			return w.weatherNow == kind ? 1.0f : 0.0f;
		float out = fmaxf(0.0f, 1.0f - w.weatherBlend / thr), in = fmaxf(0.0f, (w.weatherBlend - thr) / (1.0f - thr));
		return (w.weatherNow == kind ? out : 0.0f) + (w.weatherNext == kind ? in : 0.0f);
	};
	float rain = amount(WEATHER_RAIN) + amount(WEATHER_THUNDER) * 1.5f;
	float ash = amount(WEATHER_ASH) + amount(WEATHER_BLIGHT);
	if (rain < 0.05f && ash < 0.05f)
		return;
	static float px[96], py[96];
	static bool seeded = false;
	// The drops move once a frame, not once an eye: each eye drew its own, and the two never matched (a
	// double image that hurts to look at). The same drops in both eyes sit flat on the HUD's plane
	bool step = hudEye == 0;
	if (!seeded)
	{
		for (int k = 0; k < 96; k++)
		{
			px[k] = rand() % 400;
			py[k] = rand() % 240;
		}
		seeded = true;
	}
	int n = (int)(fminf(1.5f, rain + ash) * 64.0f);
	for (int k = 0; k < n && k < 96; k++)
	{
		if (rain >= ash)
		{
			if (step)
			{
				py[k] += 9.0f;
				px[k] -= 1.5f;
			}
			uiRect(px[k], py[k], 1, 7, C2D_Color32(190, 200, 215, 120));
		}
		else
		{
			if (step)
			{
				py[k] += 1.5f;
				px[k] -= 3.5f;
			}
			uiRect(px[k], py[k], 2, 2, C2D_Color32(120, 90, 70, 150));
		}
		if (step && (py[k] > 240 || px[k] < 0))
		{
			px[k] = rand() % 420;
			py[k] = -(float)(rand() % 40);
		}
	}
}

void Session::playMusic(bool battle)
{
	std::vector<std::string> list = battle && !w.game.battleMusic.empty() ? w.game.battleMusic : w.game.music;
	// a different track first each time
	if (list.size() > 1)
		std::rotate(list.begin(), list.begin() + rand() % list.size(), list.end());
	battleMusic = battle && !w.game.battleMusic.empty();
	audioMusicPlay(list, battleMusic ? 0.35f : 0.3f);
}

// StreamMusic: a track from the music folder, when it was converted (the others aren't on the card)
void Session::streamMusic(const std::string& file)
{
	// the converter's names: music folder first, no extension, anything but letters and digits an underscore
	std::string stem = "music/" + lower(file);
	size_t dot = stem.rfind('.');
	if (dot != std::string::npos && stem.find('/', dot) == std::string::npos)
		stem.erase(dot);
	for (auto& ch : stem)
		if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')))
			ch = '_';
	stem += ".snd";
	FILE* f = fopen((std::string(w.dataDir) + "/music/" + stem).c_str(), "rb");
	if (!f)
	{
		logf("StreamMusic: %s not converted", stem.c_str());
		return;
	}
	fclose(f);
	battleMusic = false;
	audioMusicPlay({ stem }, 0.3f);
}

void Session::updateMusic(float dt)
{
	bool fighting = false;
	for (int i : w.loadedActors)
	{
		const Ref& r = w.refs[i];
		if (r.ai == AI_COMBAT && !r.dead && !r.ally && w.distanceToPlayer(i) < 3000.0f)
		{
			fighting = true;
			break;
		}
	}
	if (fighting)
	{
		calmFor = 0.0f;
		if (!battleMusic)
			playMusic(true);
	}
	else if (battleMusic && (calmFor += dt) > 5.0f)
		playMusic(false);
}

void Session::stopLoopSounds()
{
	for (auto& l : loopSounds)
		audioStop(l.channel);
	loopSounds.clear();
	scriptSounds.clear();
}

void Session::say(int ref, const std::string& file, const std::string& text)
{
	std::string key = lower(file);
	for (auto& c : key)
		if (c == '/')
			c = '\\';
	float duration = 0.0f;
	int& channel = ref >= 0 ? w.refs[ref].voiceChannel : uiVoiceChannel;
	if (channel >= 0)
	{
		if (audioPlaying(channel))        // (a finished line's channel may be another sound's by now)
			audioStop(channel);
		channel = -1;
	}
	auto it = w.game.voices.find(key);
	if (it != w.game.voices.end())
	{
		duration = audioDuration(it->second);
		float volume = 1.0f, pan = 0.0f;
		if (ref >= 0)
		{
			float pos[3] = { w.refs[ref].pos[0], w.refs[ref].pos[1], w.refs[ref].pos[2] + 110.0f };
			mix3d(pos, volume, pan, 300.0f, 2500.0f);
		}
		channel = audioPlay(it->second, volume, pan);
	}
	else if (!key.empty())     // (no file: a line of text only)
	{
		logf("voice missing: %s", key.c_str());
		monitorOnce(("voice:" + key).c_str(), "missing voice line %s", key.c_str());
	}
	if (duration <= 0.0f)
		duration = 1.0f + text.size() * 0.06f;
	if (ref >= 0)
	{
		sayUntil[ref] = w.time + duration;
		// A scripted or dialogue line also counts as a greeting
		w.refs[ref].helloTimer = fmaxf(w.refs[ref].helloTimer, duration + 20.0f);
	}
	if (!text.empty())
	{
		std::string who = ref >= 0 && w.refs[ref].actor >= 0 ? w.game.actors[w.refs[ref].actor].name + ": " : "";
		subtitle = who + text;
		subtitleUntil = w.time + duration + 0.5f;
	}
}

bool Session::sayDone(int ref)
{
	auto it = sayUntil.find(ref);
	return it == sayUntil.end() || w.time >= it->second;
}

// NPCs greet the player with a "Hello" voice line when they come close
void Session::greetings(float dt)
{
	const Topic* hello = w.game.topic("Hello");
	// Nobody greets during scripted scenes (controls taken away)
	if (!hello || !w.controlsEnabled)
		return;
	// OpenMW's updateGreetingState: an actor wandering, travelling or with no package, not in combat, swimming or
	// paralyzed, greets once when the player is within AI Hello x iGreetDistanceMultiplier, in sight and noticed; it
	// greets again only after the player has gone fGreetDistanceReset away
	float reset = w.game.gmstf("fgreetdistancereset", 512.0f);
	for (int i : w.loadedActors)
	{
		Ref& r = w.refs[i];
		if (r.actor < 0 || !w.active(i) || !r.visible() || r.dead)
			continue;
		float d = w.distanceToPlayer(i);
		if (r.greeted && d >= reset)
			r.greeted = false;
		if (r.greeted || r.ai == AI_COMBAT || r.helloTimer > 0.0f || !sayDone(i) || (w.actorOf(i) && w.actorOf(i)->swimming)
			|| (r.aiPackage != AIPKG_NONE && r.aiPackage != AIPKG_WANDER && r.aiPackage != AIPKG_TRAVEL
				&& r.aiPackage != AIPKG_IDLE)
			|| w.actorEffect(i, 45) > 0.0f || r.knockTimer > 0.0f)
			continue;
		int helloSetting = r.hello >= 0 ? r.hello : w.game.actors[r.actor].hello;
		if (helloSetting <= 0 || d > helloSetting * w.game.gmstf("igreetdistancemultiplier", 6.0f))
			continue;
		float head[3] = { r.pos[0], r.pos[1], r.pos[2] + 110.0f }, eye[3] = { w.player.feet[0], w.player.feet[1], playerEyeZ(w.player) };
		if (!w.lineOfSight(head, eye) || !awarenessCheck(i))
			continue;
		const Info* info = dialogueVoiced(w, *hello, i);
		r.greeted = true;
		r.helloTimer = 2.0f;
		if (info && !info->sound.empty())
			say(i, info->sound, dialogueSubstitute(w, info->text, i));
	}
}

// ---- ScriptHost

// The tutorial's console wording (Xbox returns 1), said with the 3DS's controls
static std::string for3ds(std::string t)
{
	bool xbox = g_controlLayout == CONTROLS_XBOX;
	const char* swap[][2] = {
		{ "The LEFT THUMBSTICK moves you, and the RIGHT THUMBSTICK looks around.",
		  "The Circle Pad moves you, and the C-Stick looks around (the D-Pad looks up and down)." },
		{ "Press B to use your menus. When you are done with them, press B again to close them.",
		  "Tap the buttons on the bottom screen to use your menus (START for saves). Press B to close them." },
		{ "Press the WHITE button to use your journal", "Tap Journal on the bottom screen to use your journal" },
		{ "Press the X button to pull out your weapon. Once your weapon is readied, hold and release the RIGHT TRIGGER to swing it.",
		  xbox ? "Equip your weapon, then press X to ready it, and hold and release R to swing it."
			: "Equip your weapon, then hold and release X to swing it." },
		{ "Equip the dagger by dropping it on your picture in your inventory menu.",
		  "Equip the dagger: tap Items on the bottom screen, then tap the dagger twice." },
		{ "Like all menus, press B to use your new menu. The triggers cycle between menus. Press B again to exit your menus.",
		  "Like all menus, tap it on the bottom screen; B closes it." },
		{ "Press Y to ready your Active Magic, then the RIGHT TRIGGER to cast.",
		  xbox ? "Choose your Active Magic in the Magic menu, then press L to cast it."
			: "Choose your Active Magic in the Magic menu, then press Y to cast it." },
		{ "Press BLACK to rest", "Press START and choose Rest / Wait to rest" },
		{ "Then select 'Take' to pick them up.", "Then tap Take to pick them up." },
	};
	for (auto& s : swap)
	{
		size_t at = t.find(s[0]);
		if (at != std::string::npos)
			t.replace(at, strlen(s[0]), s[1]);
	}
	return t;
}

void Session::messageBox(const std::string& text0, const std::vector<std::string>& buttons)
{
	std::string text = for3ds(text0);
	if (buttons.empty())
	{
		notify(text);
		return;
	}
	buttonPressed = -1;          // a new box: no answer yet
	messages.push_back({ text, buttons, true });
	logf("messagebox: %s", text.c_str());
}

int Session::takeButtonPressed()
{
	int b = buttonPressed;
	buttonPressed = -1;
	return b;
}

void Session::openMenu(ScriptMenu menu)
{
	pendingMenus.push_back(menu);
}

// ForceGreeting: the speaker opens the conversation, hostile or not (Dagoth Gares greets Nerevar before the
// fight) and without their OnActivate (that's the player's activation)
void Session::forceGreeting(int ref)
{
	if (ref < 0 || dlg.open || !w.active(ref))
		return;
	Ref& r = w.refs[ref];
	if (r.dead || !r.visible() || r.actor < 0)
		return;
	float dx = r.pos[0] - w.player.feet[0], dy = r.pos[1] - w.player.feet[1];
	float dz = r.pos[2] + 115.0f - (w.player.feet[2] + PLAYER_EYE_HEIGHT);
	w.player.yaw = atan2f(dx, dy);
	w.player.pitch = atan2f(dz, fmaxf(1.0f, sqrtf(dx * dx + dy * dy)));
	logf("dialogue: %s greets the player (ForceGreeting)", r.id.c_str());
	if (dialogueStart(dlg, w, *this, ref))
	{
		wordsRevision = -1;
		openScreen(SCR_DIALOGUE);
	}
}

void Session::dialogueChoice(const std::vector<std::pair<std::string, int>>& choices)
{
	// each Choice adds to the ones already offered
	dlg.choices.insert(dlg.choices.end(), choices.begin(), choices.end());
	dlg.revision++;
}

void Session::dialogueGoodbye()
{
	dlg.goodbye = true;
	dlg.choices.clear();         // (OpenMW) Goodbye ends any choice
	dlg.revision++;
}

void Session::notify(const std::string& text)
{
	notes.push_back({ text, w.time + 5.0f });
	if (notes.size() > 4)
		notes.erase(notes.begin());
	logf("notify: %s", text.c_str());
}

// ---- Test harness helpers (autoinput BOOST / GIVE / PLACE)

void Session::testBoost(int value)
{
	w.recomputeStats();
	for (int i = 0; i < 8; i++)
		w.stats.attrBonus[i] += value - w.stats.attributes[i];
	for (int k = 0; k < 27; k++)
		w.stats.skillBonus[k] += value - w.stats.skills[k];
	w.recomputeStats();
	logf("test: boost to %d, health %.0f fatigue %.0f", value, w.stats.healthMax, w.stats.fatigueMax);
}

static std::string underscoresToSpaces(std::string s)
{
	for (auto& ch : s)
		if (ch == '_')
			ch = ' ';
	return s;
}

int Session::testFindRef(const std::string& id)
{
	// the creature the test placed, even once it is dead: another with its id (a spawn in a loaded neighbour) is not it
	if (testPlaced >= 0 && w.active(testPlaced))
	{
		const std::string& pid = w.refs[testPlaced].idLower;
		if (pid == lower(id) || pid == lower(underscoresToSpaces(id)))
			return testPlaced;
	}
	int ref = w.findRef(id);
	if (!w.active(ref))
	{
		int spaced = w.findRef(underscoresToSpaces(id));
		if (w.active(spaced) || ref < 0)
			ref = spaced;
	}
	// a dead one when a living one shares the id (a creature placed again): the living one
	if (w.active(ref) && w.refs[ref].dead)
	{
		const std::string want = w.refs[ref].idLower;
		w.forLoadedRefs([&](int i) {
			if (w.refs[ref].dead && w.refs[i].idLower == want && !w.refs[i].dead)
				ref = i;
		});
	}
	return ref;
}

void Session::testGive(const std::string& id, int count, bool topUp)
{
	const Object* o = w.game.object(id);
	if (!o)
		o = w.game.object(underscoresToSpaces(id));
	if (!o)
	{
		logf("test: no item %s", id.c_str());
		return;
	}
	bool ammo = o->type == "WEAP" && o->subtype >= 12;
	int want = count > 0 ? count : ammo ? 50 : 1;
	// (a chained chapter: only up to the count, so an item the last chapter's story handed over isn't doubled)
	if (topUp)
		want -= w.itemCount(o->id);
	if (want > 0)
		w.addItem(o->id, want);
	bool wearable = o->type == "WEAP" || o->type == "ARMO" || o->type == "CLOT" || o->type == "LIGH"
		|| o->type == "LOCK" || o->type == "PROB";
	for (size_t k = 0; k < w.inventory.size() && wearable; k++)
		if (lower(w.inventory[k].id) == lower(o->id))
			equipItem(k, true);
	logf("test: gave and equipped %s", o->id.c_str());
}

void Session::testPlace(const std::string& id, float dist)
{
	int ri = testFindRef(id);
	float fx = sinf(w.player.yaw), fy = cosf(w.player.yaw);
	if (!w.active(ri) || w.refs[ri].dead)
	{
		// None to move (or only a corpse): a new one, as PlaceAtPC would
		ri = w.spawnActor(id, w.player.feet, atan2f(-fx, -fy));
		if (ri < 0)
		{
			logf("test: %s not loaded", id.c_str());
			return;
		}
	}
	Ref& r = w.refs[ri];
	Cell* cell = w.cellOf(ri);
	float to[3] = { w.player.feet[0] + fx * dist, w.player.feet[1] + fy * dist, w.player.feet[2] };
	float fz;
	if (cell && collisionFloor(cell->collision, to[0], to[1], to[2] + 200.0f, to[2] - 400.0f, &fz))
		to[2] = fz;
	for (int k = 0; k < 3; k++)
		r.pos[k] = r.home[k] = to[k];
	r.fitBox();
	r.rot[2] = atan2f(-fx, -fy);
	r.moved = true;
	w.syncActor(ri);
	testPlaced = ri;
	logf("test: placed %s at %.0f %.0f %.0f", r.id.c_str(), to[0], to[1], to[2]);
}

// Enters a cell through a door that leads there (any loaded door; else the first one found)
void Session::testGoto(const std::string& cellName)
{
	std::string want = lower(cellName);
	int door = -1;
	for (size_t i = 0; i < w.refs.size(); i++)
	{
		const Ref& r = w.refs[i];
		if (r.hasDest && lower(r.destCell) == want && (door < 0 || w.active(i)))
			door = i;
	}
	if (door < 0)
	{
		// No door read yet (cells' objects load on demand): straight in; a TP step places the player
		int dest = w.cellIndex(cellName);
		if (dest < 0)
		{
			logf("test: %s is not in the level", cellName.c_str());
			return;
		}
		travelCell = dest;
		travelPos[0] = travelPos[1] = travelPos[2] = 0.0f;
		travelYaw = 0.0f;
		// an exterior cell: its middle (the position decides which cells load outdoors), not world 0 0
		if (!w.cells[dest].interior)
		{
			travelPos[0] = (w.cells[dest].gx + 0.5f) * 8192.0f;
			travelPos[1] = (w.cells[dest].gy + 0.5f) * 8192.0f;
			travelToSpawn = true;
			// a town: its street, in front of a door (the converter's pick, as a new game there starts), not the
			// cell's middle (Seyda Neen's is out in the bay)
			auto town = w.game.townSpawns.find(want);
			int at = town != w.game.townSpawns.end()
				? w.gridCell((int)floorf(town->second[0] / 8192.0f), (int)floorf(town->second[1] / 8192.0f)) : -1;
			if (at >= 0)
			{
				travelCell = at;
				memcpy(travelPos, town->second.data(), sizeof(travelPos));
				travelYaw = town->second[3];
				travelToSpawn = false;
				logf("test: going into %s (its street)", cellName.c_str());
				return;
			}
		}
		// where a door into it lands (converter: cell_entries.txt), not the cell's origin (the void, often)
		static std::unordered_map<std::string, std::array<float, 4>> entries;
		if (entries.empty())
		{
			char path[256];
			snprintf(path, sizeof(path), "%s/cell_entries.txt", w.dataDir);
			if (FILE* f = fopen(path, "r"))
			{
				char line[320];
				while (fgets(line, sizeof(line), f))
				{
					char* tab = strchr(line, '	');
					std::array<float, 4> e;
					if (tab && sscanf(tab + 1, "%f %f %f %f", &e[0], &e[1], &e[2], &e[3]) == 4)
						entries[std::string(line, tab - line)] = e;
				}
				fclose(f);
			}
		}
		auto e = entries.find(want);
		if (e != entries.end())
		{
			memcpy(travelPos, e->second.data(), sizeof(travelPos));
			travelYaw = e->second[3];
			logf("test: going into %s (where its door lands)", cellName.c_str());
			return;
		}
		logf("test: going straight into %s", cellName.c_str());
		return;
	}
	const Ref& r = w.refs[door];
	int dest = r.destHasGrid ? w.gridCell(r.destGrid[0], r.destGrid[1]) : w.cellIndex(r.destCell);
	if (dest < 0)
	{
		logf("test: %s is not in the level", cellName.c_str());
		return;
	}
	travelCell = dest;
	memcpy(travelPos, r.destPos, sizeof(travelPos));
	travelYaw = r.destRot[2];
	logf("test: going to %s", cellName.c_str());
}

void Session::noteHudMismatch(int left, int right)
{
	monitorOnce(("hud:" + std::to_string((int)screen) + ":" + std::to_string(dlg.open)).c_str(),
		"the HUD drew %d things in the left eye and %d in the right (screen %d, dialogue %d, %s): one eye is missing something",
		left, right, (int)screen, (int)dlg.open, w.cellName().c_str());
}

// Engine-side checks, run in every test (and the game): a "monitor:" line each, once per actor or place.
// Floating or sunk actors, actors going nowhere, the player under the world, doors that lead nowhere,
// linear memory that shrinks each time the same cell is entered.
void Session::monitorActors(float dt)
{
	monitorTimer += dt;
	if (monitorTimer < 1.0f)
		return;
	float step = monitorTimer;
	monitorTimer = 0.0f;
	if (monitorCell != w.current)
		monitorCellChanged();
	for (int i : w.loadedActors)
	{
		const Ref& r = w.refs[i];
		// Out of the crosshair's reach: the box it's aimed at isn't where the actor stands (no talking to
		// them, nor searching the body)
		if (r.visible() && (!r.hasBox || r.pos[0] < r.boxMin[0] || r.pos[0] > r.boxMax[0] || r.pos[1] < r.boxMin[1]
				|| r.pos[1] > r.boxMax[1]))
			monitorOnce(("aim:" + r.idLower + ":" + w.cellName()).c_str(),
				"%s can't be aimed at in %s: at %.0f %.0f %.0f, its box %s %.0f %.0f .. %.0f %.0f", r.id.c_str(),
				w.cellName().c_str(), r.pos[0], r.pos[1], r.pos[2], r.hasBox ? "" : "(none)", r.boxMin[0], r.boxMin[1],
				r.boxMax[0], r.boxMax[1]);
		Actor* a = r.dead || !r.visible() ? nullptr : w.actorOf(i);
		if (!a)
			continue;
		// Drawn, or in the way, somewhere other than where it stands: another copy of its position that
		// whatever moved it (a cell's state, a save, a script) didn't update
		{
			const Cell* cell = w.cellOf(i);
			bool hasBlock = cell && r.cellActor >= 0 && r.cellActor < (int)cell->actors.size();
			float blocks[2] = { 0.0f, 0.0f };     // (copied: the record is packed)
			if (hasBlock)
				memcpy(blocks, (const void*)&cell->actors[r.cellActor].pos, sizeof(blocks));
			float ddx = a->place[3] - r.pos[0], ddy = a->place[7] - r.pos[1];
			if (ddx * ddx + ddy * ddy > 4.0f * 4.0f)
				monitorOnce(("drawn:" + r.idLower + ":" + w.cellName()).c_str(),
					"%s is drawn at %.0f %.0f but stands at %.0f %.0f in %s", r.id.c_str(), a->place[3], a->place[7],
					r.pos[0], r.pos[1], w.cellName().c_str());
			if (hasBlock && ((blocks[0] - r.pos[0]) * (blocks[0] - r.pos[0]) + (blocks[1] - r.pos[1]) * (blocks[1] - r.pos[1])
					> 4.0f * 4.0f))
				monitorOnce(("blocks:" + r.idLower + ":" + w.cellName()).c_str(),
					"%s blocks the way at %.0f %.0f but stands at %.0f %.0f in %s", r.id.c_str(), blocks[0], blocks[1],
					r.pos[0], r.pos[1], w.cellName().c_str());
		}
		const Skeleton& sk = actorSkeleton(*w.actorsOf(i), a->skeleton);
		const char* g = a->group < (int)sk.groups.size() ? sk.groups[a->group].name : "";
		bool cycle = a->mode == ANIM_LOOP && (strstr(g, "Walk") || strstr(g, "Run"));
		Watch& wt = watches[i];
		float mx = r.pos[0] - wt.x, my = r.pos[1] - wt.y;
		wt.x = r.pos[0];
		wt.y = r.pos[1];
		wt.still = cycle && mx * mx + my * my < 15.0f * 15.0f ? wt.still + step : 0.0f;
		if (wt.still >= 4.0f && !wt.told)
		{
			wt.told = true;
			logf("monitor: %s walks in place (%s) at %.0f %.0f %.0f%s [ai %d, package %d, fleeing %d, still %.1f, hitAt %.1f, "
				"player %.0f away, %.0f up]", r.id.c_str(), g, r.pos[0], r.pos[1], r.pos[2],
				dlg.open && dlg.ref == i ? ", while talking" : "", (int)r.ai, (int)r.aiPackage, (int)r.fleeing, r.chaseStill,
				r.hitAt, w.distanceToPlayer(i), w.player.feet[2] - r.pos[2]);
		}

		// Off the floor: an idle person more than 8 units above the floor under their feet or 20 below it
		// (creatures that fly, or swim while in water, hang where they are, so they're only checked for sinking),
		// for 2 s
		bool creature = r.actor >= 0 && w.game.actors[r.actor].creature;
		if (creature)
		{
			u8 afloat = w.game.actors[r.actor].afloat;
			creature = (afloat & 0x20) || ((afloat & 0x10) && w.underWater(r.pos[2]));
		}
		bool idle = r.ai != AI_COMBAT && !r.falling && r.knockTimer <= 0.0f && !a->swimming;
		// (on the floor anywhere from the floor under the middle up to the highest under the feet: a step and
		// gravity stand them on the latter, as OpenMW's actor box rests on a slope or a ledge's edge)
		float floorZ = -1e9f, footZ = -1e9f;
		if (idle)
			for (LoadedCell* l : w.loaded)
			{
				float z;
				if (collisionFloor(l->cell.collision, r.pos[0], r.pos[1], r.pos[2] + 40.0f, r.pos[2] - 6000.0f, &z) && z > floorZ)
					floorZ = z;
				if (collisionFootFloor(l->cell.collision, r.pos[0], r.pos[1], kActorFootReach, r.pos[2] + 40.0f, r.pos[2] - 6000.0f,
						&z) && z > footZ)
					footZ = z;
			}
		float gap = floorZ > -1e8f ? r.pos[2] - floorZ : 0.0f;
		if (gap > 0.0f)
			gap = fmaxf(0.0f, r.pos[2] - footZ);
		bool off = idle && ((floorZ < -1e8f && !creature) || gap < -20.0f || (gap > 8.0f && !creature));
		wt.off = off ? wt.off + step : 0.0f;
		if (wt.off >= 2.0f && !wt.toldFloor)
		{
			wt.toldFloor = true;
			if (floorZ < -1e8f)
				logf("monitor: %s has no floor under it at %.0f %.0f %.0f in %s", r.id.c_str(), r.pos[0], r.pos[1], r.pos[2],
					w.cellName().c_str());
			else
				logf("monitor: %s is %.0f units %s the floor at %.0f %.0f %.0f in %s", r.id.c_str(), fabsf(gap),
					gap > 0 ? "above" : "below", r.pos[0], r.pos[1], r.pos[2], w.cellName().c_str());
		}

		// Going somewhere and getting nowhere: a travel / follow / escort package, or a fight, with under 15
		// units of net movement in 5 s (next to the player is where a follower or a fighter stops)
		// (a scripted follower that hasn't seen its leader yet stands, as in OpenMW; a follower stops within
		// follow distance, 256, of whoever it follows, which needn't be the player)
		bool going = ((r.aiPackage == AIPKG_TRAVEL || r.aiPackage == AIPKG_FOLLOW || r.aiPackage == AIPKG_ESCORT) && !r.aiDone
				&& r.aiActive) || r.ai == AI_COMBAT;
		float leaderDist = w.distanceToPlayer(i);
		if ((r.aiPackage == AIPKG_FOLLOW || r.aiPackage == AIPKG_ESCORT) && r.ai != AI_COMBAT && r.aiTarget != "player"
			&& !r.aiTarget.empty())
		{
			int t = w.findRef(r.aiTarget);
			if (t >= 0)
			{
				float dx = w.refs[t].pos[0] - r.pos[0], dy = w.refs[t].pos[1] - r.pos[1];
				leaderDist = sqrtf(dx * dx + dy * dy);
			}
		}
		bool near = r.aiPackage != AIPKG_TRAVEL && leaderDist < 300.0f;
		if (!going || near || r.knockTimer > 0.0f || (dlg.open && dlg.ref == i))
		{
			wt.moveT = 0.0f;
			wt.anchorX = r.pos[0];
			wt.anchorY = r.pos[1];
		}
		else
		{
			float ax = r.pos[0] - wt.anchorX, ay = r.pos[1] - wt.anchorY;
			if (ax * ax + ay * ay >= 15.0f * 15.0f)
			{
				wt.moveT = 0.0f;
				wt.anchorX = r.pos[0];
				wt.anchorY = r.pos[1];
			}
			else
				wt.moveT += step;
			if (wt.moveT >= 5.0f && !wt.toldStuck)
			{
				wt.toldStuck = true;
				logf("monitor: %s is stuck at %.0f %.0f %.0f in %s (%s, package %d, target %s, path %d points, player %.0f away)",
					r.id.c_str(), r.pos[0], r.pos[1], r.pos[2], w.cellName().c_str(), r.ai == AI_COMBAT ? "fighting" : "travelling",
					r.aiPackage, r.aiTarget.c_str(), (int)r.path.size(), w.distanceToPlayer(i));
			}
		}
	}
	if (watches.size() > 400)
		watches.clear();

	// The player under the world: no floor in any loaded cell below the feet, or in the air for over 3 s
	// (flying, levitating and swimming excepted)
	const Player& p = w.player;
	bool airborne = !p.onGround && !p.flying && !p.swimming && p.levitate <= 0.0f && !p.waterWalk && !playerDead
		&& !menuOpen() && travelCell < 0;
	playerAirT = airborne ? playerAirT + step : 0.0f;
	if (airborne)
	{
		float floorZ = -1e9f;
		for (LoadedCell* l : w.loaded)
		{
			float z;
			if (collisionFloor(l->cell.collision, p.feet[0], p.feet[1], p.feet[2] + 30.0f, p.feet[2] - 100000.0f, &z) && z > floorZ)
				floorZ = z;
		}
		// (a jump may hang in the air much longer, over open sea too: Scroll of Icarian Flight)
		bool flight = p.jumpFlight && p.feet[2] > 0.0f;
		if (((floorZ < -1e8f && !flight) || (playerAirT >= 3.0f && !p.jumpFlight)) && !toldFall)
		{
			toldFall = true;
			logf("monitor: the player is %s at %.0f %.0f %.0f in %s (%.1f s in the air, vz %.0f)",
				floorZ < -1e8f ? "under every floor" : "falling", p.feet[0], p.feet[1], p.feet[2], w.cellName().c_str(),
				playerAirT, p.vz);
		}
	}
	else if (p.onGround)
		toldFall = false;
}

// A new cell: doors in it that lead nowhere, and the linear memory left each time the same cell is entered
// (falling on each visit: something a visit allocates isn't given back)
void Session::monitorCellChanged()
{
	monitorCell = w.current;
	if (w.current < 0)
		return;
	w.forLoadedRefs([&](int i) {
		const Ref& r = w.refs[i];
		if (!r.hasDest || r.destEnd)
			return;
		int dest = r.destUnconverted ? -1 : r.destHasGrid ? w.gridCell(r.destGrid[0], r.destGrid[1]) : w.cellIndex(r.destCell);
		if (dest < 0)
			monitorOnce(("door:" + r.idLower + ":" + w.cells[w.current].name).c_str(),
				"door %s in %s leads to %s, which is not in the level (unconverted %d)", r.id.c_str(), w.cellName().c_str(),
				r.destHasGrid ? "an exterior cell" : r.destCell.c_str(), (int)r.destUnconverted);
	});
	std::vector<unsigned long>& m = cellMemory[w.cellName()];
	m.push_back(linearSpaceFree() / 1024);
	size_t n = m.size();
	if (n >= 4 && m[n - 1] + 64 < m[n - 2] && m[n - 2] + 64 < m[n - 3] && m[n - 3] + 64 < m[n - 4])
		monitorOnce(("mem:" + w.cellName()).c_str(), "linear memory falls on each visit to %s: %lu, %lu, %lu, %lu KB free at entry",
			w.cellName().c_str(), m[n - 4], m[n - 3], m[n - 2], m[n - 1]);
	if (n > 12)
		m.erase(m.begin());
}
