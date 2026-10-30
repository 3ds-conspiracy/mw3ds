// EXPECT values: player stats, saves, console, memory, menus and maps
#include <optional>
#include <3ds.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <queue>
#include <tuple>
#include "testdrive/testdrive.hpp"
#include "mwbase/session.hpp"
#include "components/debug/debuglog.hpp"

extern const char* g_playerBlock;
#include "platform/memstat.hpp"
#include "components/files/zfile.hpp"
#include <malloc.h>
#include "testdrive/testhelpers.hpp"
#include "components/misc/cardslices.hpp"

// The card files' write-once rule (components/misc/cardslices.hpp) on a scratch file: 0 when every step does what it
// should, else the first step that didn't
static int cardSlicesSteps()
{
	const char* path = "sdmc:/3ds/mw3ds/slices-test.bin";
	remove(path);
	u8 a[100], b[50], b2[50], c[70];
	for (int i = 0; i < 100; i++)
		a[i] = (u8)i;
	for (int i = 0; i < 50; i++)
		b[i] = b2[i] = (u8)(200 - i);
	b2[25] ^= 1;
	memset(c, 7, sizeof(c));
	u32 off[3], check[3];
	// (the slices given, a start's worth: how many bytes it wrote, -1 if the card refused one)
	auto start = [&](CardSlices& f, std::initializer_list<std::pair<const u8*, u32>> slices) -> int {
		if (!f.begin())
			return -1;
		int k = 0;
		for (auto& sl : slices)
		{
			if (!f.put(sl.first, sl.second, off[k], check[k]))
				return -1;
			k++;
		}
		return f.end() ? (int)f.written() : -1;
	};
	CardSlices f(path, 1);
	if (start(f, { { a, 100 }, { b, 50 }, { c, 70 } }) != 220) return 1;          // no file: all of it
	u32 cAt = off[2];
	if (start(f, { { a, 100 }, { b, 50 }, { c, 70 } }) != 0 || off[2] != cAt) return 2;   // the same: nothing
	if (start(f, { { a, 100 }, { b2, 50 }, { c, 70 } }) != 120) return 3;         // the second differs: it and on
	if (start(f, { { a, 100 }, { b2, 50 }, { c, 70 } }) != 0) return 4;
	if (start(f, { { a, 100 }, { b2, 50 } }) != 0) return 5;                       // one fewer: only the table
	if (start(f, { { a, 100 }, { b2, 50 } }) != 0) return 6;
	if (start(f, { { a, 100 }, { b2, 50 }, { c, 70 } }) != 70) return 7;          // one more: that one
	CardSlices other(path, 2);
	if (start(other, { { a, 100 }, { b2, 50 }, { c, 70 } }) != 220) return 8;     // another format: all of it
	// read back as the stores do: the slice checks out, and once a letter of it changes on the card it doesn't
	u8 back[70];
	FILE* r = fopen(path, "rb");
	bool same = r && fseek(r, off[2], SEEK_SET) == 0 && fread(back, 1, 70, r) == 70 && CardSlices::check(back, 70) == check[2];
	if (r)
		fclose(r);
	if (!same) return 9;
	FILE* w = fopen(path, "r+b");
	u8 bad = 8;
	bool hurt = w && fseek(w, off[2] + 10, SEEK_SET) == 0 && fwrite(&bad, 1, 1, w) == 1;
	if (w)
		fclose(w);
	r = fopen(path, "rb");
	same = r && fseek(r, off[2], SEEK_SET) == 0 && fread(back, 1, 70, r) == 70 && CardSlices::check(back, 70) == check[2];
	if (r)
		fclose(r);
	if (!hurt || same) return 10;
	other.damaged();                                                               // the next start writes it all
	if (start(other, { { a, 100 }, { b2, 50 }, { c, 70 } }) != 220) return 11;
	// a write under way: the header is cleared before the first slice goes, so a cut there leaves no whole file
	if (!other.begin()) return 12;
	other.put(a, 100, off[0], check[0]);
	other.put(b, 50, off[1], check[1]);
	char head[4] = { 1, 1, 1, 1 };
	r = fopen(path, "rb");
	bool cleared = r && fread(head, 1, 4, r) == 4 && memcmp(head, "\0\0\0\0", 4) == 0;
	if (r)
		fclose(r);
	other.end();
	if (!cleared) return 12;
	remove(path);
	return 0;
}

std::optional<bool> numberOfState(Session& s, const std::string& what, const std::string& arg, float& v)
{
	World& w = s.w;
	PlayerStats& st = w.stats;
	if (what == "level") v = (float)st.level;
	else if (what == "levelprog") v = (float)st.levelProgress;
	else if (what == "health") v = st.health;
	else if (what == "bloodhits") v = (float)s.bloodHits;
	else if (what == "bloodlow") v = s.bloodLow;
	else if (what == "bloodhigh") v = s.bloodHigh;
	else if (what == "bodymeshes") v = s.body.set.actors.empty() ? 0.0f : (float)s.body.set.actors[0].meshes.size();   // the third-person body's meshes
	else if (what == "bodywolf")     // the third-person body is the werewolf's own model (in wolf form), 1; else 0
		v = s.body.wolf && !s.body.set.actors.empty() && !s.body.set.actors[0].meshes.empty() ? 1.0f : 0.0f;
	else if (what == "bodyhits") v = (float)s.body.hitsPlayed;       // Hit groups the third-person body has played on being struck
	else if (what == "healthmax") v = st.healthMax;
	else if (what == "magicka") v = st.magicka;
	else if (what == "magickamax") v = st.magickaMax;
	else if (what == "magickalack") v = st.magickaMax - st.magicka;      // 0: full
	else if (what == "fatigue") v = st.fatigue;
	else if (what == "fatiguemax") v = st.fatigueMax;
	else if (what == "gold") v = (float)w.itemCount("gold_001");
	else if (what == "savenewestfirst")    // 1: the first of the player's own saves in the list is the one written last
	{
		v = 0;
		for (auto& sv : savesList(s.dataDir))
			if (!sv.autosave && !sv.bundled)
			{
				v = sv.path == g_lastSavePath ? 1.0f : 0.0f;
				break;
			}
	}
	else if (what == "savereads")    // times the save list was read from the card (the Saves screen opened from memory)
		v = (float)g_saveListReads;
	else if (what == "savemetareads")    // .meta files opened listing the saves (the saves' index lacked them)
		v = (float)g_saveMetaReads;
	else if (what == "saverow")     // <n>: row n of the save list shows its date and time whole at the right (1), and not
	{                                 // the character's name when all the saves are one character's; else 0
		std::vector<SaveInfo> list = savesList(s.dataDir);
		size_t n = (size_t)atoi(arg.c_str());
		v = 0.0f;
		if (n < list.size())
		{
			std::string left, right;
			saveRow(list, n, left, right);
			bool dated = right.find(':') != std::string::npos && uiTextWidth(right, 0.45f) + 16.0f < 312.0f;
			bool named = !list[n].name.empty() && left.find(list[n].name + " - ") != std::string::npos;
			bool oneName = true;
			for (const SaveInfo& o : list)
				oneName = oneName && (o.name.empty() || o.name == list[n].name);
			v = dated && !(named && oneName) ? 1.0f : 0.0f;
		}
	}
	else if (what == "saves")
	{
		// the player's own saves on the card (not the autosave or the starting points)
		v = 0;
		for (auto& sv : savesList(s.dataDir))
			if (!sv.autosave && !sv.bundled)
				v++;
	}
	else if (what == "bounty") v = (float)w.bounty;
	else if (what == "mapnotes") v = (float)w.mapNotes.size();                    // the player's map notes (Tribunal)
	else if (what == "mapnotehas")      // <text, underscores for spaces>: a map note holds it, 1
	{
		v = 0.0f;
		for (auto& nt : w.mapNotes)
			if (lower(nt.text).find(lower(spaced(arg))) != std::string::npos)
				v = 1.0f;
	}
	else if (what == "companionwarn") v = s.companionWarning ? 1.0f : 0.0f;      // the mercenary's warning is up
	else if (what == "sharing") v = s.screen == SCR_CONTAINER && s.companionSharing ? 1.0f : 0.0f;   // a companion's share is open
	else if (what == "conval") v = s.console.hasValue ? s.console.lastValue : -999999.0f;   // the last console expression's value
	else if (what == "conerr") v = s.console.lastError ? 1.0f : 0.0f;                    // the last console line did not run
	else if (what == "armor") v = s.playerArmor();              // the player's armor rating
	else if (what == "target") v = s.target >= 0 ? 1.0f : 0.0f;       // something under the crosshair
	else if (what == "forcesneak") v = w.forceSneak ? 1.0f : 0.0f;    // the player forced to sneak
	else if (what == "sneaking") v = w.player.sneaking ? 1.0f : 0.0f;  // the player sneaks now
	else if (what == "hudeffecticons")      // the HUD's active-effect icons (those of its effects with an icon)
	{
		v = 0;
		for (auto& e : s.hudEffects())
		{
			auto me = w.game.magicEffects.find(e.effect);
			v += me != w.game.magicEffects.end() && me->second.iconIx >= 0;
		}
		v = fminf(v, 10.0f);
	}
	else if (what == "abilitiesshown") v = (float)w.abilitiesShown.size();   // effects of abilities and worn constant effects
	else if (what == "makepicking") v = s.makePick;                  // spellmaking / enchanting: choosing a skill (1) or attribute (2)
	else if (what == "makeeffects") v = (float)s.makeEffects.size(); // ... the effects put together so far
	else if (what == "footrepeat") v = (float)s.stepRepeats;         // footsteps on the same foot twice in a row
	else if (what == "enemiesnear") v = s.enemiesNear() ? 1.0f : 0.0f;   // rest refused: enemies about
	else if (what == "werewolfkills") v = (float)w.stats.werewolfKills;   // people killed since the change (GetWerewolfKills)
	else if (what == "werewolf") v = w.stats.werewolf ? 1.0f : 0.0f;     // the player is in wolf form
	else if (what == "watervolume") v = s.waterVolume;                // the near-water sound's volume (0..1, OpenMW's rule)
	else if (what == "footeven")    // the last two gaps between footsteps, the shorter as a percent of the longer (100: even)
	{
		float a = s.stepAt[1] - s.stepAt[0], b = s.stepAt[2] - s.stepAt[1];
		v = s.stepAt[0] < 0.0f || a <= 0.0f || b <= 0.0f ? 0.0f : 100.0f * fminf(a, b) / fmaxf(a, b);
	}
	else if (what == "worldslots") v = (float)w.refs.size();          // object slots the world holds (each 832 bytes)
	else if (what == "sneakhidden") v = s.hidden ? 1.0f : 0.0f;       // the HUD's sneak eye shows (sneaking, nobody noticed)
	else if (what == "thirdperson") v = s.thirdPerson ? 1.0f : 0.0f;  // the view is third person
	else if (what == "viewcamz")    // third person or the face preview: the camera's height over the player's feet (else 0)
	{
		RenderCamera c = {};
		c.pos[2] = w.player.feet[2];
		s.viewCamera(c);
		v = c.pos[2] - w.player.feet[2];
	}
	else if (what == "unaimable")
	{
		// loaded people (and bodies) within 2000 units the crosshair can't find where they stand
		v = 0;
		for (int i : w.loadedActors)
		{
			const Ref& r = w.refs[i];
			if (r.visible() && (r.recType == REC_NPC_ || r.dead) && w.distanceToPlayer(i) < 2000.0f && !actorAimable(w, i))
			{
				logf("check: %s can't be aimed at (at %.0f %.0f %.0f)", r.id.c_str(), r.pos[0], r.pos[1], r.pos[2]);
				v++;
			}
		}
	}
	else if (what == "brewedvalue") { const Object* o = w.game.object(s.lastBrewed); if (!o) return false; v = (float)o->value; }
	else if (what == "made") { v = 0; for (auto& id : w.madeItems) v += carried(w, id) >= 0; }
	else if (what == "quizbottom") v = s.quizBottom;      // the class quiz: the bottom of its focused answer
	else if (what == "previewspin") v = s.previewSpin * 57.29578f;   // the race menu's face turned by the C-stick (degrees)
	else if (what == "topmodal") v = (float)s.topModal.kind;   // the top screen's box: 0 none, 1 a picture, 2 the vitals
	else if (what == "topmodalart") v = (float)s.topModal.art.w;   // its picture's width (0: none)
	else if (what == "magicshown") v = (float)s.magicShown;       // the Magic menu's rows under its school filter
	else if (what == "magicicons") v = (float)s.magicIcons;       // ... those with an effect icon
	else if (what == "birthicons") v = (float)s.birthIcons;       // the birthsign menu's effect rows with an icon
	else if (what == "notesright") v = s.notesRight;      // the right edge of the notices on the top screen
	else if (what == "messagesidebar") v = s.messageSidebar;   // a message box's buttons are in the right column
	else if (what == "shimmerframes") v = (float)rendererCausticFrames();   // enchanted items' shimmer frames read
	else if (what == "iconmisses") { extern int g_uiIconMisses; v = (float)g_uiIconMisses; }   // icons not drawn, page not loaded
	else if (what == "linearfree") v = (float)(linearSpaceFree() / 1024);   // KB of linear memory free
	else if (what == "spritesasked" || what == "spritesdrawn")   // the last view's particle sprites: wanted / drawn
	{
		int asked, drawn;
		rendererSpriteCounts(asked, drawn);
		v = (float)(what == "spritesasked" ? asked : drawn);
	}
	else if (what == "vfxlive") v = (float)s.vfx.size();       // spell visuals playing
	else if (what == "spriteswanted") v = rendererSpritesWantedLast();   // every visual's particles, before the share
	else if (what == "vfxpieces")                                // spell visuals' pieces loaded
	{
		v = 0;
		for (auto& kv : s.vm.pieces)
			for (auto& f : w.game.firstPerson.vfx)
				if (kv.second && f.second == kv.first)
				{
					v += 1;
					break;
				}
	}
	else if (what == "vfxsprited") v = (float)s.vfxSpritedLast;   // visuals that drew particle sprites last frame
	else if (what == "spritebudget") v = (float)g_profile.spriteBudget;   // this frame's, as the frame's work set it
	else if (what == "waterframes") v = (float)rendererWaterFrameCount();  // the water surface's frames loaded
	else if (what == "scrollbatches")
	{
		// loaded batches whose texture slides (a waterfall's NiUVController), not counting the water surface
		v = 0;
		for (LoadedCell* l : w.loaded)
			for (auto& b : l->cell.batches)
				v += (b.scrollU != 0.0f || b.scrollV != 0.0f) && !(b.flags & BATCH_SCROLL) ? 1 : 0;
	}
	// The heap (Old 3DS soak runs): KB in use now, the most used so far, the biggest block still to be had; zlib streams
	// left open; whether the out-of-memory reserve is spent (1) or held (0)
	else if (what == "heapused") v = (float)(mallinfo().uordblks / 1024);
	else if (what == "heappeak") { heapNote(heapInUse()); v = (float)(heapHighWater() / 1024); }
	else if (what == "heapblock") v = (float)(heapLargestBlock() / 1024);
	// ... KB free in holes under 256 KB (the PC memory checker's heap; 0 elsewhere): the heap cut up
	else if (what == "heapscattered")
	{
		size_t kb[4] = {};
		v = heapHoles(kb) ? (float)(kb[0] + kb[1]) : 0.0f;
	}
	else if (what == "zopen") v = (float)zOpenCount();
	else if (what == "cardslices") v = (float)cardSlicesSteps();   // the card files' write-once rule: 0 when it holds
	else if (what == "memorylow") v = memoryReserveHeld() ? 0.0f : 1.0f;
	else if (what == "uiintact") v = uiThemeIntact() ? 1.0f : 0.0f;  // the UI's font and frames still as loaded
	else if (what == "groundgaps") { extern int g_groundGaps; v = (float)g_groundGaps; }   // cracks in the last SHOT (native only, -1 on the 3DS)
	else if (what == "missingtex")
	{
		// textures of the player's cell that aren't there (drawn plain, in their vertex colours)
		v = 0;
		if (w.current >= 0 && w.cells[w.current].live)
		{
			const Cell& c = w.cells[w.current].live->cell;
			for (size_t i = 0; i < c.textures.size(); i++)
				v += !c.textures[i] && !c.textureNames[i].empty();
		}
	}
	else if (what == "waterscroll") v = rendererWaterScroll();       // water's slide, texture repeats a second
	else if (what == "controls") v = w.controlsEnabled ? 1.0f : 0.0f;   // DisablePlayerControls / EnablePlayerControls
	else if (what == "jumping") v = w.jumpingEnabled ? 1.0f : 0.0f;     // DisablePlayerJumping / EnablePlayerJumping
	else if (what == "menuopen") v = s.menuOpen() ? 1.0f : 0.0f;       // a screen, a message or a talk holds the world still
	else if (what == "loadscreens") v = (float)s.loadScreens;          // trips through the loading screen so far
	else if (what == "restsleep") v = s.restSleep ? 1.0f : 0.0f;       // the rest menu sleeps (else only waits: illegal here)
	else if (what == "skillsum") { v = 0; for (int k = 0; k < 27; k++) v += st.skills[k]; }   // all 27 skills (jail time)
	else if (what == "messagetop") v = s.messageTop;      // its first text line shown
	else if (what == "messageleft") v = s.messageLeft;    // its text lines below what is shown
	else if (what == "maplocal") v = s.mapLocal;          // the map screen shows the local map
	else if (what == "mapzoom") v = s.mapZoomIdx;         // its zoom step
	else if (what == "mapcx") v = s.mapCX;                // the map view's centre (world map: map pixels, local: picture pixels)
	else if (what == "mapcy") v = s.mapCY;
	else if (what == "mapfogseen")                         // the squares of this interior's local map out of the fog
	{
		v = 0;
		auto f = w.current >= 0 ? w.mapFog.find(lower(w.cells[w.current].name)) : w.mapFog.end();
		if (f != w.mapFog.end())
			for (u8 level : f->second)
				v += level < 8;
	}
	else if (what == "journalshown") v = (float)s.journalShown.size();   // the journal's entries in its questline and search
	else if (what == "journalpage") v = s.journalPage;    // the page of them shown (0: the newest)
	else if (what == "bookpages") v = (float)s.bookPages.size();   // the open book's pages
	else if (what == "bookspread") v = s.bookSpread;    // its two pages shown (0: pages 1 and 2)
	else if (what == "journalcat") v = s.journalCat;      // the questline shown (0: all)
	else if (what == "madespells") { v = 0; for (auto& id : w.madeSpells) for (auto& sp : st.spells) v += lower(sp) == lower(id); }
	else if (what == "effects") v = (float)w.effects.size();
	else if (what == "brewed") { v = 0; for (auto& id : w.brewed) v += std::max(0, w.itemCount(id)); }
	else if (what == "hour") v = fmodf(w.gameHour, 24.0f);     // the hour of the day (gameHour counts on past 24)
	else if (what == "flying") v = w.player.flying ? 1.0f : 0.0f;
	else if (what == "swings") v = (float)s.swingCount;       // blows the player let go
	else if (what == "casts") v = (float)s.castCount;         // spells the player cast
	else if (what == "vmaction") v = (float)s.vm.action;
	else if (what == "hudwolfhand") v = s.hudHandArt() ? 1.0f : 0.0f;   // the weapon box shows the werewolf's claw
	else if (what == "spells") v = (float)st.spells.size();
	else return false;
	return true;
}
