// The game: start-up, the title screen, loading, input and the frame loop (appMain), shared by the console build and
// the PC test build. Each build's entry file (source/main.cpp, tools/test/native/main_native.cpp) is a few lines:
// what differs between them goes through platform.h, so the two can't drift apart.
#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <malloc.h>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "audio.h"
#include "distant.h"
#include "linear.h"
#include "memstat.h"
#include "log.h"
#include "player.h"
#include "gamma.h"
#include "renderer.h"
#include "screenshot.h"
#include "testdrive.h"
#include "zfile.h"
#include "saves.h"
#include "session.h"
#include "ui.h"
#include "platform.h"
#include "autoinput.h"
#include "controls.h"
#include "frontend.h"

extern "C" u32 __ctru_heap_size, __ctru_linear_heap_size;

// Game data: SD card copy wins (fast dev loop), else the copy packed into the CIA RomFS
static const char* kSdDataDir = "sdmc:/3ds/mw3ds/data";
static const char* kRomfsDataDir = "romfs:/data";

// (inFormat: the render target's colour format, RGBA8, or RGB565 on an Old 3DS; outFormat: the screen's, RGB8, or
// RGB565 on an Old 3DS: the transfer engine never signals the end of an RGB565 -> RGB8 copy on the console, and the
// frame waits for it for ever (3dbrew, Transfer Engine; issue #43). Azahar and the native build signal any copy)
#define DISPLAY_TRANSFER_FLAGS(inFormat, outFormat) \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
	GX_TRANSFER_IN_FORMAT(inFormat) | GX_TRANSFER_OUT_FORMAT(outFormat) | \
	GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

// libctru's __appExit calls this once main has returned and newlib's exit() has closed every file still open, before
// the SD card and the services go. The watchdog's thread ends here: its stack is on the heap libctru frees next. And
// romfs is unmounted here, not among main's exit steps: the distant land's file stays open on romfs across sessions,
// and exit() closing it after romfsExit() had emptied romfs's slot in the device table was a data abort in _close_r
// (address 0xc, "main at exit: process" in crash.txt) that left the console on a black screen, from Save and quit and
// from the HOME menu closing the game alike
static bool s_romfs = false;

extern "C" void userAppExit(void)
{
	watchdogStop();
	if (s_romfs)
		romfsExit();
}

int appMain()
{
	bool isNew3ds = false;
	APT_CheckNew3DS(&isNew3ds);
	if (isNew3ds)
		osSetSpeedupEnable(true);
	// What this console can do: from its CPU and from the heap it got (console.h)
	consoleProfileInit(isNew3ds, __ctru_heap_size);
	// A slow CPU's one game core is busy drawing: cells stream in on the system core (30% of it may be ours)
	if (g_profile.slowCpu)
		g_sysCoreThreads = R_SUCCEEDED(APT_SetAppCpuTimeLimit(30));

	// The Old 3DS's 16-bit colour buffer goes to a 16-bit top screen (see DISPLAY_TRANSFER_FLAGS). The screens'
	// framebuffers are in VRAM, beside the render targets (a few MB of its 6 free): a MB of linear memory back for cells
	gfxInit(g_profile.rgb565 ? GSP_RGB565_OES : GSP_BGR8_OES, GSP_BGR8_OES, true);
	// gfxInit lights the screens on buffers that hold whatever VRAM had (multicoloured static, on an Old 2DS and when
	// started from FBI): they stay forced black until the first black frame below is up
	GSPGPU_SetLcdForceBlack(1);
	// GPU commands of a whole frame (both eyes, bottom screen): overflowing it is a panic, so it is
	// generous, the renderer stops drawing near the end and 3D backs off when one eye uses too much.
	// (an Old 3DS's is half: console.cpp)
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * g_profile.cmdBufScale);
	C2D_Init(4096);
	C2D_Prepare();
	irrstInit();
	logInit();
	crashHandlerInstall(0);
	watchdogStart();
	linearLockInit();
	logf("MW3DS start, %s, heap %lu KB, linear %lu KB, memory mode %lu%s", isNew3ds ? "New 3DS" : "Old 3DS",
		__ctru_heap_size / 1024, __ctru_linear_heap_size / 1024, (unsigned long)osGetApplicationMemType(),
		g_sysCoreThreads ? ", cells stream on the system core" : "");
	logf("console: %s CPU, %s heap (%d objects, %u KB of sounds kept)", g_profile.slowCpu ? "slow" : "fast",
		g_profile.smallHeap ? "small" : "large", g_profile.refBudget, g_profile.soundCacheBytes / 1024);
	gammaInit();
	bool romfs = R_SUCCEEDED(romfsInit());
	s_romfs = romfs;                   // (unmounted in userAppExit)
	struct stat st;
	logf("memory: %s, heap %lu KB, linear %lu KB (%lu KB free)", isNew3ds ? "New 3DS" : "Old 3DS",
		__ctru_heap_size / 1024, __ctru_linear_heap_size / 1024, linearSpaceFree() / 1024);

	// The top screen's colour buffer: 32-bit on both consoles (console.h rgb565 keeps the 16-bit one, half the buffer and
	// the copy to the screen each frame: it banded the sky and the fog on an Old 3DS)
	GPU_COLORBUF colorFmt = g_profile.rgb565 ? GPU_RB_RGB565 : GPU_RB_RGBA8;
	u32 transfer = g_profile.rgb565 ? DISPLAY_TRANSFER_FLAGS(GX_TRANSFER_FMT_RGB565, GX_TRANSFER_FMT_RGB565)
		: DISPLAY_TRANSFER_FLAGS(GX_TRANSFER_FMT_RGBA8, GX_TRANSFER_FMT_RGB8);
	logf("display: top screen %s", g_profile.rgb565 ? "16-bit, copied RGB565 to RGB565" : "24-bit, copied RGBA8 to RGB8");
	C3D_RenderTarget* top = C3D_RenderTargetCreate(240, 400, colorFmt, GPU_RB_DEPTH24_STENCIL8);
	C3D_RenderTargetSetOutput(top, GFX_TOP, GFX_LEFT, transfer);
	// Right eye for stereo 3D (only drawn while the 3D mode is on)
	C3D_RenderTarget* topRight = C3D_RenderTargetCreate(240, 400, colorFmt, GPU_RB_DEPTH24_STENCIL8);
	C3D_RenderTargetSetOutput(topRight, GFX_TOP, GFX_RIGHT, transfer);
	bool stereoOn = false;
	C3D_RenderTarget* bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
	// A black frame at once, through the GPU, before the screens are let go of black. (Cleared by the CPU instead,
	// the console stopped on a data abort: VRAM isn't the program's to write)
	C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
	clearTop(top, 0x000000FF);
	C3D_RenderTargetClear(bottom, C3D_CLEAR_ALL, 0x000000FF, 0);
	C3D_FrameEnd(0);
	for (int i = 0; i < 3; i++)        // (the frame is swapped in on a VBlank once the GPU is done with it)
		gspWaitForVBlank();
	GSPGPU_SetLcdForceBlack(0);
	rendererInit();
	uiInit();
	// A CIA with its data inside uses that (whatever an older copy left on the card); the dev build and
	// the program-only CIA read the card's
	const char* dataDir = romfs && stat("romfs:/data/game.json", &st) == 0 ? kRomfsDataDir
		: stat(kSdDataDir, &st) == 0 ? kSdDataDir : kRomfsDataDir;
	logf("romfs %s, data dir %s", romfs ? "mounted" : "absent", dataDir);
	int maxTex = dataMaxTex(dataDir);
	logf("data: textures up to %d texels%s", maxTex, maxTex ? "" : " (not recorded: converted before max_tex)");
	// (not for a scripted test run: the native Old 3DS suite runs the New 3DS's data on purpose)
	bool scriptedRun = stat("sdmc:/3ds/mw3ds/autoinput.txt", &st) == 0;
	if (!isNew3ds && maxTex > kOld3dsMaxTex && scriptedRun)
		logf("data made for a New 3DS on an Old 3DS: a scripted run, going on");
	if (!isNew3ds && maxTex > kOld3dsMaxTex && !scriptedRun)
	{
		logf("data made for a New 3DS on an Old 3DS: showing the wrong console screen, then quitting");
		wrongConsoleScreen(top, bottom, maxTex);
		uiExit();
		rendererExit();
		C2D_Fini();
		C3D_Fini();
		irrstExit();
		logExit();
		gfxExit();
		return 0;
	}
	audioInit(dataDir);
	drawLoading(top, bottom, "Loading...");

	Session* session = new Session();
	ScriptedSteps autoinput = loadAutoinput();
	session->autotest = !autoinput.empty();
	logBuffered(!session->autotest);          // play: the log written every 2 s, tests every line
	u64 loadStart = osGetTime();
	// sdmc:/3ds/mw3ds/start.txt: name of the cell to start in (e.g. "Seyda Neen"), skipping chargen,
	// or "save:<path>" to continue from that save (test runs skip the title screen)
	std::string startCell, startSave;
	if (FILE* sf = fopen("sdmc:/3ds/mw3ds/start.txt", "r"))
	{
		char line[128] = {};
		if (fgets(line, sizeof(line), sf))
		{
			line[strcspn(line, "\r\n")] = 0;
			startCell = line;
		}
		fclose(sf);
		logf("start.txt: %s", startCell.c_str());
		if (startCell.compare(0, 5, "save:") == 0)
		{
			startSave = startCell.substr(5);
			startCell.clear();
		}
	}
	// A save that won't read (Session::start's saveUnreadable): the title again to pick another, nothing started that would
	// autosave over it. False: the player quit there
	auto retryFromTitle = [&](bool& loadedOut) {
		while (!loadedOut && session->saveUnreadable)
		{
			delete session;
			session = new Session();
			session->dataDir = dataDir;
			std::string pick = titleScreen(top, bottom, dataDir, "That save couldn't be read. Choose another, or a new game.");
			if (pick == "quit")
				return false;
			drawLoading(top, bottom, "Loading...");
			LoadingProgress progress(top, bottom, "Loading...");
			loadedOut = session->start(dataDir, "", pick.empty() ? nullptr : pick.c_str());
			if (loadedOut)
			{
				session->vm.load(session->w);
				uiLoadTheme(dataDir, session->w.game.ui);
				g_splashes = session->w.game.art.splash;
			}
		}
		return true;
	};
	// Normal play: title screen to pick a save (autosave, own saves, bundled starting points) or a new
	// game. Scripted test runs and start.txt skip it and start fresh.
	static const char* kSavePath = kAutosavePath;
	bool testRun = session->autotest || stat("sdmc:/3ds/mw3ds/autocam.txt", &st) == 0;
	// MW3DS_INTRO (a recording of the start, tools/test/video.py): a scripted run first plays the logos and shows the
	// title screen for five seconds, then carries on as it would
	if (testRun && getenv("MW3DS_INTRO"))
	{
		loadStartupTheme(dataDir);
		startupMovie(top, bottom, dataDir, "bethesda_logo");
		startTitleMusic(dataDir);
		startupMovie(top, bottom, dataDir, "mw_logo", kMwLogoZoom, true);
		titleScreen(top, bottom, dataDir, nullptr, 150);
	}
	if (!testRun && startCell.empty() && startSave.empty())
	{
		// The company logo, then the title music with the Morrowind logo over it, as OpenMW starts (each skippable)
		loadStartupTheme(dataDir);
		startupMovie(top, bottom, dataDir, "bethesda_logo");
		startTitleMusic(dataDir);
		startupMovie(top, bottom, dataDir, "mw_logo", kMwLogoZoom, true);
		startSave = titleScreen(top, bottom, dataDir);
		if (startSave == "quit")
		{
			delete session;
			audioExit();
			uiExit();
			rendererExit();
			C2D_Fini();
			C3D_Fini();
			irrstExit();
			logExit();
			gfxExit();
			return 0;
		}
		logf("title: %s", startSave.empty() ? "new game" : startSave.c_str());
		MARK("after the title");
		drawLoading(top, bottom, "Loading...");
		MARK("loading screen drawn");
		loadStart = osGetTime();
	}
	session->dataDir = dataDir;
	bool loaded;
	{
		LoadingProgress progress(top, bottom, "Loading...");
		loaded = session->start(dataDir, startCell, startSave.empty() ? nullptr : startSave.c_str());
		if (loaded)
		{
			logf("heap: session %d KB", mallinfo().uordblks / 1024);
			session->vm.load(session->w);
			logf("heap: + view model %d KB", mallinfo().uordblks / 1024);
			uiLoadTheme(dataDir, session->w.game.ui);
			logf("heap: + ui theme %d KB", mallinfo().uordblks / 1024);
			g_splashes = session->w.game.art.splash;
		}
	}
	if (!testRun && !retryFromTitle(loaded))
	{
		delete session;
		audioExit();
		uiExit();
		rendererExit();
		C2D_Fini();
		C3D_Fini();
		irrstExit();
		logExit();
		gfxExit();
		return 0;
	}
	session->savePath = kSavePath;      // test runs still save (at the end of scripted input)
	memoryReserveTake();
	logf("load %s in %llu ms, linear free %lu KB, heap used %d KB", loaded ? "ok" : "FAILED",
		osGetTime() - loadStart, linearSpaceFree() / 1024, mallinfo().uordblks / 1024);

	RenderCamera spawnCam = { { session->w.player.feet[0], session->w.player.feet[1],
		session->w.player.feet[2] + PLAYER_EYE_HEIGHT }, session->w.player.yaw, 0.0f };
	std::vector<CamPose> autocam = loadAutocam(spawnCam);
	size_t poseIndex = 0, inputIndex = 0;
	int poseFrames = 0;
	float inputTime = 0.0f;
	bool autoShot = autocam.empty() && autoinput.empty() && stat("sdmc:/3ds/mw3ds/autoshot", &st) == 0;
	bool stepShot = false;
	int stepShots = 0;
	TestDriver driver;
	bool chainMode = false;
	bool stepBlocking = false;           // this step waits for the driver (a walk, a kill, an activation)

	u64 lastTick = svcGetSystemTick(), statTick = lastTick;
	int frames = 0;
	float cmdPeak = 0.0f;          // most of the GPU command buffer a frame used this second
	float t = 0.0f;
	bool touchWasDown = false, dragged = false;
	int lastTouchX = 0, lastTouchY = 0, pressX = 0, pressY = 0;

	while (aptMainLoop())
	{
		hidScanInput();
		irrstScanInput();
		u32 down = hidKeysDown(), held = hidKeysHeld(), up = hidKeysUp();
		if (!loaded && (down & KEY_START))
			break;
		if (loaded && session->wantQuit)
			break;

		u64 now = svcGetSystemTick();
		float dt = fminf((float)(now - lastTick) / SYSCLOCK_ARM11, 0.1f);
		lastTick = now;
		t += dt;

		// Touch: taps and drags for the bottom-screen UI
		UiInput ui = {};
		touchPosition tp;
		hidTouchRead(&tp);
		bool touching = held & KEY_TOUCH;
		// The scripted tests press the classic buttons, or the layout a CONTROLS: step chose
		if (session->autotest)
			g_controlLayout = g_testLayout;
		g_menuForInput = session->menuOpen();
		PlayerInput in = readInput(held, down, up, dt);
		bool autoShotNow = false;

		if (inputIndex < autoinput.size() && loaded)
		{
			ScriptedInput& s = autoinput[inputIndex];
			static float testDt = 1.0f / 30.0f;      // fixed steps so scripted runs are repeatable (DT: sets them)
			if (s.dt > 0.0f)
				testDt = s.dt;
			dt = testDt;
			in = {};
			in.moveX = s.moveX;
			in.moveY = s.moveY;
			in.lookX = s.lookX * lookScale();       // (the step's look is the C-stick's)
			in.attack = (s.keys & KEY_X) && !(s.keys & KEY_ZL);     // ZL + X puts the weapon away
			in.cast = inputTime == 0.0f && (s.keys & KEY_Y);
			in.sneak = s.keys & KEY_L;
			in.up = s.keys & KEY_R;
			in.down = s.keys & KEY_L;
			bool first = inputTime == 0.0f;
			in.jump = s.jump && first;
			down = first ? s.keys : 0;
			held = s.keys;
			in.togglePov = down & KEY_SELECT;
			in.sheathe = (held & KEY_ZL) && (down & KEY_X);
			if (held & KEY_ZL)
			{
				const u32 dirs[4] = { KEY_DUP, KEY_DRIGHT, KEY_DDOWN, KEY_DLEFT };
				for (int k = 0; k < 4; k++)
					if (down & dirs[k])
						in.quick = k;
			}
			else
			{
				if (held & KEY_DUP) in.lookY += lookScale();
				if (held & KEY_DDOWN) in.lookY -= lookScale();
			}
			// CONTROLS:goldeneye: the step's stick is the Circle Pad and its keys the buttons, through that layout;
			// GYRO:yaw:pitch: the console turning that fast, through gyro aim's switch and sensitivity
			if (first && s.layout >= 0)
			{
				g_testLayout = g_controlLayout = s.layout;
				logf("test: controls %s", s.layout == CONTROLS_GOLDENEYE ? "goldeneye" : s.layout == CONTROLS_XBOX ? "xbox" : "classic");
			}
			if (first && s.frameCap >= 0)
				g_testFrameCap = s.frameCap == 1;
			if (first && s.skin >= 0)
			{
				g_gpuSkinning = s.skin == 1;
				logf("test: actors loaded from here skinned on the %s", s.skin ? "GPU" : "CPU");
			}
			if (first && s.gyroAim >= 0)
			{
				g_gyroAim = s.gyroAim;
				logf("test: gyro aim %d%%", g_gyroAim);
			}
			if (g_controlLayout == CONTROLS_GOLDENEYE)
			{
				PlayerInput ge = {};
				ge.quick = -1;
				ge.lookX = s.lookX * lookScale();
				static u32 lastHeld = 0;
				u32 released = first ? lastHeld & ~held : 0;
				lastHeld = held;
				static GoldenEyeState state;      // (its own: the console's pass above runs every frame too)
				goldenEyeInput(state, ge, s.moveX, s.moveY, held, down, released, dt);
				ge.jump |= s.jump && first;
				in = ge;
			}
			in = invertLook(in);
			if (g_gyroAim > 0)
			{
				in.gyroYaw = s.gyroYaw * g_gyroAim / 100.0f;
				in.gyroPitch = s.gyroPitch * g_gyroAim / 100.0f;
			}
			if (first && !s.door.empty())
			{
				int ref = session->testFindRef(s.door);
				logf("autoinput: door %s %s", s.door.c_str(), session->w.active(ref) ? "used" : "not in this cell");
				if (session->w.active(ref))
					session->playerActivate(ref);
			}
			if (first && s.boost > 0)
				session->testBoost(s.boost);
			if (first && s.jail)
				session->goToJail();
			if (first && s.shot)
				stepShot = true;
			if (first && s.legit)
			{
				session->testLegit = true;
				logf("test: LEGIT: from here only what a player can do");
			}
			if (first && s.chain)
			{
				chainMode = true;
				logf("test: chained run (journal setup only raises)");
				// A bounty the last chapter's driving ran up (a book taken in sight) is paid, as a player would
				// between chapters: else the first guard's arrest screen holds every step after it
				if (session->w.bounty > 0)
				{
					logf("test: chained run: bounty %d paid", session->w.bounty);
					session->w.bounty = 0;
					session->w.arrestDeclined = -1;
				}
				if (session->screen == SCR_ARREST)
					session->closeScreen();
			}
			if (first && !s.startScript.empty())
			{
				// STARTSCRIPT:name:ref: started on an object, as ref->StartScript does
				std::string nm = s.startScript, on;
				size_t colon = nm.find(':');
				if (colon != std::string::npos)
				{
					on = nm.substr(colon + 1);
					nm = nm.substr(0, colon);
				}
				session->w.startGlobalScript(nm, on.empty() ? -1 : session->w.findRef(on));
				logf("test: started script %s", s.startScript.c_str());
			}
			if (first && !s.saveAs.empty())
			{
				std::string p = "sdmc:/3ds/mw3ds/test_" + s.saveAs + ".sav";
				logf("test: saved %s%s: %s", p.c_str(), s.saveLegacy ? " (version 1)" : "",
					session->w.saveGame(p.c_str(), s.saveLegacy) ? "ok" : "FAILED");
			}
			if (first && !s.loadFrom.empty())
			{
				session->reloadPath = "sdmc:/3ds/mw3ds/test_" + s.loadFrom + ".sav";
				session->wantReload = true;
				logf("test: loading %s", session->reloadPath.c_str());
			}
			if (first)
			{
				stepBlocking = false;
				for (auto& act : s.actions)
					stepBlocking |= driver.start(*session, act);
			}
			if (first && s.enchant)
				session->openEnchanting(-1);
			if (first && !s.wear.empty())
				for (auto& it : session->w.inventory)
					if (it.id == s.wear)
						it.condition = s.wearTo;
			if (first && s.repair)
				for (size_t i = 0; i < session->w.inventory.size(); i++)
					if (const Object* o = session->w.game.object(session->w.inventory[i].id))
						if (o->type == "REPA")
						{
							session->openRepair(-1, i);
							break;
						}
			if (first && !s.recharge.empty())
			{
				int item = -1, gem = -1;
				for (size_t i = 0; i < session->w.inventory.size(); i++)
				{
					if (session->w.inventory[i].id == s.recharge)
						item = i;
					if (!session->w.inventory[i].soul.empty() && gem < 0)
						gem = i;
				}
				if (item >= 0 && gem >= 0)
					session->rechargeItem(item, gem);
				else
					logf("test: nothing to recharge %s with", s.recharge.c_str());
			}
			if (first && !s.join.empty())
			{
				if (s.joinRank < 0)
				{
					// JOIN:faction:-1 leaves it (the sweeps undo a quest's setup)
					session->w.pcRank.erase(s.join);
					session->w.pcExpelled.erase(s.join);
					logf("test: left %s", s.join.c_str());
				}
				else
				{
					// (chained: never below the rank the last chapter's story gave)
					auto had = session->w.pcRank.find(s.join);
					if (!chainMode || had == session->w.pcRank.end() || had->second < s.joinRank)
						session->w.pcRank[s.join] = s.joinRank;
					logf("test: joined %s at rank %d", s.join.c_str(), session->w.pcRank[s.join]);
				}
			}
			if (first && !s.cast.empty())
			{
				if (session->testLegit && !TestDriver::canCast(*session, s.cast))
					logf("drive: FAIL cast %s: not a known spell or a carried item (LEGIT)", s.cast.c_str());
				else
				{
					// the id as given when there is such a spell (or item), else with '_' as spaces
					std::string sel = s.cast;
					if (sel.compare(0, 5, "item:") != 0 && !session->w.game.spells.count(lower(sel)))
					{
						std::string sp = lower(sel);
						std::replace(sp.begin(), sp.end(), '_', ' ');
						if (session->w.game.spells.count(sp))
							sel = sp;
					}
					session->w.stats.selectedSpell = sel;
					session->castSpell();
				}
			}
			if (first && !s.soulKill.empty())
			{
				int ref = session->testFindRef(s.soulKill);
				// a living one (several can share the id)
				std::string want = session->w.refs.size() && ref >= 0 ? session->w.refs[ref].idLower : "";
				for (size_t i = 0; i < session->w.refs.size() && ref >= 0; i++)
					if (session->w.refs[i].idLower == want && !session->w.refs[i].dead && session->w.active(i))
					{
						ref = i;
						break;
					}
				if (session->w.active(ref) && !session->w.refs[ref].dead)
				{
					session->w.refs[ref].soulTrapUntil = session->w.time + 30.0f;
					session->killNpc(ref, false);          // no witness, no crime: the test isn't about the law
				}
				logf("test: soulkill %s %s", s.soulKill.c_str(), session->w.active(ref) ? "done" : "not loaded");
			}
			if (first && !s.brew.empty())
			{
				session->alchemySlots.clear();
				std::string rest = s.brew;
				while (!rest.empty())
				{
					size_t c = rest.find(':');
					session->alchemySlots.push_back(rest.substr(0, c));
					rest = c == std::string::npos ? "" : rest.substr(c + 1);
				}
				// (the screen offers only what is carried: one that isn't is the case's mistake, not a potion)
				for (auto& id : session->alchemySlots)
					if (session->w.itemCount(id) <= 0)
						logf("drive: FAIL BREW: %s not carried", id.c_str());
				session->brewPotion();
			}
			// SLEEPUNTIL:h: whole hours from now to the next time the clock reads h (a full day when it already does)
			int sleepHours = s.sleep;
			if (first && s.sleepUntil >= 0)
			{
				int now = (int)floorf(fmodf(session->w.gameHour, 24.0f));
				sleepHours = ((s.sleepUntil % 24) - now + 24) % 24;
				if (sleepHours == 0)
					sleepHours = 24;
			}
			// (a rest still running from the step before, whose seconds were shorter: not started over)
			if (first && sleepHours > 0 && session->restDone >= 0 && session->screen == SCR_REST)
				logf("drive: SLEEP %d ignored: a rest is still running (%d of %d hours)", sleepHours, session->restDone, session->restTotal);
			else if (first && sleepHours > 0)
			{
				// (a bed's rest menu already open: its Rest, asleep there even where sleeping is illegal)
				if (!(session->screen == SCR_REST && session->restBed))
					session->openRest(false);
				session->startRest(sleepHours);
				if (s.sleepForce == 2)
					session->restInterruptAt = -1;     // 2: nothing comes
				else if (s.sleepForce && session->restInterruptList.empty())
				{
					const LevelCell& lc = session->w.cells[session->w.current];
					auto rg = session->w.game.regions.find(lc.region);
					if (rg != session->w.game.regions.end())
						session->restInterruptList = rg->second.sleep;
				}
				if (s.sleepForce == 1)
					session->restInterruptAt = 1;
			}
			if (first && s.level > 0)
				session->w.stats.level = s.level;
			if (first && s.weather >= 0 && session->w.current >= 0)
			{
				World& ww = session->w;
				ww.setWeatherHere(s.weather);
				logf("test: weather %d in %s", s.weather, ww.cells[ww.current].region.c_str());
			}
			if (first && s.persuade >= 0 && session->dlg.open && !session->dlg.choices.empty())
				logf("drive: FAIL persuade %d ignored: a choice is open (%d choices): answer it first (CHOICE)", s.persuade, (int)session->dlg.choices.size());
			else if (first && s.persuade >= 0 && session->dlg.open)
				session->persuade(s.persuade);
			if (first && !s.tool.empty())
			{
				int ref = session->testFindRef(s.tool);
				InventoryItem* t = session->playerToolItem();
				if (session->w.active(ref) && t)
				{
					session->target = ref;
					session->useTool(*t);
				}
				else
					logf("test: no tool or %s not loaded", s.tool.c_str());
			}
			for (int k = 0; first && k < s.skillUps; k++)
				session->raiseSkill(s.skillUp);
			if (first && s.days > 0)
				session->w.gameHour += 24.0f * s.days;
			if (first && s.hour >= 0.0f)
			{
				session->w.gameHour = floorf(session->w.gameHour / 24.0f) * 24.0f + s.hour;
				session->w.weatherSeen = session->w.gameHour;      // (setting the hour is not time passing)
			}
			if (first && s.rep >= 0)
				session->w.pcReputation = s.rep;
			if (first && !s.setLocal.empty())
			{
				// id:var:value (id may hold underscores for spaces)
				size_t a = s.setLocal.find(':'), b = s.setLocal.rfind(':');
				if (a != std::string::npos && b > a)
				{
					std::string id = s.setLocal.substr(0, a), var = s.setLocal.substr(a + 1, b - a - 1);
					float v = atof(s.setLocal.c_str() + b + 1);
					int ri = session->w.findRefAnywhere(id);
					if (ri < 0)
					{
						std::string spaced = id;
						std::replace(spaced.begin(), spaced.end(), '_', ' ');
						ri = session->w.findRefAnywhere(spaced);
					}
					float* p = ri >= 0 && session->w.refs[ri].script >= 0 ? session->w.scripts[session->w.refs[ri].script].local(var) : nullptr;
					if (p)
						*p = v;
					logf("test: local %s.%s = %g%s", id.c_str(), var.c_str(), v, p ? "" : " (no such local)");
				}
			}
			if (first && !s.drop.empty())
			{
				World& ww = session->w;
				std::string id = lower(s.drop);
				if (ww.itemCount(id) > 0)
				{
					ww.removeItem(id, 1);
					ww.dropItemAt({ id, 1, false }, ww.player.feet, ww.player.yaw, 50.0f);
				}
				else
					logf("test: no %s to drop", id.c_str());
			}
			if (first && !s.setGlobal.empty())
			{
				session->w.setGlobal(s.setGlobal, s.globalValue);
				logf("test: global %s = %g", s.setGlobal.c_str(), s.globalValue);
			}
			if (first && !s.talk.empty())
			{
				World& ww = session->w;
				int ri = session->testFindRef(s.talk);
				if (ri < 0 || !ww.active(ri))
					logf("test: talk %s: not here", s.talk.c_str());
				else if (!ww.refs[ri].visible() || ww.refs[ri].dead)
					logf("test: talk %s: disabled or dead", s.talk.c_str());
				else
				{
					Ref& r = ww.refs[ri];
					if (r.ai == AI_COMBAT)
					{
						// an earlier test left them fighting (the sweep runs quests back to back)
						logf("test: talk %s: was fighting, calmed", s.talk.c_str());
						r.ai = AI_IDLE;
						r.fight = 0;
					}
					float fx = sinf(r.rot[2]), fy = cosf(r.rot[2]);
					float to[4] = { r.pos[0] + fx * 90.0f, r.pos[1] + fy * 90.0f, r.pos[2] + 10.0f, 0.0f };
					Player& p = ww.player;
					memcpy(p.feet, to, sizeof(p.feet));
					p.yaw = atan2f(r.pos[0] - to[0], r.pos[1] - to[1]);
					p.vz = 0.0f;
					p.fallTop = p.feet[2];
					p.landedFall = 0.0f;
					r.disposition = 100;
					session->playerActivate(ri);
					logf("test: talk %s (%s)", s.talk.c_str(), session->dlg.open ? "talking" : "no talk");
				}
			}
			if (first && s.quickSet.size() > 2)
			{
				session->w.stats.quickKeys[(s.quickSet[0] - '0') & 3] = s.quickSet.substr(2);
				logf("test: quick key %s", s.quickSet.c_str());
			}
			if (first && !s.spell.empty())
			{
				PlayerStats& ps = session->w.stats;
				std::string sid = lower(s.spell);
				if (!session->w.game.spells.count(sid))
					std::replace(sid.begin(), sid.end(), '_', ' ');    // ids with spaces: "vampire_blood_aundae"
				if (std::find(ps.spells.begin(), ps.spells.end(), sid) == ps.spells.end())     // (known once, as AddSpell)
					ps.spells.push_back(sid);
				float hp = ps.health;
				session->w.recomputeStats();
				ps.health = fminf(ps.healthMax, hp);
				logf("test: spell %s", sid.c_str());
			}
			if (first && s.effect >= 0)
			{
				// (the last number is the skill for the skill effects: fortify, drain, damage, restore, absorb skill)
				bool skillEffect = s.effect == 83 || s.effect == 21 || s.effect == 26 || s.effect == 78 || s.effect == 89;
				SpellEffect e = { s.effect, skillEffect ? s.effectAttr : -1, skillEffect ? -1 : s.effectAttr, s.effectMag, s.effectMag, s.effectSecs, 0 };
				session->applyEffectToPlayer(e, "test");
			}
			if (first && !s.moveTo.empty())
			{
				World& w = session->w;
				int ri = w.findRefAnywhere(s.moveTo);
				if (ri < 0)
					ri = w.findRefAnywhere(std::string(s.moveTo).replace(s.moveTo.find('_') == std::string::npos ? 0 : s.moveTo.find('_'), s.moveTo.find('_') == std::string::npos ? 0 : 1, " "));
				float pos[3] = { w.player.feet[0] + sinf(w.player.yaw) * 200.0f, w.player.feet[1] + cosf(w.player.yaw) * 200.0f,
					w.player.feet[2] };
				if (ri >= 0)
					w.relocate(ri, w.current, pos, w.player.yaw + 3.14159265f);
				else
					logf("test: %s not found", s.moveTo.c_str());
			}
			if (first && !s.placePC.empty())
				session->w.placeAt(s.placePC, 1, 256.0f, 0, session->w.player.feet, session->w.player.yaw);
			if (first && !s.follow.empty())
			{
				int ri = session->testFindRef(s.follow);
				if (ri >= 0)
				{
					session->w.refs[ri].aiPackage = AIPKG_FOLLOW;
					session->w.refs[ri].aiTarget = "player";
					session->w.refs[ri].aiDone = false;
					session->w.refs[ri].fight = 0;       // a companion, not a creature about to attack
					session->w.refs[ri].ai = AI_IDLE;
				}
			}
			if (first && s.evict >= 0)
				session->evictFarCells(s.evict, true);
			if (first && !s.check.empty())
			{
				int ri = session->testFindRef(s.check);
				if (s.check.compare(0, 8, "journal:") == 0)
				{
					std::string q = s.check.substr(8);
					for (auto& ch : q)
						ch = tolower(ch);
					auto it = session->w.journalIndex.find(q);
					logf("check: journal %s = %d", q.c_str(), it != session->w.journalIndex.end() ? it->second : 0);
				}
				else if (s.check.compare(0, 7, "global:") == 0)
				{
					std::string g = s.check.substr(7);
					for (auto& ch : g)
						ch = tolower(ch);
					auto it = session->w.globals.find(g);
					logf("check: global %s = %g", g.c_str(), it != session->w.globals.end() ? it->second : 0.0f);
				}
				else if (s.check == "player")
				{
					const PlayerStats& ps = session->w.stats;
					logf("check: player in %s at %.0f %.0f %.0f str %d int %d health %.0f/%.0f effects %d",
						session->w.cellName().c_str(), session->w.player.feet[0], session->w.player.feet[1],
						session->w.player.feet[2], ps.attributes[0], ps.attributes[1], ps.health, ps.healthMax,
						(int)session->w.effects.size());
				}
				else if (ri < 0)
					logf("check: %s not in memory", s.check.c_str());
				else
					logf("check: %s cell %s enabled %d taken %d dead %d at %.0f %.0f %.0f rot %.2f %.2f %.2f ai %d done %d", s.check.c_str(),
						session->w.cells[session->w.refs[ri].cell].file.c_str(), session->w.refs[ri].enabled,
						session->w.refs[ri].pickedUp, session->w.refs[ri].dead, session->w.refs[ri].pos[0],
						session->w.refs[ri].pos[1], session->w.refs[ri].pos[2], session->w.refs[ri].rot[0], session->w.refs[ri].rot[1],
						session->w.refs[ri].rot[2], session->w.refs[ri].aiPackage,
						session->w.refs[ri].aiDone);
				if (ri >= 0 && session->w.refs[ri].actor >= 0)
				{
					const Ref& cr = session->w.refs[ri];
					std::string p;
					for (int k : cr.path)
						p += " " + std::to_string((int)session->w.pathPoints[k].pos[0]) + "," + std::to_string((int)session->w.pathPoints[k].pos[1]);
					logf("check: %s moving: path%s stuck %.2f grid %d ai-state %d", s.check.c_str(), p.empty() ? " (straight)" : p.c_str(),
						cr.stuckTimer, session->w.time < cr.gridUntil, cr.ai);
				}
				// What it holds (containers, NPCs)
				if (ri >= 0 && !session->w.refs[ri].contents.empty())
				{
					std::string held;
					for (auto& c : session->w.refs[ri].contents)
						held += " " + std::to_string(c.first) + "x" + c.second;
					logf("check: %s holds%s", s.check.c_str(), held.substr(0, 200).c_str());
				}
				// Its script's locals (what a scripted scene counted)
				int sc = ri >= 0 ? session->w.refs[ri].script : -1;
				if (sc >= 0 && sc < (int)session->w.scripts.size() && session->w.scripts[sc].script)
				{
					const ScriptInstance& si = session->w.scripts[sc];
					std::string text;
					for (size_t k = 0; k < si.locals.size() && k < si.script->localNames.size(); k++)
						text += " " + si.script->localNames[k] + "=" + std::to_string((int)si.locals[k]);
					logf("check: %s script %s%s", s.check.c_str(), si.script->name.c_str(), text.c_str());
				}
			}
			if (first && !s.give.empty())
				session->testGive(s.give, s.giveCount, chainMode);
			if (first && !s.place.empty())
				session->testPlace(s.place, s.placeDist);
			if (first && !s.hit.empty())
				session->testHit(s.hit);
			if (first && !s.journal.empty())
			{
				// CHAIN (a chapter run from the last one's save): setup only raises, never undoes progress
				if (!chainMode || session->w.getJournal(s.journal) < s.journalIndex)
					session->w.setJournal(s.journal, s.journalIndex);
				logf("test: journal %s %d", s.journal.c_str(), s.journalIndex);
			}
			if (first && !s.gotoCell.empty())
				session->testGoto(s.gotoCell);
			if (first && s.choiceValue > 0 && session->dlg.open)
			{
				bool found = false;
				for (auto& ch : session->dlg.choices)
					if (ch.second == s.choiceValue)
					{
						logf("autoinput: choice value %d (%s)", s.choiceValue, ch.first.c_str());
						dialogueChoose(session->dlg, session->w, *session, ch.second);
						found = true;
						break;
					}
				if (!found)
					logf("autoinput: choice value %d not offered (%d choices)", s.choiceValue, (int)session->dlg.choices.size());
			}
			if (first && s.choice > 0 && session->dlg.open)
			{
				// As in the game: only a choice the talk offers (the k-th button)
				if (s.choice <= (int)session->dlg.choices.size())
				{
					int value = session->dlg.choices[s.choice - 1].second;
					logf("autoinput: choice %d (%s)", s.choice, session->dlg.choices[s.choice - 1].first.c_str());
					dialogueChoose(session->dlg, session->w, *session, value);
				}
				else
					logf("autoinput: choice %d not offered (%d choices)", s.choice, (int)session->dlg.choices.size());
			}
			if (first && !s.topic.empty() && session->dlg.open)
			{
				logf("autoinput: topic %s (available: %d)", s.topic.c_str(), (int)session->dlg.topics.size());
				// A player can only pick what the list shows: a topic not in it is a failure (asked anyway, so
				// the rest of the test still runs)
				const Topic* t = session->w.game.topic(s.topic);
				bool listed = false;
				for (auto& name : session->dlg.topics)
					listed |= t && name == t->name;
				if (t && !listed)
				{
					std::string by = dialogueNamedBy(t->lower);
					logf("drive: FAIL topic %s not offered (%s%s%s, %s)", t->name.c_str(),
						session->w.knownTopics.count(t->lower) ? "known" : "never learned",
						by.empty() ? "" : "; named by ", by.empty() ? "" : (by + ", who had no answer").c_str(),
						dialogueFindInfo(session->w, *t, session->dlg.ref, -1) ? "the speaker has an answer" : "no answer from the speaker");
				}
				if (!session->dlg.choices.empty())
					logf("drive: FAIL topic %s ignored: a choice is open (%d choices): answer it first (CHOICE)", s.topic.c_str(),
						(int)session->dlg.choices.size());
				// (LEGIT: not asked; a player has no way to)
				if (listed || !t || !session->testLegit)
					dialogueTopic(session->dlg, session->w, *session, s.topic);
			}
			if (s.waitMessage && !session->messages.empty())
			{
				logf("autoinput: dismiss \"%.50s\"", session->messages.front().text.c_str());
				down = held = KEY_A;
				inputTime = s.secs;
			}
			if (first && s.camSet >= 0)
			{
				g_camHeld = s.camSet == 1;
				g_heldCam = RenderCamera{ { s.cam[0], s.cam[1], s.cam[2] }, C3D_AngleFromDegrees(s.cam[3]),
					C3D_AngleFromDegrees(s.cam[4]) };
			}
			if (first && s.hud >= 0)
				g_hudOff = s.hud == 0;
			if (first && s.halt >= 0)
				g_halt = s.halt == 1;
			if (first && s.teleport && !s.tpInside && session->w.current >= 0 && session->w.cells[session->w.current].interior)
			{
				// from inside to world coordinates: out to the exterior cell there (as a door would)
				int dest = session->w.gridCell((int)floorf(s.tp[0] / 8192.0f), (int)floorf(s.tp[1] / 8192.0f));
				if (dest >= 0)
				{
					session->travelCell = dest;
					memcpy(session->travelPos, s.tp, sizeof(session->travelPos));
					session->travelYaw = C3D_AngleFromDegrees(s.tp[3]);
					logf("test: out to %s", session->w.cells[dest].name.c_str());
				}
			}
			else if (first && s.teleport)
			{
				Player& p = session->w.player;
				p.feet[0] = s.tp[0];
				p.feet[1] = s.tp[1];
				p.feet[2] = s.tp[2];
				p.yaw = C3D_AngleFromDegrees(s.tp[3]);
				p.pitch = 0.0f;
				p.vz = 0.0f;
				p.fallTop = p.feet[2];         // a test teleport isn't a fall
				p.landedFall = 0.0f;
			}
			driver.keepFloating(*session);
			if (driver.busy())
				driver.update(*session, in, down, dt);
			touching = false;
			if (first && s.tapX >= 0)
			{
				ui.tapped = true;
				ui.tapX = s.tapX;
				ui.tapY = s.tapY;
			}
			if (first && s.drag)
			{
				ui.touching = true;
				ui.touchX = 160;
				ui.touchY = 100;
				ui.dragDX = s.dragX;
				ui.dragDY = s.dragY;
			}
			// A finger held (TOUCH): down at the step's spot every frame, slid there from where the step before held it;
			// a step without one lifts it
			static int heldX = -1, heldY = -1;
			if (s.touchX >= 0)
			{
				ui.touching = true;
				ui.touchX = s.touchX;
				ui.touchY = s.touchY;
				if (heldX >= 0)
				{
					ui.dragDX = s.touchX - heldX;
					ui.dragDY = s.touchY - heldY;
				}
			}
			heldX = s.touchX;
			heldY = s.touchY;
			inputTime += dt;
			if (first)
			{
				Player& p = session->w.player;
				logf("autoinput: step %d [%s] feet %.0f %.0f %.0f %s screen %d msgs %d ctrl %d target %s pending %d", (int)inputIndex,
					session->w.cellName().c_str(), p.feet[0], p.feet[1], p.feet[2],
					p.swimming ? "water" : p.onGround ? "ground" : "air", session->screen, (int)session->messages.size(),
					session->w.controlsEnabled, session->target >= 0 ? session->w.refs[session->target].id.c_str() : "-",
					(int)session->pendingMenus.size());
			}
			// (a flight takes as long as its distance needs: the step's seconds are a guess, FLYTO knows better)
			float stepSecs = s.secs;
			if (driver.busy() && driver.kind == TestDriver::FLY && driver.flyAllow > stepSecs)
				stepSecs = driver.flyAllow;
			if (driver.busy() && inputTime >= stepSecs)
				driver.fail("timeout");
			if (inputTime >= stepSecs || (stepBlocking && !driver.busy()))
			{
				if (inputTime >= 15.0f)
					logf("autoinput: step %zu took %.1f s (limit %.0f)", (size_t)inputIndex, inputTime, stepSecs);
				stepBlocking = false;
				inputIndex++;
				inputTime = 0.0f;
				if (inputIndex == autoinput.size())
				{
					logf("autoinput: end feet %.0f %.0f %.0f screen %d", session->w.player.feet[0], session->w.player.feet[1],
						session->w.player.feet[2], session->screen);
					autoShotNow = true;
					session->save();
					platformRunOver();         // (the PC test build ends here; the console carries on, into QUIT below)
					// QUIT: the game menu's Save and quit from here, the way a player leaves. The quit on the console ran
					// code no test did: a crash at exit froze the console on a black screen
					if (s.quit)
					{
						session->openScreen(SCR_GAMEMENU);
						session->quitSaving = 1;
					}
				}
			}
		}
		else if (touching)
		{
			ui.touching = true;
			ui.touchX = tp.px;
			ui.touchY = tp.py;
			if (touchWasDown)
			{
				ui.dragDX = tp.px - lastTouchX;
				ui.dragDY = tp.py - lastTouchY;
				if (abs(tp.px - pressX) + abs(tp.py - pressY) > kTapSlop)
					dragged = true;
			}
			else
			{
				dragged = false;
				pressX = tp.px;
				pressY = tp.py;
			}
			lastTouchX = tp.px;
			lastTouchY = tp.py;
		}
		else if (touchWasDown && !dragged)
		{
			ui.tapped = true;
			ui.tapX = lastTouchX;
			ui.tapY = lastTouchY;
		}
		touchWasDown = touching;
		ui.down = down;
		ui.held = held;
		ui.up = up;
		ui.stickY = inputIndex < autoinput.size() && loaded ? autoinput[inputIndex].stickY : readStickScroll();
		ui.dt = dt;
		uiBeginFrame(ui);

		if (session->wantName)
		{
			if (session->autotest)
				session->setName("Tester");
			else
			{
				SwkbdState kb;
				char name[32] = {};
				swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, 24);
				swkbdSetHintText(&kb, "What is your name?");
				swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
				SwkbdButton button = swkbdInputText(&kb, name, sizeof(name));
				session->setName(button == SWKBD_BUTTON_CONFIRM ? name : "Stranger");
				lastTick = svcGetSystemTick();
			}
		}
		// The name of the spell / item being made, or the journal's search: the system keyboard, starting from the
		// text there now (a name can't be left blank; an empty search shows everything again)
		if (session->wantText)
		{
			if (session->autotest)
				session->setText(true, session->typedText);
			else
			{
				SwkbdState kb;
				char text[256] = {};
				bool search = session->wantText == Session::TEXT_SEARCH;
				bool console = session->wantText == Session::TEXT_CONSOLE;      // a script line (Options > Console)
				bool classDesc = session->wantText == Session::TEXT_CLASSDESC;  // the made class's description (may be empty)
				swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, console || classDesc ? 200 : 31);
				swkbdSetHintText(&kb, console ? "Script line, e.g. Player->AddItem gold_001 100" : search ? "Search"
					: classDesc ? "Describe your class" : session->wantText == Session::TEXT_CLASSNAME ? "Name your class"
					: session->screen == SCR_ENCHANT ? "Name the item" : "Name the spell");
				swkbdSetInitialText(&kb, session->textNow().c_str());
				if (!search && !console && !classDesc)
					swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
				SwkbdButton button = swkbdInputText(&kb, text, sizeof(text));
				session->setText(button == SWKBD_BUTTON_CONFIRM, text);
				lastTick = svcGetSystemTick();
			}
		}
		// A movie: the game waits; A, B or START skips it
		if (loaded && session->moviePlaying())
		{
			if (down & (KEY_A | KEY_B | KEY_START))
				session->stopMovie();
			else
				session->movieUpdate(dt);
			audioUpdate();                    // (the movie's sound streams on the music channel: the game's update that feeds it isn't running)
			C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
			linearRetire();
			C2D_Prepare();
			C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
			clearTop(top, 0x000000FF);
			C2D_SceneBegin(top);
			session->movieDraw();
			C2D_TargetClear(bottom, C2D_Color32(0, 0, 0, 255));
			C2D_SceneBegin(bottom);
			uiTextCentered(160, 110, 0.45f, col::textDim, "A: skip");
			C2D_Flush();
			MARK("frame end");
			g_mainFrames++;
			C3D_FrameEnd(0);
			continue;
		}
		if (loaded)
		{
			if (down & KEY_START)
				session->toggleGameMenu();
			u64 updStart = svcGetSystemTick();
			MARK("session update");
			session->frameDt = dt;         // (the frame's own time: a menu's panning goes on while the world stands still)
			session->update(in, session->worldPaused() || g_halt ? 0.0f : dt);      // (a menu stops the world; the test driver keeps its own clock)
			MARK("after update");
			extern float g_profWorldMs;
			g_profWorldMs += (svcGetSystemTick() - updStart) * 1000.0f / SYSCLOCK_ARM11;
			// `new` ran out and spent the reserve: memory back before the next allocation finds none
			if (g_memoryLow)
				session->relieveMemory();
		}

		// Death screen asked to load the last save: start the session over from it
		if (loaded && session->wantReload)
		{
			bool testGodKept = session->testGod, testGodBlowsKept = session->testGodBlows, testLegitKept = session->testLegit;
			int gammaKept = g_gammaPct, autoGammaKept = g_autoGamma;     // (the new session reads settings.txt)
			drawLoading(top, bottom, "Loading...");
			std::string path = session->reloadPath.empty() ? kSavePath : session->reloadPath;
			logf("reload from %s, heap used %d KB", path.c_str(), mallinfo().uordblks / 1024);
			session->shutdown();
			logf("reload: after shutdown, heap used %d KB", mallinfo().uordblks / 1024);
			delete session;
			logf("reload: after the old session, heap used %d KB, %d zopen streams open", mallinfo().uordblks / 1024, zOpenCount());
			session = new Session();
			session->dataDir = dataDir;
			{
				LoadingProgress progress(top, bottom, "Loading...");
				loaded = session->start(dataDir, "", path.c_str());
				if (loaded)
				{
					session->vm.load(session->w);
					uiLoadTheme(dataDir, session->w.game.ui);
			g_splashes = session->w.game.art.splash;
				}
			}
			if (!testRun && !retryFromTitle(loaded))
				break;
			session->savePath = kSavePath;
			// a test reload: still a test (and still invincible if it was), its Display settings kept as the case set them
			session->autotest = !autoinput.empty();
			if (session->autotest)
			{
				g_gammaPct = gammaKept;
				g_autoGamma = autoGammaKept;
			}
			logBuffered(!session->autotest);          // play: the log written every 2 s, tests every line
			session->testGod = testGodKept;
			session->testGodBlows = testGodBlowsKept;
			session->testLegit = testLegitKept;
			lastTick = svcGetSystemTick();
			continue;
		}

		// A door to another cell: show the loading screen for a frame, then load
		if (loaded && session->travelCell >= 0)
		{
			std::string text = "Loading " + session->w.cells[session->travelCell].name + "...";
			drawLoading(top, bottom, text.c_str());
			LoadingProgress progress(top, bottom, text);
			session->finishTravel();
			lastTick = svcGetSystemTick();
			continue;
		}

		RenderCamera cam = { { session->w.player.feet[0], session->w.player.feet[1],
			playerEyeZ(session->w.player) }, session->w.player.yaw, session->w.player.pitch };
		if (loaded)
			session->viewCamera(cam);            // third person: behind the player
		if (loaded && g_camHeld)
			cam = g_heldCam;
		bool autocamShot = false;
		if (poseIndex < autocam.size() && loaded)
		{
			const std::string& want = autocam[poseIndex].cell;
			int ci = want.empty() ? session->w.current : session->w.cellIndex(want);
			if (ci >= 0 && ci != session->w.current)
			{
				drawLoading(top, bottom, ("Loading " + want + "...").c_str());
				const LevelCell& lc = session->w.cells[ci];
				if (!lc.interior)
				{
					session->w.player.feet[0] = (lc.gx + 0.5f) * 8192.0f;
					session->w.player.feet[1] = (lc.gy + 0.5f) * 8192.0f;
				}
				session->w.enterCell(ci);
				lastTick = svcGetSystemTick();
				continue;
			}
			cam = autocam[poseIndex].cam;
			if (++poseFrames == 4)
				autocamShot = true;
		}

		u64 workEnd = svcGetSystemTick();       // the frame's update, before waiting for the GPU
		// The first frames after a load say how far they get (a GPU hang would stop in FrameBegin)
		static int traced = 0;
		bool trace = loaded && traced < 3;
		if (trace)
			logf("frame %d: begin", traced);
		MARK("frame begin");
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		linearRetire();                         // the GPU is done with what the update freed
		if (trace)
			logf("frame %d: GPU done with the previous frame", traced);
		if (autocamShot)
		{
			char path[64];
			snprintf(path, sizeof(path), "sdmc:/3ds/mw3ds/shot_%02d.bmp", (int)poseIndex);
			screenshotSave(path);
			poseIndex++;
			poseFrames = 0;
			if (poseIndex == autocam.size())
				logf("autocam done");
		}
		if ((autoShot && t >= 3.0f) || autoShotNow)
		{
			autoShot = false;
			logf("autoshot: %s", screenshotSave("sdmc:/3ds/mw3ds/shot.bmp") ? "saved" : "FAILED");
		}
		if (stepShot)
		{
			stepShot = false;
			char path[64];
			snprintf(path, sizeof(path), "sdmc:/3ds/mw3ds/shot_step_%02d.bmp", stepShots++);
			logf("stepshot: %s %s", path, screenshotSave(path) ? "saved" : "FAILED");
		}
		if (!driver.failShot.empty())
		{
			logf("drive: screenshot %s %s", driver.failShot.c_str(), screenshotSave(driver.failShot.c_str()) ? "saved" : "FAILED");
			driver.failShot.clear();
		}
		// MW3DS_FAILFAST=1 (the PC test build): the run ends at its first failure (after its screenshot): a fix is tried on
		// that, not on the failures it drags after it, each a step running to its limit. Never on the console
		else if (g_testFailed && platformFailFast())
		{
			logf("autoinput: stopped at the first failure (MW3DS_FAILFAST)");
			platformRunOver();
		}

		if (loaded)
			session->updateLocalMap();

		// Stereo 3D: eases in and out; the screen leaves 3D mode only once the depth is gone
		float slider = osGet3DSliderState();
		float target = loaded && !autoShotNow && poseIndex >= autocam.size() ? session->stereoTarget(slider, dt) : 0.0f;
		float& amount = session->stereoAmount;
		amount = target > amount ? fminf(target, amount + dt * 2.0f) : fmaxf(target, amount - dt * 2.0f);
		bool wantStereo = amount > 0.0f || target > 0.0f;
		if (wantStereo != stereoOn)
		{
			gfxSet3D(wantStereo);
			stereoOn = wantStereo;
		}
		// Half the eye separation in world units at full slider (a person's is ~4.5 units): depth
		// between near and far scales with it. The focus (renderer.cpp) sets what sits on the screen
		const float kMaxEyeShift = 3.0f;
		float shift = slider * kMaxEyeShift * amount;
		// Outdoors the fog closes in while frames run long, and with the weather
		float fogScale = 1.0f;
		if (loaded && !session->w.cells[session->w.current].interior)
			fogScale = session->viewNow() * session->w.weatherFogScale();
		// (the horizon: distant land as far as the weather lets one see, not shortened with the detailed view; under
		// water no farther than the murk)
		rendererSetHorizon(loaded ? session->w.weatherFogScale() : 0.0f);
		if (loaded)
			session->w.drawScale = fogScale;

		const u8* fog = loaded ? session->w.here().fog : (const u8*)"\0\0\0\xff";
		u32 clear = (fog[0] << 24) | (fog[1] << 16) | (fog[2] << 8) | 0xFF;
		if (loaded)
		{
			// Day and night outdoors: the light, sky and fog for this hour
			float land[3], sky[3], dayFog[3];
			session->w.daylight(land, sky, dayFog);
			bool outdoors = !session->w.cells[session->w.current].interior && session->w.game.hasWeather;
			// Night Eye adds light to everything, in the dark too (OpenMW's updateAmbient: the ambient + 0.7 x
			// min(1, magnitude / 100)); it was a scale on the time of day's colour, near black at night: no help
			rendererSetNightEye(0.7f * fminf(1.0f, session->w.effectTotal(43) / 100.0f));
			// Gamma (Display): the light curved, the fog and the clear colour with it
			rendererSetGamma(gammaUpdate(dt));
			// Light brightens what the player sees a little
			float boost = 1.0f + (session->w.effectTotal(41) > 0.0f ? 0.25f : 0.0f);
			if (boost > 1.0f)
			{
				for (int c = 0; c < 3; c++)
					land[c] = fminf(2.0f, land[c] * boost);
				outdoors = true;           // the tint applies indoors too while it lasts
			}
			// Under water (OpenMW's FogManager): Morrowind.ini's Underwater Color (12, 30, 37) weighed 0.85 against the
			// fog's colour, the fog thick from the eye (Underwater Fog) to the view's end
			Cell& here = session->w.here();
			bool underwater = here.hasWater() && playerEyeZ(session->w.player) < here.waterZ;
			rendererSetUnderwater(underwater, session->w.underwaterFog());
			if (underwater)
			{
				static const float kColour[3] = { 12.0f / 255.0f, 30.0f / 255.0f, 37.0f / 255.0f };
				for (int c = 0; c < 3; c++)
					dayFog[c] = kColour[c] * 0.85f + dayFog[c] * 0.15f;
				outdoors = true;
				rendererSetHorizon(0.0f);
			}
			rendererSetDaylight(land, sky, dayFog, outdoors);
			float side[4];
			session->w.sunSide(side);
			rendererSetSunSide(side);
			if (outdoors)
				clear = ((u32)(dayFog[0] * 255) << 24) | ((u32)(dayFog[1] * 255) << 16) | ((u32)(dayFog[2] * 255) << 8) | 0xFF;
			u32 curved = 0xFF;
			for (int sh = 8; sh < 32; sh += 8)
				curved |= (u32)(rendererCurve(((clear >> sh) & 0xFF) / 255.0f) * 255.0f + 0.5f) << sh;
			clear = curved;
		}
		float eyeCmd = 0.0f;
		float aimDepth = stereoOn && loaded ? session->aimDepth() : 0.0f;
		int hudDrawsLeft = 0, worldDrawsLeft = 0;
		for (int eye = 0; eye < (stereoOn ? 2 : 1); eye++)
		{
			C3D_RenderTarget* target3d = eye ? topRight : top;
			clearTop(target3d, clear);
			C3D_FrameDrawOn(target3d);
			float cmdBefore = C3D_GetCmdBufUsage();
			if (loaded)
			{
				int worldDraws = rendererDrawWorld(session->w, cam, stereoOn ? (eye ? shift : -shift) : 0.0f, eye == 1, fogScale);
				// Both eyes draw the same things (the second reuses the first's lists): a count that differs is
				// something in one eye only
				if (eye == 0)
					worldDrawsLeft = worldDraws;
				else if (worldDraws != worldDrawsLeft)
					monitorOnce(("eyes:" + session->w.cellName()).c_str(), "the world drew %d things in the left eye and %d in the right (%s)",
						worldDrawsLeft, worldDraws, session->w.cellName().c_str());
			}
			if (loaded && poseIndex >= autocam.size())
				session->drawExtras(cam.pos, eye == 1, !g_camHeld);
			if (eye == 0)
				eyeCmd = C3D_GetCmdBufUsage() - cmdBefore;

			C2D_Prepare();
			C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
			// The world's last draw leaves its blend / alpha test / culling behind: the UI needs
			// blending (glyphs are alpha-only), no alpha test, both faces
			C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA,
				GPU_ONE_MINUS_SRC_ALPHA);
			C3D_AlphaTest(false, GPU_ALWAYS, 0);
			C3D_CullFace(GPU_CULL_NONE);
			C2D_SceneBegin(target3d);
			// The HUD (subtitles, notes, crosshair) floats a little in front of the world: flat on the screen
			// surface, it sat behind what's nearer than the stereo focus (the speaker, the weapon) while
			// covering it, a depth clash that makes text hard to read. Left eye right, right eye left
			float hudShift = stereoOn ? slider * 3.0f : 0.0f;
			C2D_ViewReset();
			C2D_ViewTranslate(eye ? -hudShift : hudShift, 0.0f);
			// The crosshair and the label of what it's on sit at the depth of what it points at (drawn
			// flat, they doubled while the eyes looked at the target): its stereo offset, less the HUD's
			session->aimShift = 0.0f;
			session->hudEye = eye;
			session->hudEyes = stereoOn ? 2 : 1;
			if (stereoOn && loaded)
				session->aimShift = rendererStereoPixels(aimDepth, eye ? shift : -shift) - (eye ? -hudShift : hudShift);
			extern int g_uiDraws;
			g_uiDraws = 0;
			if (loaded)
			{
				if (!g_hudOff)
					session->drawTop();
			}
			else
				uiTextCentered(200, 110, 0.6f, col::header, "Load failed - see sdmc:/3ds/mw3ds/log.txt");
			// The same HUD in both eyes: a pass that drew a different number of things is a one-eye element
			if (loaded && stereoOn)
			{
				if (eye == 0)
					hudDrawsLeft = g_uiDraws;
				else if (g_uiDraws != hudDrawsLeft)
					session->noteHudMismatch(hudDrawsLeft, g_uiDraws);
			}
			// citro2d draws what it queued only when flushed, into whatever target is bound then: without this
			// the left eye's HUD went out after the right eye's target was bound, and both HUDs landed in one eye
			C2D_Flush();
			C2D_ViewReset();
		}
		C2D_TargetClear(bottom, C2D_Color32(10, 8, 6, 255));
		C2D_SceneBegin(bottom);
		if (loaded)
			session->drawBottom();
		cmdPeak = fmaxf(cmdPeak, C3D_GetCmdBufUsage());
		if (trace)
			logf("frame %d: built (%d%% of the command buffer), submitting", traced, (int)(C3D_GetCmdBufUsage() * 100.0f));
		MARK("frame end");
		g_mainFrames++;
		C3D_FrameEnd(0);
		if (trace)
			logf("frame %d: submitted", traced++);
		// The frame rate the session asks for (an Old 3DS: 30 or 20, Session::adaptView)
		static int capNow = 60;
		int capWant = session->autotest && !g_testFrameCap ? 60 : session->frameCap;
		if (loaded && capWant != capNow)
		{
			capNow = capWant;
			C3D_FrameRate((float)capNow);
		}
		if (loaded)
		{
			session->noteRenderCost(C3D_GetProcessingTime(), C3D_GetDrawingTime(), stereoOn, eyeCmd);
			float updateMs = (float)(workEnd - now) * 1000.0f / SYSCLOCK_ARM11;
			if (poseIndex >= autocam.size() && inputIndex >= autoinput.size())
				session->adaptView(fmaxf(updateMs + C3D_GetProcessingTime(), C3D_GetDrawingTime()));
		}

		frames++;
		float statSecs = (float)(now - statTick) / SYSCLOCK_ARM11;
		if (statSecs >= 1.0f)
		{
			session->fps = frames / statSecs;
			session->cpuMs = C3D_GetProcessingTime();
			session->gpuMs = C3D_GetDrawingTime();
			extern float g_profActorsMs, g_profWorldMs;
			float talk[3] = {};
			extern int g_drawnBatches, g_culledBatches;
			(void)talk;
			logf("profile per frame: update %.2fms (world %.2f player %.2f stream %.2f ai %.2f scripts %.2f other %.2f) "
				"actors drawn %.2fms, batches %d drawn %d culled, 3D %s",
				g_profWorldMs / frames, g_profParts[PROF_WORLD] / frames, g_profParts[PROF_PLAYER] / frames,
				g_profParts[PROF_STREAM] / frames, g_profParts[PROF_AI] / frames, g_profParts[PROF_SCRIPTS] / frames,
				g_profParts[PROF_OTHER] / frames, g_profActorsMs / frames, g_drawnBatches, g_culledBatches,
				stereoOn ? "on" : "off");
			logf("view distance %d%%, frame work %.2fms", (int)(session->viewScale * 100.0f), session->workCost);
			g_profActorsMs = g_profWorldMs = 0.0f;
			for (auto& p : g_profParts)
				p = 0.0f;
			extern int g_skippedDraws;
			logf("fps %.1f cpu %.2fms gpu %.2fms linearFree %luKB heapUsed %dKB gpu commands peak %d%%%s",
				session->fps, session->cpuMs, session->gpuMs, linearSpaceFree() / 1024, mallinfo().uordblks / 1024,
				(int)(cmdPeak * 100.0f), g_skippedDraws ? " (buffer full: draws skipped)" : "");
			cmdPeak = 0.0f;
			g_skippedDraws = 0;
			// The heap: the most used so far and, every few seconds, the biggest block left (fragmentation).
			// Never probed while a cell loads in the background: the probe briefly holds the biggest free
			// block (real allocations), and the streaming thread's own allocation then fails for nothing —
			// a spurious out-of-memory that purged far cells and churned the heap it was watching
			heapNote(mallinfo().uordblks);
			static int heapSecs = 0;
			if (++heapSecs >= 5 && !session->w.streamJob)
			{
				heapSecs = 0;
				logf("heap: %dKB used, %uKB peak, %uKB in the biggest free block, %dKB in holes, %d objects in memory (%d slots)",
					mallinfo().uordblks / 1024, (unsigned)(heapHighWater() / 1024), (unsigned)(heapLargestBlock() / 1024),
					mallinfo().fordblks / 1024, session->w.refsInMemory(), (int)session->w.refs.size());
			}
			uiCheckTheme();                   // the UI's textures still as loaded (a monitor)
			frames = 0;
			statTick = now;
		}
	}

	// Each step marked and timed, and the log left open to the last: a quit that hangs names its step in the
	// watchdog's line (log.txt and watchdog.txt) instead of ending the log at "exit"
	logf("exit");
	char exitTimes[256] = "";
	const char* stepName = nullptr;
	u64 exitStart = osGetTime(), stepStart = exitStart;
	auto exitStep = [&](const char* mark) {
		u64 now = osGetTime();
		size_t used = strlen(exitTimes);
		if (stepName)
			snprintf(exitTimes + used, sizeof(exitTimes) - used, "%s%s %llu", used ? ", " : "", stepName + 6, now - stepStart);
		stepName = mark;
		stepStart = now;
		MARK(mark);
	};
	exitStep("exit: sensors");
	gyroStop();
	exitStep("exit: session shutdown");
	session->shutdown();
	exitStep("exit: session delete");
	delete session;
	// (the distant land outlives a session; its file, open on romfs, has to close before romfs goes: userAppExit)
	exitStep("exit: distant land");
	distantFree();
	exitStep("exit: gamma");
	gammaExit();
	exitStep("exit: audio");
	audioExit();
	exitStep("exit: ui");
	uiExit();
	exitStep("exit: renderer");
	rendererExit();
	exitStep("exit: citro2d");
	C2D_Fini();
	exitStep("exit: citro3d");
	C3D_Fini();
	// (romfs: unmounted in userAppExit, once newlib's exit has closed whatever is still open on it)
	exitStep("exit: irrst");
	irrstExit();
	exitStep("exit: gfx");
	gfxExit();
	exitStep("exit: process");
	logf("exit: shut down in %llu ms (ms a step: %s)", osGetTime() - exitStart, exitTimes);
	logExit();
	return 0;
}
