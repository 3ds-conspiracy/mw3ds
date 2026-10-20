
static const float kLightBounds = 1.65f;     // OpenMW's light bounds multiplier: a light reaches this x its radius
// Light on people: the lamps and the torches near each actor, and the player's carried light (OpenMW's LightManager)
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
#include <malloc.h>

void Session::updateLights()
{
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
}
