// EXPECT values: weather, sky, rendering, fonts, textures, spell visuals
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
#include "platform/gamma.hpp"
#include "mwdialogue/dialoguemanagerimp.hpp"
#include "mwbase/session.hpp"
#include "components/debug/debuglog.hpp"
#include "mwsound/soundmanagerimp.hpp"
#include "mwrender/renderingmanager.hpp"

extern const char* g_playerBlock;
extern int g_sharedDrawn, g_worldDraws;
#include <malloc.h>
#include "testdrive/testhelpers.hpp"
#include "testdrive/expecthelpers.hpp"

std::optional<bool> numberOfVisuals(Session& s, const std::string& what, const std::string& arg, float& v)
{
	World& w = s.w;
	// the crosshair name's widest line in pixels (-1: nothing aimed at), and its lines; the bottom prompt's lines
	if (what == "popupwidth" || what == "popuplines" || what == "promptlines")
	{
		if (s.target < 0)
			v = -1.0f;
		else if (what == "promptlines")
			v = (float)s.targetPromptLines(s.target).size();
		else
		{
			std::vector<std::string> lines = s.targetPopupLines(s.target);
			v = what == "popuplines" ? (float)lines.size() : 0.0f;
			if (what == "popupwidth")
				for (auto& l : lines)
					v = fmaxf(v, uiTextWidth(l, 0.55f));
		}
		return true;
	}
	if (what == "deletingsave") { v = s.deletingSave ? 1.0f : 0.0f; return true; }    // the Saves screen asks "Delete ...?"
	if (what == "vfxcount") { v = (float)s.vfx.size(); return true; }      // spell visuals showing now
	// blended draws of the last drawn frame made after a nearer blended one (they go back to front: mwrender/renderingmanager.cpp)
	if (what == "blendoutoforder") { extern int g_blendOutOfOrder; v = (float)g_blendOutOfOrder; return true; }
	if (what == "vfxpiece")           // vfxpiece:<text>: spell visuals showing now whose static's id has that text
	{
		v = 0.0f;
		for (auto& x : s.vfx)
			v += x.what.find(lower(arg)) != std::string::npos ? 1.0f : 0.0f;
		return true;
	}
	// vfxlife:<text>: seconds the first spell visual showing now whose static's id has that text plays in all (-1 none)
	if (what == "vfxlife")
	{
		v = -1.0f;
		for (auto& x : s.vfx)
			if (x.what.find(lower(arg)) != std::string::npos)
			{
				v = x.life;
				break;
			}
		return true;
	}
	// vfxdark:<static id>: that spell visual's meshes drawn blended over the scene (not added to it) in near-black vertex
	// colours: a dark smudge where Morrowind shows a glow (its particles lit by their emissive material)
	// refsprites:<id>[/<bone>]: that creature's sprites shed and alive now (all of them, or from that bone's sprays)
	// vfxfxsprites:<text>: particle records the first spell visual showing now whose static's id has that text carries
	// in the shapes that take its magic effect's particle texture (-1 none showing)
	if (what == "vfxfxsprites")
	{
		v = -1.0f;
		for (auto& x : s.vfx)
			if (x.what.find(lower(arg)) != std::string::npos)
			{
				v = 0.0f;
				if (FpPiece* p = s.vm.piece(w, x.piece))
					for (const ActorMesh& m : p->meshes)
						v += m.fx ? (float)(m.sprites.size() / 9) : 0.0f;
				break;
			}
		return true;
	}
	if (what == "refsprites")
	{
		std::string id = arg, bone;
		size_t slash = arg.find('/');
		if (slash != std::string::npos)
			id = arg.substr(0, slash), bone = lower(spaced(arg.substr(slash + 1)));
		int ri = findRef(s, id);
		v = -1.0f;
		if (ri < 0)
			return true;
		v = 0.0f;
		auto st = s.creatureFx.find(ri);
		FpPiece* p = st == s.creatureFx.end() ? nullptr : s.vm.piece(w, st->second.piece);
		if (!p)
			return true;
		for (size_t i = 0; i + 10 <= st->second.live.size(); i += 10)
			v += bone.empty() || lower(p->meshes[(size_t)st->second.live[i]].boneName) == bone ? 1.0f : 0.0f;
		return true;
	}
	if (what == "vfxdark")
	{
		v = -1.0f;
		auto it = w.game.firstPerson.vfx.find(lower(arg));
		FpPiece* p = it == w.game.firstPerson.vfx.end() ? nullptr : s.vm.piece(w, it->second);
		if (!p)
			return true;
		v = 0.0f;
		for (const ActorMesh& m : p->meshes)
		{
			if (!(m.flags & BATCH_BLEND) || (m.flags & BATCH_ADDITIVE) || m.gpuSkin || !m.numVerts)
				continue;
			if (!m.sprites.empty())
			{
				// (sprites: their colour keys, each weighted by its alpha, as the particles are seen over their life)
				float lit = 0.0f, wsum = 0.0f;
				for (u32 c : m.spriteKeyC)
				{
					float a = ((c >> 24) & 255) / 255.0f + 1e-3f;
					lit += a * fmaxf(fmaxf(c & 255, (c >> 8) & 255), (c >> 16) & 255) / 255.0f;
					wsum += a;
				}
				v += m.spriteKeyC.empty() || lit / wsum >= 0.15f ? 0.0f : 1.0f;
				continue;
			}
			float sum = 0.0f;
			for (u32 i = 0; i < m.numVerts; i++)
			{
				const u8* c = (const u8*)m.verts + i * 24 + 20;
				sum += fmaxf(fmaxf(c[0], c[1]), c[2]) / 255.0f;
			}
			v += sum / m.numVerts < 0.15f ? 1.0f : 0.0f;
		}
		return true;
	}
	// the first spell bolt in flight: its shapes drawn in its magic effect's own particle texture in place of the model's
	// (a Drain bolt flies as Destruction's in vfx_map21, not the fire picture); -1 no bolt
	if (what == "boltfxswap")
	{
		v = -1.0f;
		for (auto& p : s.projectiles)
		{
			FpPiece* bolt = p.spell.empty() || p.bolt.empty() ? nullptr : s.vm.piece(w, p.bolt);
			if (!bolt)
				continue;
			v = 0.0f;
			for (auto& m : bolt->meshes)
				v += s.fxTexture(m, bolt->textures[m.tex], p.boltEffect) != bolt->textures[m.tex] ? 1.0f : 0.0f;
			break;
		}
		return true;
	}
	if (what == "vmammo")             // 1: an arrow or bolt sits in the first-person bow or crossbow
	{
		v = 0.0f;
		if (s.vm.ready)
			for (const ActorMesh& m : s.vm.set.actors[0].meshes)
				if (m.driven)
					v = 1.0f;
		return true;
	}
	// how far above the player's feet the first spell visual stands (-999 with none): a self spell's wraps the body from the feet
	if (what == "vfxheight") { v = s.vfx.empty() ? -999.0f : s.vfx[0].pos[2] - w.player.feet[2]; return true; }
	// how far (flat) the first spell visual stands from the player (-999 with none): a self spell's travels with the caster
	if (what == "vfxdist") { v = s.vfx.empty() ? -999.0f : hypotf(s.vfx[0].pos[0] - w.player.feet[0], s.vfx[0].pos[1] - w.player.feet[1]); return true; }
	// how far (degrees) the first spell visual's facing is off the player's (-999 with none): a cast's faces as the caster does
	if (what == "vfxyaw") { v = s.vfx.empty() ? -999.0f : fabsf(remainderf(s.vfx[0].yaw - w.player.yaw, 6.2831853f)) * 57.29578f; return true; }
	// the largest spell visual's scale: an area burst is drawn at twice its area in feet (OpenMW's explodeSpell)
	if (what == "vfxscale") { v = 0.0f; for (auto& x : s.vfx) v = fmaxf(v, x.scale); return true; }
	if (what == "bolts") { v = (float)s.projectiles.size(); return true; }    // arrows, bolts and spells in flight
	// the first projectile flies as a model (1) or a plain glow (0); -1 with none in flight
	if (what == "boltmodel") { v = s.projectiles.empty() ? -1.0f : s.projectiles[0].bolt.empty() && s.projectiles[0].item.empty() ? 0.0f : 1.0f; return true; }
	if (what == "untextured")           // first-person and body meshes drawn white for lack of a texture
	{
		v = 0;
		for (auto& p : s.vm.pieces)
			v += fpUntextured(p.second);
		for (auto& p : s.body.pieces)
			v += fpUntextured(p.second);
		return true;
	}
	return std::nullopt;
}

std::optional<bool> numberOfWorld(Session& s, const std::string& what, const std::string& arg, float& v)
{
	World& w = s.w;
	if (what == "journalentries") { v = 0; for (auto& l : w.journal) v += l.quest == lower(arg); }   // lines written for a quest
	else if (what == "questfinished") v = w.questFinished.count(lower(arg)) ? 1.0f : 0.0f;
	else if (what == "weathernow") v = (float)w.weatherNow;
	else if (what == "wnext") v = w.weatherNext != w.weatherNow ? (float)w.weatherNext : -1.0f;     // -1: none changing
	else if (what == "wqueued") v = (float)w.weatherQueued;
	else if (what == "wtrans") v = w.weatherNext != w.weatherNow ? w.weatherBlend : 0.0f;
	else if (what == "wcolor")           // <sky|fog|ambient|sun>,<r|g|b>: the weather's colour for this hour
	{
		size_t c = arg.find(',');
		std::string kind = arg.substr(0, c);
		int k = kind == "sky" ? 0 : kind == "fog" ? 1 : kind == "ambient" ? 2 : kind == "sun" ? 3 : -1;
		int ch = c == std::string::npos || c + 1 >= arg.size() ? -1 : arg[c + 1] == 'r' ? 0 : arg[c + 1] == 'g' ? 1 : arg[c + 1] == 'b' ? 2 : -1;
		if (k < 0 || ch < 0)
			return false;
		v = w.weatherColour(k, ch);
	}
	else if (what == "wfogdepth") v = w.weatherFogDepth();
	else if (what == "wfogscale") v = w.weatherFogScale();      // the share of the view drawn in this weather
	else if (what == "framecap") v = (float)s.frameCap;          // the frame rate the console holds to (Old 3DS: 30 or 20)
	else if (what == "colorbits") v = g_profile.rgb565 ? 16.0f : 32.0f;   // the 3D screen's colour buffer
	else if (what == "viewscale") v = s.viewScale;              // the adaptive view distance (1: the whole view)
	else if (what == "fovy")         // the world's field of view, up and down, in degrees (a werewolf's first person: wider)
	{
		extern float g_worldFovY;
		v = g_worldFovY;
	}
	// The last frame's drawing work, both eyes (mwrender/renderingmanager.cpp), and what the console's GPU would take for it
	// cpuest: its CPU time on this build times GPUMODEL's scale (-1 with no scale); fpsest: the frame rate the console
	// would show, the slower of the two in whole screen refreshes (60 a second), at most the frame cap
	else if (what == "gpudraws" || what == "gputris" || what == "gpuest" || what == "cpuest" || what == "fpsest")
	{
		extern u32 g_lastDraws, g_lastTris;
		extern float g_lastCpuMs, g_testCpuScale;
		float gpuModelMs(u32 draws, u32 tris);
		float gpu = gpuModelMs(g_lastDraws, g_lastTris), cpu = g_testCpuScale > 0.0f ? g_lastCpuMs * g_testCpuScale : -1.0f;
		if (what == "gpudraws") v = (float)g_lastDraws;
		else if (what == "gputris") v = (float)g_lastTris;
		else if (what == "gpuest") v = gpu;
		else if (what == "cpuest") v = cpu;
		else
		{
			float refreshes = ceilf(fmaxf(gpu, cpu) / (1000.0f / 60.0f) - 0.001f);
			v = fminf((float)s.frameCap, 60.0f / fmaxf(1.0f, refreshes));
		}
	}
	else if (what == "viewdrawn") v = s.viewDrawn;              // ... as drawn: in steps, held while viewScale saws (Session::viewDrawn)
	else if (what == "hudcaption")    // the cell name under the HUD's minimap is this cell's, 1; else 0
	{
		std::string want = w.game.cellLabel(w.cellName());
		v = s.hudCaption == (want.size() > 26 ? want.substr(0, 25) + "..." : want) ? 1.0f : 0.0f;
	}
	else if (what == "torchtime") v = w.useTorches() ? 1.0f : 0.0f;   // time for torches here (night, a dark interior)
	else if (what == "subtitle") v = w.time < s.subtitleUntil && !s.subtitle.empty() ? 1.0f : 0.0f;   // a spoken line written now
	else if (what == "subtitlehas")     // <text, '_' a space>: the last line spoken (its subtitle) holds it, 1
		v = lower(s.subtitle).find(lower(spaced(arg))) != std::string::npos ? 1.0f : 0.0f;
	else if (what == "lampbatches")     // batches the last view drew with lamp light the night left alone (outdoors)
	{
		extern int g_lampLitBatches;
		v = (float)g_lampLitBatches;
	}
	else if (what == "precip")          // <streaks|flakes|ring|farring|x0>: the rain and ash the last frame drew (drawPrecipitation;
	{                                   // ring: the far rain's band 450 out, farring: the one 900 out)
		// (snow: the flakes that were snow's sheets; dust: the clouds, blizzard: those that were a blizzard's; windx /
		// windy: the way the flakes and clouds blew across the ground)
		extern int g_precipStreaks, g_precipFlakes, g_precipRing, g_precipFarRing, g_precipSnow, g_precipDust, g_precipBlizzard;
		extern float g_precipX0, g_precipWind[2];
		v = arg == "flakes" ? (float)g_precipFlakes : arg == "ring" ? (float)g_precipRing : arg == "farring" ? (float)g_precipFarRing
			: arg == "x0" ? g_precipX0 : arg == "snow" ? (float)g_precipSnow : arg == "dust" ? (float)g_precipDust
			: arg == "blizzard" ? (float)g_precipBlizzard : arg == "windx" ? g_precipWind[0] : arg == "windy" ? g_precipWind[1]
			: (float)g_precipStreaks;
	}
	else if (what == "stormdir")        // <x|y>: where the storm of the weather now blows the player (World::stormDirection)
	{
		float d[2];
		w.stormDirection(w.weatherNow, w.player.feet, d);
		v = arg == "x" ? d[0] : d[1];
	}
	else if (what == "torchpixel")      // the player's torch in the last view: per pixel 1, per vertex 0, none -1
	{
		extern int g_torchPerPixel;
		v = (float)g_torchPerPixel;
	}
	else if (what == "setting")     // <lookspeed|geturn|geaim|inverty|subtitles>: Controls > Look's and Display's, now
	{
		if (arg == "lookspeed") v = (float)g_lookSpeed;
		else if (arg == "geturn") v = (float)g_geTurnSpeed;
		else if (arg == "geaim") v = (float)g_lookSpeed;      // (the look speed: one bar since)
		else if (arg == "inverty") v = g_invertY ? 1.0f : 0.0f;
		else if (arg == "subtitles") v = g_subtitles ? 1.0f : 0.0f;
		else if (arg == "stickzone") v = (float)g_stickZonePct;
		else
			return false;
	}
	else if (what == "savedsetting")    // <key>: settings.txt's <key>= (on 1, off 0, else its number; -1: not there)
	{
		v = -1.0f;
		if (FILE* f = fopen("sdmc:/3ds/mw3ds/settings.txt", "r"))
		{
			char line[64];
			std::string want = arg + "=";
			while (fgets(line, sizeof(line), f))
				if (strncmp(line, want.c_str(), want.size()) == 0)
				{
					const char* val = line + want.size();
					v = strncmp(val, "on", 2) == 0 ? 1.0f : strncmp(val, "off", 3) == 0 ? 0.0f : (float)atof(val);
				}
			fclose(f);
		}
	}
	else if (what == "gamma")       // <value|setting|auto|lift|sensor|saved>: Gamma (Display): the value used now, the bar,
	{                               // Auto gamma on, the light reading's lift, the sensor (0 none, 1 QTM, 2 camera), settings.txt's
		if (arg == "value") v = gammaValue();
		else if (arg == "setting") v = g_gammaPct / 100.0f;
		else if (arg == "auto") v = gammaAutoOn() ? 1.0f : 0.0f;
		else if (arg == "lift") v = gammaLift();
		else if (arg == "sensor") v = (float)gammaSensor();
			else if (arg == "qtmopen") v = gammaQtmOpen() ? 1.0f : 0.0f;
		else if (arg == "saved")
		{
			v = -1.0f;
			if (FILE* f = fopen("sdmc:/3ds/mw3ds/settings.txt", "r"))
			{
				char line[64];
				while (fgets(line, sizeof(line), f))
					if (strncmp(line, "gamma=", 6) == 0)
						v = atoi(line + 6) / 100.0f;
				fclose(f);
			}
		}
		else
			return false;
	}
	else if (what == "cellfog") v = arg == "start" ? w.here().fogStart : w.here().fogEnd;    // <start|end>: this cell's fog distance (0: none)
	else if (what == "keptcells") v = (float)w.cellStates.size();     // cells out of memory with their changes kept
	else if (what == "nearunloaded")  // outdoors: the neighbouring cells nearer the player than the fog's start not loaded
	{
		v = 0;
		if (w.current >= 0 && !w.cells[w.current].interior)
			for (int dy = -1; dy <= 1; dy++)
				for (int dx = -1; dx <= 1; dx++)
				{
					int ci = w.gridCell(w.cells[w.current].gx + dx, w.cells[w.current].gy + dy);
					if (ci < 0 || w.cells[ci].live)
						continue;
					float x0 = w.cells[ci].gx * 8192.0f, y0 = w.cells[ci].gy * 8192.0f;
					float ox = fmaxf(fmaxf(x0 - w.player.feet[0], w.player.feet[0] - (x0 + 8192.0f)), 0.0f);
					float oy = fmaxf(fmaxf(y0 - w.player.feet[1], w.player.feet[1] - (y0 + 8192.0f)), 0.0f);
					if (ox * ox + oy * oy < kExteriorFogStart * kExteriorFogStart)
						v++;
				}
	}
	else if (what == "scriptsinheap")  // scripts with their tree in the heap now (mwscript/scriptstore.cpp)
	{
		v = 0;
		for (const auto& kv : w.game.scripts)
			if (!kv.second.spilled || kv.second.loaded)
				v++;
	}
	else if (what == "talkreads") v = (float)topicsTalkReads();         // the talk so far: reads of the card ...
	else if (what == "talkdecoded") v = (float)topicsTalkDecoded();     // ... and responses decoded (topicstore.cpp)
	else if (what == "topicsinheap")   // topics other than Voice with their responses in the heap now (topicstore.cpp)
	{
		v = 0;
		for (const Topic& t : w.game.topics)
			if (t.type != TOPIC_VOICE && !t.infos.empty())
				v++;
	}
	else if (what == "gridloaded")  // <gx>,<gy>: 1 when that exterior cell's geometry is loaded (-1: no such cell)
	{
		int gx = 0, gy = 0;
		if (sscanf(arg.c_str(), "%d,%d", &gx, &gy) != 2)
			return false;
		int ci = w.gridCell(gx, gy);
		v = ci < 0 ? -1.0f : w.cells[ci].live ? 1.0f : 0.0f;
	}
	else if (what == "savevalid")   // <name>: 1 when test_<name>.sav reads whole (a damaged one is refused)
		v = World::savedCell(("sdmc:/3ds/mw3ds/test_" + arg + ".sav").c_str()).empty() ? 0.0f : 1.0f;
	else if (what == "saveneeds")   // <name>: 1 when test_<name>.sav was made with an add-on the data lacks (refused)
		v = w.saveMissingAddon(("sdmc:/3ds/mw3ds/test_" + arg + ".sav").c_str()).empty() ? 0.0f : 1.0f;
	else if (what == "batchdepthwrite")   // <flags>: 1 if a batch with those BATCH_* flags writes depth (3: blended and tested)
		v = batchWritesDepth((u8)atoi(arg.c_str())) ? 1.0f : 0.0f;
	else if (what == "shotluma")    // <n>: how bright step shot n's top screen is, 0..255 (its pixels' mean; -1: no such shot)
		v = shotLuma(atoi(arg.c_str()));
	else if (what == "shotspread")  // <n>: how far step shot n's top screen's pixels stray from its mean brightness (-1: none)
		v = shotLuma(atoi(arg.c_str()), 1);
	else if (what == "shotdark")    // <n>: the percentage of step shot n's top screen darker than 0.6 x its mean (-1: none)
		v = shotLuma(atoi(arg.c_str()), 2);
	else if (what == "shotblack")   // <n>: the percentage of step shot n's top screen all but black (under 16 of 255)
		v = shotLuma(atoi(arg.c_str()), 3);
	else if (what == "vmalpha")     // how solid the first-person arms are drawn (1; less under Invisibility, Chameleon)
		v = s.vm.set.actors.empty() ? -1.0f : s.vm.set.actors[0].alpha;
	else if (what == "bodyalpha")   // how solid the player's third-person body is drawn (1; less under Invisibility, Chameleon)
		v = s.body.set.actors.empty() ? -1.0f : s.body.set.actors[0].alpha;
	else if (what == "sunelev" || what == "sunsize")   // the sun in the sky now: degrees over the horizon / its picture's
	{                                                  // half width in degrees (-1: not up)
		v = -1.0f;
		if (!g_skyBillboards.empty() && g_skyBillboards[0].tex && g_skyBillboards[0].tex == s.skyTex(w.game.skyBodies.sun))
		{
			const SkyBillboard& sb = g_skyBillboards[0];
			v = what == "sunelev" ? asinf(fmaxf(-1.0f, fminf(1.0f, sb.dir[2]))) * 57.29578f : atanf(sb.size / 2700.0f) * 57.29578f;
		}
	}
	else if (what == "hurtflash") v = s.hurtFlash;     // the red flash of being hurt now, 1 as it starts, 0 gone
	else if (what == "fade") v = s.fade;               // how black the screen is now (travel, rest, scripts' FadeOut)
	else if (what == "uwfog") v = w.underwaterFog();   // how thick the water is to the eye now (Morrowind.ini Underwater Fog)
	else if (what == "fogrange")    // <start|end>: the fog the last frame was drawn with (world units; 0 none)
	{
		// (<horizonstart|horizonend>: the distant land's past the detailed view, 0 when none was drawn)
		float fs, fe;
		if (arg.compare(0, 7, "horizon") == 0)
			rendererHorizonFog(fs, fe);
		else
			rendererFogRange(fs, fe);
		v = arg == "start" || arg == "horizonstart" ? fs : fe;
	}
	else if (what == "arrowvz" || what == "arrowz" || what == "arrowdist")  // the first arrow, bolt or thrown weapon in
	{                                                                    // flight: how fast it climbs / its height /
		v = -9999.0f;                                                    // how far it is from the player (-9999: none)
		for (const Projectile& p : s.projectiles)
			if (p.spell.empty() && !p.item.empty())
			{
				float dx = p.pos[0] - w.player.feet[0], dy = p.pos[1] - w.player.feet[1];
				v = what == "arrowvz" ? p.vel[2] : what == "arrowz" ? p.pos[2] : sqrtf(dx * dx + dy * dy);
				break;
			}
	}
	else if (what == "bloodmodels")  // how many of the three blood splat models hits have thrown so far
		v = (float)((s.bloodModels & 1) + ((s.bloodModels >> 1) & 1) + ((s.bloodModels >> 2) & 1));
	else if (what == "cloudtex")    // <weather 0..9>: the sky's clouds are drawn in that weather's picture now, 1; else 0
	{
		int k = atoi(arg.c_str());
		v = k >= 0 && k < (int)w.game.weatherTypes.size() && g_cloudTex[0] && !w.game.weatherTypes[k].clouds.empty()
			&& g_cloudTex[0] == s.skyTex(w.game.weatherTypes[k].clouds) ? 1.0f : 0.0f;
	}
	else if (what == "moonx" || what == "moonz" || what == "moonalpha")   // <0 Masser | 1 Secunda>: its direction's east
	{                                                                         // (x) or up (z) part, how much of it shows
		int m = atoi(arg.c_str()) ? 1 : 0;
		s.updateSky();
		v = what == "moonx" ? s.moonDir[m][0] : what == "moonz" ? s.moonDir[m][2] : s.moonAlpha[m];
	}
	else if (what == "moonred") v = w.moonRed ? 1.0f : 0.0f;          // TurnMoonRed's state
	else if (what == "moontint")        // <0 Masser | 1 Secunda>,<0 r | 1 g | 2 b>: the colour its picture is drawn in (0..255)
	{
		size_t comma = arg.find(',');
		int m = atoi(arg.c_str()) ? 1 : 0, k = comma == std::string::npos ? 0 : std::max(0, std::min(2, atoi(arg.c_str() + comma + 1)));
		s.updateSky();
		v = (float)s.moonTint[m][k];
	}
	else if (what == "sunside")     // <0..3>: the light from the side now (World::sunSide): r, g, b on a wall facing it, w +1 east
	{
		float side[4];
		w.sunSide(side);
		v = side[std::clamp(atoi(arg.c_str()), 0, 3)];
	}
	else if (what == "sharedmodels")   // the loaded cells' shared models (format 11: Solstheim's pines) and their placements
	{
		v = 0.0f;
		for (LoadedCell* l : w.loaded)
			v += arg == "places" ? (float)l->cell.places.size() : (float)l->cell.models.size();
	}
	else if (what == "shareddrawn")    // shared models' placements the last view drew
		v = (float)g_sharedDrawn;
	else if (what == "worlddraws")     // draws the last view of the world made (each one CPU time on the console)
		v = (float)g_worldDraws;
	else if (what == "totalposes")     // poses made so far by every loaded person and creature (the CPU's animation work)
	{
		v = 0.0f;
		for (LoadedCell* l : w.loaded)
			for (const Actor& a : l->actors.actors)
				v += (float)a.poses;
	}
	else if (what == "detailbatches")  // this cell's batches and shared placements that stop at a distance by their size
	{
		v = 0.0f;
		for (const CellBatch& b : w.here().batches)
			v += b.maxDist > 0.0f ? 1.0f : 0.0f;
		for (const auto& p : w.here().places)
			v += p.maxDist > 0.0f ? 1.0f : 0.0f;
	}
	else if (what == "sidebatches")  // this cell's batches lit from the side by the low sun (their colours' alpha the facing)
	{
		v = 0.0f;
		for (const CellBatch& b : w.here().batches)
			v += b.side ? 1.0f : 0.0f;
	}
	else if (what == "glyph")       // <code point>: the game font's own glyph's advance for it; 0 when the font has none
	{
		int gx, gy;
		float adv = 0.0f;
		v = uiFontGlyph((unsigned)atoi(arg.c_str()), gx, gy, adv) ? adv : 0.0f;
	}
	else if (what == "glyphsame")   // <a>,<b>: 1 when the font draws the two code points with the same glyph; else 0
	{
		float a = 0.0f, b = 0.0f;
		twoNumbers(arg, a, b);
		int ax, ay, bx, by;
		float aa, ba;
		v = uiFontGlyph((unsigned)a, ax, ay, aa) && uiFontGlyph((unsigned)b, bx, by, ba) && ax == bx && ay == by ? 1.0f : 0.0f;
	}
	else if (what == "clouduv")     // <elevation>: how far out the cloud picture is at that height in the sky, as a % of
	{                                 // its reach at the dome's rim (Morrowind's flat dome: little overhead, most low down)
		v = -1.0f;
		float e = (float)atof(arg.c_str()) * 3.14159265f / 180.0f, best = 1e9f, at = 0.0f, rim = 0.0f, cu = 0.0f, cv = 0.0f, top = -2.0f;
		for (const CellBatch& b : w.here().sky)
		{
			if (!(b.flags & BATCH_SCROLL) || !b.verts)
				continue;
			bool packed = b.stride == CELL_VERTEX_SIZE_PACKED;
			auto vert = [&](u32 i, float p[3], float uv[2]) {
				const u8* q = (const u8*)b.verts + i * b.stride;
				if (packed)
				{
					const s16* h = (const s16*)q;
					for (int k = 0; k < 3; k++)
						p[k] = b.posOffset[k] + h[k] * b.posScale[k];
					uv[0] = h[4] * b.uvScale[0], uv[1] = h[5] * b.uvScale[1];
				}
				else
				{
					const float* f = (const float*)q;
					p[0] = f[0], p[1] = f[1], p[2] = f[2], uv[0] = f[3], uv[1] = f[4];
				}
			};
			float p[3], uv[2];
			for (u32 i = 0; i < b.numVerts; i++)     // the zenith's spot in the picture is its middle
			{
				vert(i, p, uv);
				float el = atan2f(p[2], sqrtf(p[0] * p[0] + p[1] * p[1]));
				if (el > top)
					top = el, cu = uv[0], cv = uv[1];
			}
			for (u32 i = 0; i < b.numVerts; i++)
			{
				vert(i, p, uv);
				float el = atan2f(p[2], sqrtf(p[0] * p[0] + p[1] * p[1]));
				float d = sqrtf((uv[0] - cu) * (uv[0] - cu) + (uv[1] - cv) * (uv[1] - cv));
				rim = std::max(rim, d);
				if (fabsf(el - e) < best)
					best = fabsf(el - e), at = d;
			}
			break;
		}
		if (rim > 0.0f)
			v = 100.0f * at / rim;
	}
	else if (what == "bodyshield")  // the player's third-person body shows its shield now, 1; else 0
	{
		v = 0.0f;
		if (!s.body.set.actors.empty())
			for (const ActorMesh& m : s.body.set.actors[0].meshes)
				if (m.hand == 2 && actorMeshShown(s.body.set.actors[0], m))
					v = 1.0f;
	}
	else if (what == "shotlumagain")    // <a>,<b>: shot b's brightness over shot a's (-1: either missing or black)
	{
		float a, b;
		twoNumbers(arg, a, b);
		float la = shotLuma((int)a), lb = shotLuma((int)b);
		v = la > 0.0f && lb >= 0.0f ? lb / la : -1.0f;
	}
	else if (what == "texlow")      // <text>: the lowest point of this cell's batches drawn with a texture whose name has it
	{
		const Cell& c = w.here();
		std::string want = lower(arg);
		bool any = false;
		for (const CellBatch& b : c.batches)
			if (b.tex >= 0 && b.tex < (int)c.textureNames.size() && lower(c.textureNames[b.tex]).find(want) != std::string::npos)
			{
				v = any ? fminf(v, b.bmin[2]) : b.bmin[2];
				any = true;
			}
		if (!any)
			return false;
	}
	else if (what == "texside")     // <text>: the smallest short side, in texels, of this cell's textures whose names have it
	{
		const Cell& c = w.here();
		std::string want = lower(arg);
		bool any = false;
		for (size_t i = 0; i < c.textureNames.size(); i++)
			if (i < c.textures.size() && c.textures[i] && lower(c.textureNames[i]).find(want) != std::string::npos)
			{
				float side = (float)std::min(c.textures[i]->width, c.textures[i]->height);
				v = any ? fminf(v, side) : side;
				any = true;
			}
		if (!any)
			return false;
	}
	else if (what == "texlod")      // <text>: the highest mipmap bias, in levels, of this cell's textures whose names have it
	{
		const Cell& c = w.here();
		std::string want = lower(arg);
		bool any = false;
		for (size_t i = 0; i < c.textureNames.size(); i++)
			if (i < c.textures.size() && c.textures[i] && lower(c.textureNames[i]).find(want) != std::string::npos)
			{
				int b = c.textures[i]->lodBias & 0x1FFF;
				float bias = (b & 0x1000 ? b - 0x2000 : b) / 256.0f;
				v = any ? fmaxf(v, bias) : bias;
				any = true;
			}
		if (!any)
			return false;
	}
	else if (what == "wwind") v = w.windNow;
	else if (what == "wloopis") v = lower(s.weatherSound) == lower(spaced(arg)) ? 1.0f : 0.0f;   // <sound, '_' a space>: the weather's loop now
	else if (what == "loadspeed") v = w.player.loadSpeed;     // the player's speed factor from the load and a storm blowing against them
	else if (what == "wstorm") v = w.weatherStorm() ? 1.0f : 0.0f;
	else if (what == "wprecip") v = w.weatherPrecip() ? 1.0f : 0.0f;
	else if (what == "wtorches") v = w.useTorches() ? 1.0f : 0.0f;
	else if (what == "wsunon") v = w.sunOn() ? 1.0f : 0.0f;
	else if (what == "wskyon") v = w.current >= 0 && w.cells[w.current].underSky() ? 1.0f : 0.0f;    // the sky is drawn under it only (outdoors, Mournhold)
	else if (what == "wsunvis") v = w.sunVisibility();
	else if (what == "wglarefade") v = w.glareFade();
	else if (what == "wnightday") v = (float)w.nightDay();
	else if (what == "wloop") v = s.weatherChannel >= 0 && audioPlaying(s.weatherChannel) ? 1.0f : 0.0f;   // the weather's loop sound
	else if (what == "regionchance")     // <region>,<weather>: the stored chance
	{
		size_t c = arg.rfind(',');
		v = (float)w.regionChance(lower(arg.substr(0, c)), c == std::string::npos ? 0 : atoi(arg.c_str() + c + 1));
	}
	else if (what == "regionweather")    // that region's weather now, -1 if none yet
	{
		auto f = w.forcedWeather.find(lower(arg));
		v = f == w.forcedWeather.end() ? -1.0f : (float)f->second;
	}
	else
		return std::nullopt;
	return true;
}
