// Alchemy: the screen a mortar and pestle opens, and making potions (Morrowind's rules as OpenMW has them)
#include <cmath>
#include <functional>

#include "components/debug/debuglog.hpp"
#include "mwgui/windowmanagerimp.hpp"
#include "mwbase/session.hpp"
#include "mwmechanics/alchemyhelpers.hpp"

std::string Session::effectLabel(int effect, int skill, int attribute)
{
	auto it = w.game.magicEffects.find(effect);
	std::string name = it != w.game.magicEffects.end() ? it->second.name : "Effect " + std::to_string(effect);
	// "Fortify Attribute" + Strength -> "Fortify Strength"
	size_t p;
	if (attribute >= 0 && (p = name.find("Attribute")) != std::string::npos)
		name.replace(p, 9, attrName(w, attribute));
	else if (skill >= 0 && (p = name.find("Skill")) != std::string::npos)
		name.replace(p, 5, skillName(w, skill));
	return name;
}

// The same effect on two ingredients: the id, and the skill or attribute only for effects that have one
// (the game's data leaves other values in those fields, 0 on one ingredient and -1 on another: comparing
// them kept ingredients that share an effect from making a potion)
const World* s_alchemyWorld = nullptr;
bool sameEffect(const Object::IngredientEffect& a, const Object::IngredientEffect& b)
{
	if (a.effect != b.effect)
		return false;
	int flags = 0;
	if (s_alchemyWorld)
	{
		auto it = s_alchemyWorld->game.magicEffects.find(a.effect);
		if (it != s_alchemyWorld->game.magicEffects.end())
			flags = it->second.flags;
	}
	return (!(flags & 0x1) || a.skill == b.skill) && (!(flags & 0x2) || a.attribute == b.attribute);
}

// Effects two or more of the chosen ingredients share (what a potion of them does)
std::vector<Object::IngredientEffect> commonEffects(const World& w, const std::vector<std::string>& ids)
{
	s_alchemyWorld = &w;
	std::vector<Object::IngredientEffect> out;
	for (size_t a = 0; a < ids.size(); a++)
	{
		const Object* oa = w.game.object(ids[a]);
		if (!oa)
			continue;
		for (auto& e : oa->ingredient)
		{
			bool shared = false, have = false;
			for (size_t b = 0; b < ids.size() && !shared; b++)
				if (b != a)
					if (const Object* ob = w.game.object(ids[b]))
						for (auto& f : ob->ingredient)
							shared |= sameEffect(e, f);
			for (auto& f : out)
				have |= sameEffect(e, f);
			if (shared && !have)
				out.push_back(e);
		}
	}
	return out;
}

// x = (Alchemy + Intelligence / 10 + Luck / 10) x mortar quality x fPotionStrengthMult; each shared
// effect gets magnitude x / fPotionT1MagMult / base cost and duration x / fPotionT1DurMult / base cost,
// raised by the retort (the alembic lowers harmful ones) and the calcinator. Fails when a d100 beats
// the alchemy factor; the ingredients are used either way
bool Session::brewPotion()
{
	lastBrewed.clear();
	float q[4];
	bestApparatus(w, q);
	if (q[0] <= 0.0f)
	{
		notify("You need a mortar and pestle.");
		return false;
	}
	// only what is carried goes in (the screen offers nothing else; a script or a test could name more)
	std::vector<std::string> have;
	for (auto& id : alchemySlots)
		if (w.itemCount(id) > 0)
			have.push_back(id);
	alchemySlots = have;
	if (alchemySlots.size() < 2)
	{
		logf("alchemy: fewer than two ingredients carried");
		return false;
	}
	std::vector<Object::IngredientEffect> shared = commonEffects(w, alchemySlots);
	std::vector<std::string> used = alchemySlots;
	float weight = 0.0f;
	for (auto& id : used)
	{
		if (const Object* o = w.game.object(id))
			weight += o->weight;
		w.removeItem(id, 1);
	}
	weight /= used.size();
	// Those still in the pack stay chosen for the next one
	alchemySlots.clear();
	for (auto& id : used)
		if (w.itemCount(id) > 0)
			alchemySlots.push_back(id);
	const PlayerStats& s = w.stats;
	float factor = s.skills[16] + 0.1f * s.attributes[ATTR_INTELLIGENCE] + 0.1f * s.attributes[ATTR_LUCK];
	if (shared.empty() || factor < rand() % 100)
	{
		playSound(-1, "potion fail");
		notify(w.game.gmst("snotifymessage8", "Your potion failed."));
		logf("alchemy: failed (%zu shared effects, factor %.0f)", shared.size(), factor);
		return false;
	}
	float x = factor * q[0] * w.game.gmstf("fpotionstrengthmult", 0.5f);
	Object potion;
	potion.type = "ALCH";
	potion.weight = weight;
	potion.value = (int)(x * w.game.gmstf("ialchemymod", 2.0f));
	for (auto& e : shared)
	{
		auto md = w.game.magicEffects.find(e.effect);
		float cost = md != w.game.magicEffects.end() ? fmaxf(0.1f, md->second.cost) : 1.0f;
		int flags = md != w.game.magicEffects.end() ? md->second.flags : 0;
		bool harmful = flags & 0x10;
		float mag = (flags & 0x8) ? 1.0f : x / w.game.gmstf("fpotiont1magmult", 1.5f) / cost;
		float dur = (flags & 0x4) ? 1.0f : x / w.game.gmstf("fpotiont1durmult", 0.5f) / cost;
		// Retort (helpful) / alembic (harmful) and calcinator
		float tool = harmful ? q[1] : q[3], calc = q[2];
		// (OpenMW's applyTools: the apparatus' quality by which are there and whether the effect has both a
		// magnitude and a duration; helpful ones add it, harmful ones with a tool divide by it)
		bool both = !(flags & 0x8) && !(flags & 0x4);
		auto applyTools = [&](float& v) {
			bool t = tool > 0.0f, c = calc > 0.0f;
			if (!t && !c)
				return;
			float quality;
			if (t && c)
				quality = harmful ? 2.0f * tool + 3.0f * calc : both ? 2.0f * tool + calc : 2.0f / 3.0f * (tool + calc) + 0.5f;
			else if (t)
				quality = harmful ? 1.0f + tool : both ? tool : tool + 0.5f;
			else
				quality = both ? calc : calc + 0.5f;
			if (!t || !harmful)
				v += quality;
			else
				v /= quality;
		};
		if (!(flags & 0x8))
			applyTools(mag);
		if (!(flags & 0x4))
			applyTools(dur);
		int m = (int)roundf(mag), d = (int)roundf(dur);
		if (m <= 0 || d <= 0)
			continue;
		SpellEffect se = { e.effect, e.skill, e.attribute, m, m, d, 0 };
		potion.effects.push_back(se);
	}
	if (potion.effects.empty())
	{
		notify(w.game.gmst("snotifymessage8", "Your potion failed."));
		return false;
	}
	potion.name = effectLabel(potion.effects[0].effect, potion.effects[0].skill, potion.effects[0].attribute) + " Potion";
	// The look of a bought potion
	for (auto& kv : w.game.objects)
		if (kv.second.type == "ALCH" && kv.second.iconIx >= 0)
		{
			potion.icon = kv.second.icon;
			potion.iconIx = kv.second.iconIx;
			break;
		}
	// The same potion again stacks with the first
	std::string key;
	for (auto& e : potion.effects)
		key += std::to_string(e.effect) + "_" + std::to_string(e.skill) + "_" + std::to_string(e.attribute) + "_"
			+ std::to_string(e.min) + "_" + std::to_string(e.duration) + "_";
	potion.id = "mw3ds_potion_" + std::to_string(std::hash<std::string>()(key) % 1000000);
	if (!w.game.object(potion.id))
	{
		w.game.objects[potion.id] = potion;
		w.brewed.push_back(potion.id);
	}
	w.addItem(potion.id, 1);
	lastBrewed = potion.id;
	useSkill(16, 0);
	playSound(-1, "potion success");
	notify("You made a " + potion.name + ".");
	logf("alchemy: made %s (%s, %zu effects, value %d, factor %.0f)", potion.id.c_str(), potion.name.c_str(),
		potion.effects.size(), potion.value, factor);
	return true;
}
