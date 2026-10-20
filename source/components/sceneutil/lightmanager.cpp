
static const float kLightBounds = 1.65f;     // OpenMW's light bounds multiplier: a light reaches this x its radius
// Light on people: the lamps and the torches near each actor, and the player's carried light (OpenMW's LightManager)
#include <algorithm>
#include <array>
#include <functional>
#include <sys/stat.h>
#include <tex3ds.h>
#include <citro2d.h>
#include "mwbase/session.hpp"
#include "components/debug/profile.hpp"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <citro3d.h>
#include "mwrender/renderhelpers.hpp"

void Session::updateLights()
{
	{
		// A carried light lights the way (the equipped one, held at the hand)
		float radius = 0.0f, rgb[3] = { 0, 0, 0 };
		for (auto& it : w.inventory)
			if (it.equipped)
				if (const Object* o = w.game.object(it))
					if (o->recType == REC_LIGH && o->lightRadius > 0)
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
	// Every fourth frame: every lamp against every person each frame was a few ms of an Old 3DS's, for a tint that eases
	if (w.frame % 4 == 0 || !cadenceOn(CAD_LIGHTS))
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
			if (const Object* o = w.game.carriedLight(r.obj ? r.obj : w.game.object(r.id)))
			{
				Lamp L = { { r.pos[0], r.pos[1], r.pos[2] + 100.0f }, (float)o->lightRadius,
					{ o->lightColor[0] / 255.0f, o->lightColor[1] / 255.0f, o->lightColor[2] / 255.0f }, i };
				lamps.push_back(L);
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
}

void rendererSetNightEye(float add)
{
	g_gpu.ambientAdd = add;
}

void rendererSetGamma(float gamma)
{
	g_gpu.gammaOn = g_gpu.bGamma >= 0 && gamma > 0.0f && fabsf(gamma - 1.0f) > 0.001f;
	g_gpu.invGamma = g_gpu.gammaOn ? 1.0f / gamma : 1.0f;
}

float rendererCurve(float v)
{
	return g_gpu.gammaOn && v > 0.0f ? powf(v, g_gpu.invGamma) : v;
}

void rendererSetCarriedLight(const float pos[3], float radius, const float rgb[3])
{
	memcpy(g_gpu.light, pos, sizeof(g_gpu.light));
	g_gpu.lightRadius = radius;
	memcpy(g_gpu.lightRgb, rgb, sizeof(g_gpu.lightRgb));
}

// One light in this view's eye space: position (w = 1 / radius squared) and colour (off: no colour)
static void putLight(int uPos, int uColor, const C3D_Mtx* view, const float pos[3], float radius, const float rgb[3])
{
	if (!view || radius <= 0.0f)
	{
		C3D_FVUnifSet(GPU_VERTEX_SHADER, uColor, 0, 0, 0, 0);
		C3D_FVUnifSet(GPU_VERTEX_SHADER, uPos, 0, 0, 0, 0);
		return;
	}
	float e[3];
	for (int r = 0; r < 3; r++)
		e[r] = view->r[r].x * pos[0] + view->r[r].y * pos[1] + view->r[r].z * pos[2] + view->r[r].w;
	C3D_FVUnifSet(GPU_VERTEX_SHADER, uPos, e[0], e[1], e[2], 1.0f / (radius * radius));
	C3D_FVUnifSet(GPU_VERTEX_SHADER, uColor, rgb[0], rgb[1], rgb[2], 0.0f);
}

// The carried light in this view's eye space (none: no view). Per pixel (first person) it is no
// vertex work: the shaders put how far out its light reaches into the fog's texture coordinate (torchVec: the eye-space
// depth over twice its radius, the torch being at the eye), the light ramp's rows hold its falloff in its colour, and the
// torch stage adds texture x that before the fog. Depth rather than distance: at the screen's sides it reaches a little
// further. Per vertex otherwise: lightColor x (1 - distance^2 / radius^2) at each vertex (shaders/cell.v.pica lightOn)
void setLight(const C3D_Mtx* view)
{
	if (g_gpu.uLightColor < 0 || g_gpu.uLightPos < 0)
		return;
	bool lit = view && g_gpu.lightRadius > 0.0f && (g_gpu.lightRgb[0] > 0.0f || g_gpu.lightRgb[1] > 0.0f || g_gpu.lightRgb[2] > 0.0f);
	// (per pixel only with the torch at the eye: first person; a third-person or held camera lights per vertex)
	float d2 = 0.0f;
	for (int k = 0; k < 3; k++)
		d2 += (g_gpu.light[k] - g_gpu.eye[k]) * (g_gpu.light[k] - g_gpu.eye[k]);
	bool pixel = lit && g_gpu.uTorchVec >= 0 && d2 < kTorchAtEye * kTorchAtEye;
	putLight(g_gpu.uLightPos, g_gpu.uLightColor, pixel ? nullptr : view, g_gpu.light, g_gpu.lightRadius, g_gpu.lightRgb);
	if (g_gpu.uTorchVec >= 0)
	{
		if (pixel)
			C3D_FVUnifSet(GPU_VERTEX_SHADER, g_gpu.uTorchVec, 0.0f, 0.0f, -1.0f / (kTorchReach * g_gpu.lightRadius), 0.0f);
		else
			C3D_FVUnifSet(GPU_VERTEX_SHADER, g_gpu.uTorchVec, 0.0f, 0.0f, 0.0f, 1.0f);
	}
	if (pixel)
	{
		updateLightRamp();
		C3D_TexBind(1, &g_gpu.fogRamp);
	}
	if (view)
		g_torchPerPixel = pixel ? 1 : lit ? 0 : -1;
	if (pixel != g_gpu.torchStageOn)
	{
		g_gpu.torchStageOn = pixel;
		invalidateState();                  // (setBatchState writes the torch stage, or clears it)
	}
	// (no light carried, or lit per pixel: its five instructions a vertex are skipped)
	if (g_gpu.bLightOn >= 0)
		C3D_BoolUnifSet(GPU_VERTEX_SHADER, g_gpu.bLightOn, lit && !pixel);
	// Night Eye on the world's draws (none on the sky's: OpenMW adds it to the ambient light, which the sky doesn't take)
	if (g_gpu.uAmbient >= 0)
	{
		float a = view ? g_gpu.ambientAdd : 0.0f;
		C3D_FVUnifSet(GPU_VERTEX_SHADER, g_gpu.uAmbient, a, a, a, g_gpu.invGamma);
	}
	g_gpu.lightViewOn = view != nullptr;
	if (view)
		g_gpu.lightView = *view;
}

void rendererSetDaylight(const float land[3], const float sky[3], const float fog[3], bool outdoors)
{
	memcpy(g_gpu.land, land, sizeof(g_gpu.land));
	memcpy(g_gpu.skyTint, sky, sizeof(g_gpu.skyTint));
	g_gpu.dayFog = 0xFF000000 | ((u32)(fog[2] * 255) << 16) | ((u32)(fog[1] * 255) << 8) | (u32)(fog[0] * 255);
	g_gpu.daylight = outdoors;
}

// A person's or creature's tint: its meshes carry the light where it was placed (Actor::tint, baked in when they load);
// outdoors the hour darkens the sun's and the sky's share of it, not the lamps' (Actor::lamp), channel by channel
static const float kPieceLight = 0.63f;    // a library's even light against a third-person piece's (about 0.5 / 0.8)
void actorTint(const Actor& a, float out[3])
{
	const float* t = worldTint();
	for (int k = 0; k < 3; k++)
	{
		// (and the torch it holds out, on itself: Actor::heldLight, in the tint's units, light / 0.5 x 64)
		// (and the lamps near where it stands now in place of those where it was placed: Actor::lampNow)
		float all = a.tint[k], lamp = a.lamp[k] < a.tint[k] ? a.lamp[k] : a.tint[k], held = a.heldLight[k] * 128.0f;
		float now = a.lampSplit ? a.lampNow[k] : lamp;
		out[k] = all > 0.0f ? (t[k] * (all - lamp) + now + held) / all : t[k] + (now + held) / 64.0f;
		// Dressed again from the third-person pieces (npcDress): their colours lack the placement's light a library's take
		// as it loads, and are lit as the player's arms are (ambient 0.55 and a sun), not a library's even 0.5
		if (a.dressed)
			out[k] *= (all > 0.0f ? all / 64.0f : 1.0f) * kPieceLight;
	}
}

void rendererSetSunSide(const float side[4])
{
	memcpy(g_gpu.sideNow, side, sizeof(g_gpu.sideNow));
}

// The batch's lamp light (shaders/cell.v.pica lamp): its colour x (1 - the hour's tint) / 16383, so tint x colour + this x the
// vertex's brightness keeps the lamps' share whole; nothing indoors or on a batch without lamp light. On a side-lit
// batch the low sun's light (sideLight: its colour, w 2 x direction / 255) and lamp.w -direction: shaders/cell.v.pica turns the
// colours' alpha into facing x direction with them
void setLamp(const CellBatch& b)
{
	if (g_gpu.uLamp < 0)
		return;
	bool side = b.side && g_gpu.daylight && g_gpu.sideNow[3] != 0.0f;
	float v[4] = { 0.0f, 0.0f, 0.0f, side ? -g_gpu.sideNow[3] : 0.0f };
	if (g_gpu.daylight && (b.lamp[0] > 0.0f || b.lamp[1] > 0.0f || b.lamp[2] > 0.0f))
		for (int k = 0; k < 3; k++)
			v[k] = b.lamp[k] * (1.0f - g_gpu.land[k]) * (1.0f / 16383.0f);
	g_lampLitBatches += v[0] > 0.0f || v[1] > 0.0f || v[2] > 0.0f;
	float sd[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	if (side)
		sd[0] = g_gpu.sideNow[0], sd[1] = g_gpu.sideNow[1], sd[2] = g_gpu.sideNow[2], sd[3] = 2.0f * g_gpu.sideNow[3] / 255.0f;
	if (g_gpu.lampValid && memcmp(v, g_gpu.lampSet, sizeof(v)) == 0 && memcmp(sd, g_gpu.sideSet, sizeof(sd)) == 0)
		return;
	C3D_FVUnifSet(GPU_VERTEX_SHADER, g_gpu.uLamp, v[0], v[1], v[2], v[3]);
	if (g_gpu.uSide >= 0)
		C3D_FVUnifSet(GPU_VERTEX_SHADER, g_gpu.uSide, sd[0], sd[1], sd[2], sd[3]);
	memcpy(g_gpu.lampSet, v, sizeof(v));
	memcpy(g_gpu.sideSet, sd, sizeof(sd));
	g_gpu.lampValid = true;
}
