// The game (Engine): start-up, the title screen, loading, input and the frame loop, shared by the console build and
// the PC test build. Each build's entry file (source/main.cpp, tools/test/native/main_native.cpp) is a few lines:
// what differs between them goes through platform/platform.hpp, so the two can't drift apart.
#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <malloc.h>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "engine.hpp"
#include "mwsound/soundmanagerimp.hpp"
#include "mwrender/objectpaging.hpp"
#include "platform/linear.hpp"
#include "platform/memstat.hpp"
#include "components/debug/debuglog.hpp"
#include "components/debug/profile.hpp"
#include "mwrender/renderingmanager.hpp"
#include "platform/gamma.hpp"
#include "mwrender/screenshotmanager.hpp"
#include "components/files/zfile.hpp"
#include "platform/platform.hpp"
#include "testdrive/steprunner.hpp"
#include "mwinput/inputmanagerimp.hpp"
#include "mwgui/loadingscreen.hpp"

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

static const char* const kSavePath = kAutosavePath;

// The top screen's targets (the right eye drawn only while 3D is on): a pair with the 24-bit depth buffer and a pair
// with the 16-bit one, both made at the start (Engine::topDepth16).
// The colour buffer: 32-bit on both consoles (platform/console.hpp rgb565 keeps the 16-bit one, half the buffer and the
// copy to the screen each frame: it banded the sky and the fog on an Old 3DS)
void Engine::createTopTargets()
{
	GPU_COLORBUF colorFmt = g_profile.rgb565 ? GPU_RB_RGB565 : GPU_RB_RGBA8;
	for (int d = 0; d < 2; d++)
		for (int eye = 0; eye < 2; eye++)
			topPair[d][eye] = C3D_RenderTargetCreate(240, 400, colorFmt, d ? GPU_RB_DEPTH16 : GPU_RB_DEPTH24_STENCIL8);
	// (no room in VRAM for the second pair: 3D draws with the 24-bit one)
	if (!topPair[1][0] || !topPair[1][1])
	{
		logf("display: no VRAM for the 16-bit depth targets, 3D keeps 24 bits");
		for (int eye = 0; eye < 2; eye++)
		{
			if (topPair[1][eye])
				C3D_RenderTargetDelete(topPair[1][eye]);
			topPair[1][eye] = topPair[0][eye];
		}
	}
	useTopDepth(false);
}

// The screen's output comes from the pair in use (the other pair keeps its buffers, drawn on nothing)
void Engine::useTopDepth(bool depth16)
{
	u32 transfer = g_profile.rgb565 ? DISPLAY_TRANSFER_FLAGS(GX_TRANSFER_FMT_RGB565, GX_TRANSFER_FMT_RGB565)
		: DISPLAY_TRANSFER_FLAGS(GX_TRANSFER_FMT_RGBA8, GX_TRANSFER_FMT_RGB8);
	top = topPair[depth16][0];
	topRight = topPair[depth16][1];
	C3D_RenderTargetSetOutput(top, GFX_TOP, GFX_LEFT, transfer);
	C3D_RenderTargetSetOutput(topRight, GFX_TOP, GFX_RIGHT, transfer);
	topDepth16 = depth16;
	logf("display: top depth %s", depth16 ? "16-bit" : "24-bit and stencil");
}

int Engine::go()
{
	if (!prepare())
		return 0;
	while (aptMainLoop())
		if (!frame())
			break;
	shutdown();
	return 0;
}

// Consoles, screens, the data, the title screen and the first session; false: the player quit before playing
bool Engine::prepare()
{
	APT_CheckNew3DS(&isNew3ds);
	if (isNew3ds)
		osSetSpeedupEnable(true);
	// What this console can do: from its CPU and from the heap it got (platform/console.hpp)
	consoleProfileInit(isNew3ds, __ctru_heap_size);
	// A slow CPU's one game core is busy drawing: cells stream in on the system core. Half of it may be ours (a cell
	// took 2.5 to 3.7 s to stream at 30%); the usual 30% if the system won't give that
	u32 sysCorePercent = 0;
	if (g_profile.slowCpu)
	{
		g_sysCoreThreads = R_SUCCEEDED(APT_SetAppCpuTimeLimit(50)) || R_SUCCEEDED(APT_SetAppCpuTimeLimit(30));
		APT_GetAppCpuTimeLimit(&sysCorePercent);
	}

	// The Old 3DS's 16-bit colour buffer goes to a 16-bit top screen (see DISPLAY_TRANSFER_FLAGS). The screens'
	// framebuffers are in VRAM, beside the render targets (a few MB of its 6 free): a MB of linear memory back for cells
	platformClearVram();
	gfxInit(g_profile.rgb565 ? GSP_RGB565_OES : GSP_BGR8_OES, GSP_BGR8_OES, true);
	// The bottom screen keeps one framebuffer: a frame that doesn't draw it (the HUD is drawn every third frame) shows
	// the last one drawn, not the one before that
	gfxSetDoubleBuffering(GFX_BOTTOM, false);
	// gfxInit lights the screens on buffers that hold whatever VRAM had (multicoloured static, on an Old 2DS and when
	// started from FBI): they stay forced black until the first black frame below is up
	GSPGPU_SetLcdForceBlack(1);
	// GPU commands of a whole frame (both eyes, bottom screen): overflowing it is a panic, so it is
	// generous, the renderer stops drawing near the end and 3D backs off when one eye uses too much.
	// (an Old 3DS's is half: platform/console.cpp)
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * g_profile.cmdBufScale);
	C2D_Init(4096);
	C2D_Prepare();
	irrstInit();
	logInit();
	crashHandlerInstall(0);
	watchdogStart();
	linearLockInit();
	char sysCore[64] = "";
	if (g_sysCoreThreads)
		snprintf(sysCore, sizeof(sysCore), ", cells stream on the system core (%lu%% of it ours)", (unsigned long)sysCorePercent);
	logf("MW3DS start, %s, heap %lu KB, linear %lu KB, memory mode %lu%s", isNew3ds ? "New 3DS" : "Old 3DS",
		__ctru_heap_size / 1024, __ctru_linear_heap_size / 1024, (unsigned long)osGetApplicationMemType(), sysCore);
	logf("console: %s CPU, %s heap (%d objects, %u KB of sounds kept)", g_profile.slowCpu ? "slow" : "fast",
		g_profile.smallHeap ? "small" : "large", g_profile.refBudget, g_profile.soundCacheBytes / 1024);
	gammaInit();
	romfs = R_SUCCEEDED(romfsInit());
	s_romfs = romfs;                   // (unmounted in userAppExit)
	struct stat st;
	logf("memory: %s, heap %lu KB, linear %lu KB (%lu KB free)", isNew3ds ? "New 3DS" : "Old 3DS",
		__ctru_heap_size / 1024, __ctru_linear_heap_size / 1024, linearSpaceFree() / 1024);

	logf("display: top screen %s", g_profile.rgb565 ? "16-bit, copied RGB565 to RGB565" : "24-bit, copied RGBA8 to RGB8");
	createTopTargets();
	bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
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
	dataDir = romfs && stat("romfs:/data/game.json", &st) == 0 ? kRomfsDataDir
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
		return false;
	}
	audioInit(dataDir);
	drawLoading(top, bottom, "Loading...");

	session = new Session();
	autoinput = loadAutoinput();
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
	// Normal play: title screen to pick a save (autosave, own saves, bundled starting points) or a new
	// game. Scripted test runs and start.txt skip it and start fresh.
	testRun = session->autotest || stat("sdmc:/3ds/mw3ds/autocam.txt", &st) == 0;
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
			return false;
		}
		logf("title: %s", startSave.empty() ? "new game" : startSave.c_str());
		MARK("after the title");
		drawLoading(top, bottom, "Loading...");
		MARK("loading screen drawn");
		loadStart = osGetTime();
	}
	session->dataDir = dataDir;
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
		return false;
	}
	session->savePath = kSavePath;      // test runs still save (at the end of scripted input)
	memoryReserveTake();
	logf("load %s in %llu ms, linear free %lu KB, heap used %d KB", loaded ? "ok" : "FAILED",
		osGetTime() - loadStart, linearSpaceFree() / 1024, mallinfo().uordblks / 1024);

	RenderCamera spawnCam = { { session->w.player.feet[0], session->w.player.feet[1],
		session->w.player.feet[2] + PLAYER_EYE_HEIGHT }, session->w.player.yaw, 0.0f };
	autocam = loadAutocam(spawnCam);
	autoShot = autocam.empty() && autoinput.empty() && stat("sdmc:/3ds/mw3ds/autoshot", &st) == 0;

	lastTick = svcGetSystemTick();
	statTick = lastTick;
	return true;
}

// A save that won't read (Session::start's saveUnreadable): the title again to pick another, nothing started that would
// autosave over it. False: the player quit there
bool Engine::retryFromTitle(bool& loadedOut)
{
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
}

// One pass of the loop: input, the scripted step, the update, both screens drawn; false: the loop ends
bool Engine::frame()
{
	hidScanInput();
	irrstScanInput();
	u32 down = hidKeysDown(), held = hidKeysHeld(), up = hidKeysUp();
	if (!loaded && (down & KEY_START))
		return false;
	if (loaded && session->wantQuit)
		return false;

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
		runScriptedStep(session, autoinput, inputIndex, inputTime, dt, in, down, held, ui, touching, autoShotNow, driver, chainMode, stepBlocking, stepShot);
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
	ui.stickX = inputIndex < autoinput.size() && loaded ? autoinput[inputIndex].stickX : readStickTurn();
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
			bool offer = session->wantText == Session::TEXT_OFFER;          // the gold offered in a barter: the number pad
			if (offer)
				swkbdInit(&kb, SWKBD_TYPE_NUMPAD, 2, 6);
			else
				swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, console || classDesc ? 200 : 31);
			swkbdSetHintText(&kb, offer ? "Your offer (gold)" : console ? "Script line, e.g. Player->AddItem gold_001 100" : search ? "Search"
				: classDesc ? "Describe your class" : session->wantText == Session::TEXT_CLASSNAME ? "Name your class"
				: session->screen == SCR_ENCHANT ? "Name the item" : "Name the spell");
			swkbdSetInitialText(&kb, session->textNow().c_str());
			if (!search && !console && !classDesc && !offer)
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
		return true;
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
			return false;
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
		return true;
	}

	// A door to another cell: show the loading screen for a frame, then load
	if (loaded && session->travelCell >= 0)
	{
		std::string text = "Loading " + session->w.cells[session->travelCell].name + "...";
		drawLoading(top, bottom, text.c_str());
		LoadingProgress progress(top, bottom, text);
		session->finishTravel();
		lastTick = svcGetSystemTick();
		return true;
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
			return true;
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
	// Pictures kept for long go to VRAM while nothing draws from them (platform/linear.hpp)
	texToVramNow();
	MARK("frame begin");
	C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
	{
		u64 since = workEnd;
		drawProfAdd(DRAWP_WAIT, since);     // (how long the GPU still had on the last frame)
	}
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

	u64 drawSince = svcGetSystemTick();
	if (loaded)
		session->updateLocalMap();
	drawProfAdd(DRAWP_MAP, drawSince);

	// Stereo 3D: eases in and out; the screen leaves 3D mode only once the depth is gone
	float slider = g_test3dSlider >= 0.0f ? g_test3dSlider : osGet3DSliderState();
	float target = loaded && !autoShotNow && poseIndex >= autocam.size() ? session->stereoTarget(slider, dt) : 0.0f;
	float& amount = session->stereoAmount;
	amount = target > amount ? fminf(target, amount + dt * 2.0f) : fmaxf(target, amount - dt * 2.0f);
	bool wantStereo = amount > 0.0f || target > 0.0f;
	if (wantStereo != stereoOn)
	{
		gfxSet3D(wantStereo);
		stereoOn = wantStereo;
	}
	// (16 bits of depth: Options > Display, g_depthMode; a scripted run asks for it with DEPTH16:on)
	bool want16 = g_testDepth16 || g_depthMode == DEPTH_16 || (g_depthMode == DEPTH_16_IN_3D && wantStereo);
	if (want16 != topDepth16)
		useTopDepth(want16);
	// The slider's first third is the plain stereo view, its whole range: half the eye separation in world units
	// grows to kMaxEyeShift (a person's is ~4.5 units) and depth between near and far scales with it. The focus
	// (mwrender/renderingmanager.cpp) sets what sits on the screen
	const float kNearDepthTo = 1.0f / 3.0f;
	const float kMaxEyeShift = 3.0f;
	float nearDepth = fminf(1.0f, slider / kNearDepthTo);
	float shift = nearDepth * kMaxEyeShift * amount;
	// Past ~8 m a plain stereo view is flat (its depth goes as 1 / distance): the other two thirds add depth for
	// each doubling of the distance beyond (mwrender/renderingmanager.cpp kStereoKnee), evenly up to kFarDepthShare,
	// nearer things as the first third left them. Within the first third the shaders skip it (no GPU cost)
	const float kFarDepthShare = 1.0f;
	float farDepth = fminf(1.0f, fmaxf(0.0f, (slider - kNearDepthTo) / (1.0f - kNearDepthTo)));
	rendererSetStereoCurve(stereoOn ? kFarDepthShare * farDepth : 0.0f);
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
	static int uiPeakTop = 0, uiPeakBottom = 0;     // the most UI pieces drawn in a frame this second (citro2d objects)
	for (int eye = 0; eye < (stereoOn ? 2 : 1); eye++)
	{
		C3D_RenderTarget* target3d = eye ? topRight : top;
		clearTop(target3d, clear);
		C3D_FrameDrawOn(target3d);
		float cmdBefore = C3D_GetCmdBufUsage();
		// The right eye from the left eye's recorded GPU words (mwrender/renderingmanager.cpp): the world and the
		// extras in no CPU time. Without a record, or no room for it, it is drawn again
		bool replayed = false;
		if (loaded && eye == 1 && rendererReplayWorld(shift) >= 0)
			replayed = true;
		if (loaded && !replayed)
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
		drawSince = svcGetSystemTick();
		if (loaded && !replayed && poseIndex >= autocam.size())
			session->drawExtras(cam.pos, eye == 1, !g_camHeld);
		drawProfAdd(DRAWP_EXTRAS, drawSince);
		if (loaded && eye == 0)
			rendererReplayEnd(worldDrawsLeft);
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
		float hudShift = stereoOn ? nearDepth * 3.0f : 0.0f;
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
			else
				session->drawHurtFlash();
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
		drawProfAdd(DRAWP_HUD, drawSince);
		uiPeakTop = std::max(uiPeakTop, g_uiDraws);
	}
	// The HUD (no menu, no message up) is drawn every third frame: 330 pieces of text and frame cost an Old 3DS 5 ms a
	// frame, and its bars and clock don't need more. Menus and messages draw every frame (taps, scrolling), and so does
	// the HUD while a finger is on the screen or just lifted: its buttons, quick slots and map read the tap as they draw
	bool hudOnly = loaded && session->screen == SCR_NONE && session->messages.empty() && !session->wantName;
	if (!hudOnly || g_mainFrames % 3 == 0 || ui.touching || ui.tapped)
	{
		extern int g_uiDraws;
		g_uiDraws = 0;
		C2D_TargetClear(bottom, C2D_Color32(10, 8, 6, 255));
		C2D_SceneBegin(bottom);
		if (loaded)
			session->drawBottom();
		drawProfAdd(DRAWP_BOTTOM, drawSince);
		uiPeakBottom = std::max(uiPeakBottom, g_uiDraws);
	}
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
		extern int g_drawnBatches, g_culledBatches;
		const float* pp = g_profParts;
		// (ai is combatUpdate less the detection rays and the visual sweeps, which have their own parts)
		logf("profile per frame: update %.2fms (world %.2f player %.2f stream %.2f ai %.2f scripts %.2f other %.2f "
			"detect %.2f fx %.2f greet %.2f sound %.2f tail %.2f) actors drawn %.2fms, batches %d drawn %d culled, 3D %s",
			g_profWorldMs / frames, pp[PROF_WORLD] / frames, pp[PROF_PLAYER] / frames, pp[PROF_STREAM] / frames,
			(pp[PROF_AI] - pp[PROF_DETECT] - pp[PROF_FX]) / frames, pp[PROF_SCRIPTS] / frames, pp[PROF_OTHER] / frames,
			pp[PROF_DETECT] / frames, pp[PROF_FX] / frames, pp[PROF_GREET] / frames, pp[PROF_SOUND] / frames,
			pp[PROF_TAIL] / frames, g_profActorsMs / frames, g_drawnBatches, g_culledBatches, stereoOn ? "on" : "off");
		const float* pd = g_profDraw;
		logf("profile draw: wait %.2f sky %.2f cull %.2f opaque %.2f distant %.2f layers %.2f actors %.2f blend %.2f "
			"extras %.2f hud %.2f bottom %.2f map %.2f ms, ui draws top %d bottom %d, "
			"calls disp %d rays %d floors %d steps %d objects %d, replay %d/%d",
			pd[DRAWP_WAIT] / frames, pd[DRAWP_SKY] / frames, pd[DRAWP_CULL] / frames, pd[DRAWP_OPAQUE] / frames,
			pd[DRAWP_DISTANT] / frames, pd[DRAWP_LAYERS] / frames, g_profActorsMs / frames, pd[DRAWP_BLEND] / frames,
			pd[DRAWP_EXTRAS] / frames, pd[DRAWP_HUD] / frames, pd[DRAWP_BOTTOM] / frames, pd[DRAWP_MAP] / frames,
			uiPeakTop, uiPeakBottom, g_profCalls[PCALL_DISPOSITION] / frames, g_profCalls[PCALL_RAYS] / frames,
			g_profCalls[PCALL_FLOORS] / frames, g_profCalls[PCALL_STEPS] / frames, g_profCalls[PCALL_OBJECTS] / frames,
			g_replayCount, g_replayMissed);
		g_replayCount = g_replayMissed = 0;
		logf("view distance %d%%, frame work %.2fms", (int)(session->viewScale * 100.0f), session->workCost);
		g_profActorsMs = g_profWorldMs = 0.0f;
		for (auto& p : g_profParts)
			p = 0.0f;
		for (auto& p : g_profDraw)
			p = 0.0f;
		for (auto& c : g_profCalls)
			c = 0;
		uiPeakTop = uiPeakBottom = 0;
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
		uiArtIdle();                      // pictures not shown for a while give their memory back
		frames = 0;
		statTick = now;
	}
	return true;
}

void Engine::shutdown()
{
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
}

int appMain()
{
	Engine engine;
	return engine.go();
}
