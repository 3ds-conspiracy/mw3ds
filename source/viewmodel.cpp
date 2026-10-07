// First-person view model: the player's arms (race skin, sleeves, gauntlets) and the weapon /
// shield they hold, on the first-person skeleton, animated with Morrowind's first-person groups
// and drawn in front of the world. Parts come from data/fp (tools/convert/firstperson.py).
#include "viewmodel.h"

#include <cmath>
#include <cstring>

#include "linear.h"
#include "log.h"
#include "renderer.h"
#include "world.h"
#include "zfile.h"

template <typename T> static bool rd(FILE* f, T& v) { return fread(&v, sizeof(T), 1, f) == 1; }

static int boneIndex(const Skeleton& sk, const char* name)
{
	if (!name[0])
		return -1;
	for (size_t i = 0; i < sk.bones.size(); i++)
		if (strcasecmp(sk.bones[i].name, name) == 0)
			return i;
	return -1;
}

// The Camera bone's orientation at the first Idle1h frame: the view the rig is built around. Later frames' tilt
// of the camera (it rides the animated head) is taken out of the rig, so the arms are seen from the camera
static float s_camIdle[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };

bool ViewModel::load(World& w)
{
	char path[256];
	snprintf(path, sizeof(path), "%s/fp/skeletons.skl", w.dataDir);
	if (!skeletonsLoadInto(path, set.skeletons))
	{
		logf("viewmodel: no %s", path);
		return false;
	}
	set.actors.resize(1);
	Actor& a = set.actors[0];
	a.ref = -1;
	a.skeleton = 0;
	a.pose.assign(set.skeletons[0].bones.size() * 12, 0.0f);
	memset(a.place, 0, sizeof(a.place));
	a.place[0] = a.place[5] = a.place[10] = 1.0f;
	// The reference camera orientation: the pose at the start of the standing idle
	{
		Actor tmp = a;
		int g = actorFindGroup(set.skeletons[0], "Idle1h");
		int cb = boneIndex(set.skeletons[0], "camera");
		if (g >= 0 && cb >= 0)
		{
			tmp.group = g;
			tmp.time = set.skeletons[0].groups[g].start;
			actorAnimate(set, tmp, 0.0f);
			for (int r = 0; r < 3; r++)
				for (int c = 0; c < 3; c++)
					s_camIdle[r * 3 + c] = tmp.pose[cb * 12 + r * 4 + c];
		}
	}
	ready = true;
	logf("viewmodel: skeleton %d bones, %d groups", (int)set.skeletons[0].bones.size(), (int)set.skeletons[0].groups.size());
	return true;
}

void ViewModel::free(World& w)
{
	for (auto& p : pieces)
		fpFreePiece(w, p.second);
	pieces.clear();
	failedAt.clear();
	if (!set.actors.empty())
		set.actors[0].meshes.clear();
	textures.clear();
	builtFor.clear();
}

FpPiece* ViewModel::piece(World& w, const std::string& name)
{
	if (name.empty() || !ready)
		return nullptr;
	auto it = pieces.find(name);
	if (it != pieces.end())
		return it->second;
	// One that ran out of memory is tried again every few seconds, not given up on (a dagger that never
	// came back until the next load)
	auto bad = failedAt.find(name);
	u32 now = (u32)osGetTime();
	if (bad != failedAt.end() && now - bad->second < 3000)
		return nullptr;
	bool missing = false;
	FpPiece* p = fpLoadPiece(w, set.skeletons[0], name, &missing);
	if (p || missing)
		pieces[name] = p;
	else
		failedAt[name] = now;
	return p;
}

void fpFreePiece(World& w, FpPiece* p)
{
	if (!p)
		return;
	for (auto& m : p->meshes)
	{
		lockedLinearFree(m.verts);
		lockedLinearFree(m.indices);
	}
	// Release by name, not by what came back: a texture that failed to load (linear memory was full) is
	// still held in the cache, and without this it stayed failed and the next try came back white
	for (auto& n : p->texNames)
		if (!n.empty())
			w.textures.release(n);
	delete p;
}

void fpRetryTextures(World& w, FpPiece* p)
{
	if (!p)
		return;
	for (size_t i = 0; i < p->textures.size(); i++)
		if (!p->textures[i] && !p->texNames[i].empty())
		{
			p->textures[i] = w.textures.recheck(w.dataDir, p->texNames[i]);
			if (!p->textures[i])
				monitorOnce((std::string("tex:") + p->texNames[i]).c_str(), "missing texture %s (on a first-person / body piece)", p->texNames[i].c_str());
		}
}

int fpUntextured(const FpPiece* p)
{
	int n = 0;
	if (p)
		for (size_t i = 0; i < p->textures.size(); i++)
			n += !p->textures[i] && !p->texNames[i].empty();
	return n;
}

FpPiece* fpLoadPiece(World& w, const Skeleton& sk, const std::string& name, bool* missing)
{
	char path[256];
	snprintf(path, sizeof(path), "%s/fp/%s.fpm", w.dataDir, name.c_str());
	FILE* f = zopen(path);
	if (!f)
	{
		logf("viewmodel: no %s", path);
		if (missing)
			*missing = true;
		return nullptr;
	}
	FpPiece* p = new FpPiece();
	char magic[4];
	u32 ntex = 0, nmesh = 0;
	fread(magic, 4, 1, f);
	rd(f, ntex);
	std::vector<std::string> texFiles(ntex);
	for (u32 i = 0; i < ntex; i++)
	{
		char n[65] = {};
		fread(n, 64, 1, f);
		texFiles[i] = n;
	}
	rd(f, nmesh);
	bool ok = memcmp(magic, "MWP1", 4) == 0;
	for (u32 k = 0; k < nmesh && ok; k++)
	{
		ActorMesh m = {};
		u8 pad;
		char bone[33] = {};
		rd(f, m.tex);
		rd(f, m.flags); rd(f, m.alphaRef); rd(f, m.kind); rd(f, pad);
		m.hasMorph = pad;
		fread(bone, 32, 1, f);
		rd(f, m.numVerts);
		rd(f, m.numIndices);
		m.bone = boneIndex(sk, bone);
		u32 vbytes = m.numVerts * 24, ibytes = m.numIndices * 2;
		m.verts = lockedLinearAlloc(vbytes);
		m.indices = (u16*)lockedLinearAlloc(ibytes);
		if (!m.verts || !m.indices)
		{
			lockedLinearFree(m.verts);
			lockedLinearFree(m.indices);
			ok = false;
			break;
		}
		fread(m.verts, vbytes, 1, f);
		fread(m.indices, ibytes, 1, f);
		if (m.numIndices & 1)
			fseek(f, 2, SEEK_CUR);
		GSPGPU_FlushDataCache(m.indices, ibytes);
		GSPGPU_FlushDataCache(m.verts, vbytes);
		p->bytes += vbytes + ibytes;
		m.src.resize(m.numVerts * 3);
		for (u32 i = 0; i < m.numVerts; i++)
			memcpy(&m.src[i * 3], (u8*)m.verts + i * 24, 12);
		if (m.kind == ACTOR_SKINNED)
		{
			u32 pal = 0;
			rd(f, pal);
			m.palBone.resize(pal);
			m.palMat.resize(pal * 12);
			for (u32 i = 0; i < pal; i++)
			{
				char b[33] = {};
				fread(b, 32, 1, f);
				m.palBone[i] = boneIndex(sk, b);
				if (m.palBone[i] < 0)
					m.palBone[i] = 0;
				fread(&m.palMat[i * 12], 4, 12, f);
			}
			std::vector<u8> inf(m.numVerts * 8);
			fread(inf.data(), 1, inf.size(), f);
			m.infIndex.assign(m.numVerts * 4, 0);
			m.infWeight.assign(m.numVerts * 4, 0.0f);
			m.infCount.assign(m.numVerts, 0);
			for (u32 v = 0; v < m.numVerts; v++)
			{
				int n = 0;
				for (int j = 0; j < 4; j++)
					if (inf[v * 8 + 4 + j])
					{
						m.infIndex[v * 4 + n] = inf[v * 8 + j];
						m.infWeight[v * 4 + n] = inf[v * 8 + 4 + j] * (1.0f / 255.0f);
						n++;
					}
				m.infCount[v] = n;
			}
		}
		if (m.hasMorph)
		{
			// (a bow's string: tools/convert/firstperson.py)
			u32 nm = 0;
			rd(f, nm);
			m.morphs.resize(nm);
			for (auto& mt : m.morphs)
			{
				u32 nk = 0;
				rd(f, nk);
				mt.keys.resize(nk * 2);
				fread(mt.keys.data(), 4, nk * 2, f);
				mt.delta.resize(m.numVerts * 3);
				fread(mt.delta.data(), 4, m.numVerts * 3, f);
			}
			m.talk[0] = m.talk[1] = m.blink[0] = m.blink[1] = 0.0f;
		}
		// Textures: the mesh's own entry in the piece
		if (m.tex >= 0 && m.tex < (int)ntex)
		{
			p->texNames.push_back(texFiles[m.tex]);
			p->textures.push_back(w.textures.acquire(w.dataDir, texFiles[m.tex]));
		}
		else
		{
			p->texNames.push_back("");
			p->textures.push_back(nullptr);
		}
		m.tex = p->meshes.size();
		p->meshes.push_back(m);
	}
	zclose(f);
	if (!ok)
	{
		// A half-loaded piece (a face with parts missing) is not kept: the next try may fit
		logf("viewmodel: %s failed (linear memory?)", name.c_str());
		fpFreePiece(w, p);
		return nullptr;
	}
	return p;
}

// Body part slots the arms use (npc.PART_BONES): hands, wrists, shield, forearms, upper arms, weapon
static const int kSlots[] = { 6, 7, 8, 9, 10, 11, 12, 13, 14, 25 };
static const int CLOT_ROBE = 4, CLOT_SKIRT = 7;

void ViewModel::rebuild(World& w, const std::string& weaponId, const std::string& shieldId)
{
	if (!ready)
		return;
	int female = w.stats.female ? 1 : 0;
	std::string part[32];
	u32 glow[32] = {};                // enchanted items glow
	const FirstPersonDef& fp = w.game.firstPerson;
	std::string race = lower(w.stats.race);
	const std::unordered_map<int, std::string>* skin = nullptr;
	for (int sex : { female, 0 })
		if (!skin && fp.races[sex].count(race))
			skin = &fp.races[sex].at(race);
	if (skin)
		for (auto& s : *skin)
			if (s.first >= 0 && s.first < 32)
				part[s.first] = s.second;
	// Clothes, then armor over them, then skirts and robes (robes cover the arms)
	for (int layer = 0; layer < 3; layer++)
		for (auto& it : w.inventory)
		{
			if (!it.equipped)
				continue;
			const Object* o = w.game.object(it.id);
			if (!o || (o->type != "CLOT" && o->type != "ARMO"))
				continue;
			int l = o->type == "ARMO" ? 1 : (o->subtype == CLOT_ROBE || o->subtype == CLOT_SKIRT) ? 2 : 0;
			if (l != layer || (o->type == "ARMO" && o->subtype == 8))
				continue;
			if (o->type == "CLOT" && o->subtype == CLOT_ROBE)
				for (int s : { 11, 12, 13, 14 })
				{
					part[s].clear();
					glow[s] = 0;
				}
			auto fit = fp.items.find(lower(it.id));
			if (fit == fp.items.end())
				continue;
			for (auto& s : fit->second.slots)
				if (s.first >= 0 && s.first < 32 && s.first != 25 && s.first != 10)
				{
					const std::string& pc = female && !s.second.second.empty() ? s.second.second : s.second.first;
					if (!pc.empty())
					{
						part[s.first] = pc;
						glow[s.first] = w.game.enchantGlow(it.id);
					}
				}
		}
	if (!weaponId.empty())
	{
		auto fit = fp.items.find(lower(weaponId));
		if (fit != fp.items.end() && fit->second.slots.count(25))
		{
			part[25] = fit->second.slots.at(25).first;
			glow[25] = w.game.enchantGlow(weaponId);
		}
	}
	if (!shieldId.empty())
	{
		auto fit = fp.items.find(lower(shieldId));
		if (fit != fp.items.end() && fit->second.slots.count(10))
		{
			part[10] = fit->second.slots.at(10).first;
			glow[10] = w.game.enchantGlow(shieldId);
		}
	}
	std::string key;
	for (int s : kSlots)
		key += part[s] + (glow[s] ? "*|" : "|");
	u32 now = (u32)osGetTime();
	if (key == builtFor && !(incomplete && now >= retryAt))
		return;
	builtFor = key;
	incomplete = false;
	retryAt = now + 3000;
	Actor& a = set.actors[0];
	a.meshes.clear();
	textures.clear();
	for (int s : kSlots)
	{
		FpPiece* p = piece(w, part[s]);
		if (!p)
		{
			incomplete |= failedAt.count(part[s]) > 0;
			continue;
		}
		fpRetryTextures(w, p);
		incomplete |= fpUntextured(p) > 0;
		for (auto& m : p->meshes)
		{
			ActorMesh copy = m;
			copy.tex = textures.size();
			copy.glow = glow[s];
			textures.push_back(p->textures[m.tex]);
			a.meshes.push_back(copy);
		}
	}
	a.deformed = false;
	logf("viewmodel: %d meshes (%s)", (int)a.meshes.size(), key.c_str());
}

float ViewModel::groupLength(const char* groupName) const
{
	if (!ready)
		return 0.0f;
	int g = actorFindGroup(set.skeletons[0], groupName);
	return g < 0 ? 0.0f : set.skeletons[0].groups[g].stop - set.skeletons[0].groups[g].start;
}

void ViewModel::play(VmAction act, const char* groupName, bool hold)
{
	(void)hold;
	if (!ready)
		return;
	Actor& a = set.actors[0];
	// the same group held at its end (a second cast of the same kind) plays again from its start, not "already playing"
	bool again = actorFindGroup(set.skeletons[0], groupName) == a.group && finished();
	if (actorPlay(set, a, groupName, ANIM_HOLD))
	{
		action = act;
		if (again)
			a.time = set.skeletons[0].groups[a.group].start;
	}
	else if (act == VM_UNEQUIP)
		action = VM_NONE;
	else
		action = VM_IDLE;
	charge = 0.0f;
	followTo.clear();
}

void ViewModel::playFollow(const char* groupName, float strength)
{
	play(VM_FOLLOW, groupName);
	if (!ready || action != VM_FOLLOW)
		return;
	// Under 0.33 the small one, under 0.66 the medium one, else the large one the group itself runs on into
	// (data without the "FM" / "FS" groups: always the large one)
	std::string next = std::string(groupName) + (strength < 0.33f ? "S" : "M");
	if (strength < 0.66f && actorFindGroup(set.skeletons[0], next.c_str()) >= 0)
		followTo = next;
}

bool ViewModel::finished() const
{
	const Actor& a = set.actors[0];
	return a.time >= set.skeletons[0].groups[a.group].stop - 1e-4f;
}

// A blow can't be let go before the wind-up's min attack mark (OpenMW plays it out first)
bool ViewModel::windupReached() const
{
	if (!ready || action != VM_WINDUP)
		return true;
	const Actor& a = set.actors[0];
	const AnimGroup& g = set.skeletons[0].groups[a.group];
	return a.time >= fminf(g.loopStart, g.stop - 1e-4f);
}

// A spell takes effect at the cast animation's release mark, not when the button goes down
bool ViewModel::releaseReached() const
{
	if (!ready || action != VM_CAST)
		return true;
	const Actor& a = set.actors[0];
	const AnimGroup& g = set.skeletons[0].groups[a.group];
	return a.time >= fminf(g.loopStart, g.stop - 1e-4f);
}

void ViewModel::update(float dt, bool moving, bool running)
{
	if (!ready)
		return;
	Actor& a = set.actors[0];
	const Skeleton& sk = set.skeletons[0];
	float step = dt;
	switch (action)
	{
	case VM_NONE:
		visible = false;
		return;
	case VM_WINDUP:
	{
		// Up to the min attack mark at normal speed, then held by how far the blow is drawn back
		const AnimGroup& g = sk.groups[a.group];
		step = dt * speed;
		if (a.time + step < g.loopStart)
			break;
		a.time = g.loopStart + (g.loopStop - g.loopStart) * charge;
		step = 0.0f;
		break;
	}
	case VM_IDLE:
	{
		std::string fam = group == "Bow" || group == "Throw" ? "1h" : group;
		std::string name = (moving ? (running ? "Run" : "Walk") : "Idle") + (moving && fam == "Xbow" ? std::string("2c") : fam);
		if (!actorPlay(set, a, name.c_str(), ANIM_LOOP))
			actorPlay(set, a, "Idle1h", ANIM_LOOP);
		break;
	}
	default:
		if (action == VM_FOLLOW)
		{
			step = dt * speed;      // the follow-through goes at the weapon's speed too
			// a weaker blow: at the hit mark, on to its own follow-through
			const AnimGroup& g = sk.groups[a.group];
			if (!followTo.empty() && a.time + step >= g.loopStart && g.loopStart < g.stop)
			{
				actorPlay(set, a, followTo.c_str(), ANIM_HOLD);
				followTo.clear();
				step = 0.0f;
			}
		}
		if (finished())
		{
			action = action == VM_UNEQUIP ? VM_NONE : VM_IDLE;
			if (action == VM_NONE)
			{
				visible = false;
				return;
			}
		}
		break;
	}
	visible = true;
	actorAnimate(set, a, step);
	// The weapon's own animation (a bow's string drawing back and let go) runs on a timeline that starts where the
	// first-person group's equip does: the string is fully back at the "shoot max attack" mark
	a.driveMorph = true;
	a.driveT = a.time;
	int eq = actorFindGroup(sk, (group + "Eq").c_str());
	if (eq >= 0)
		a.driveT -= sk.groups[eq].start;
}

void ViewModel::draw(const float eye[3], float yaw, float pitch, bool deform)
{
	if (!ready || !visible || set.actors[0].meshes.empty())
		return;
	Actor& a = set.actors[0];
	const Skeleton& sk = set.skeletons[0];
	// The first-person skeleton's Camera bone sits at the eye; the rig turns with the view
	static int cam = -2;
	if (cam == -2)
		cam = boneIndex(sk, "camera");
	float cp[3] = { 0, 0, 0 };
	if (cam >= 0)
	{
		cp[0] = a.pose[cam * 12 + 3];
		cp[1] = a.pose[cam * 12 + 7];
		cp[2] = a.pose[cam * 12 + 11];
	}
	C3D_Mtx m;
	Mtx_Identity(&m);
	Mtx_Translate(&m, eye[0], eye[1], eye[2], true);
	Mtx_RotateZ(&m, -yaw, true);
	Mtx_RotateX(&m, pitch, true);
	if (cam >= 0)
	{
		// q = idle camera orientation x (current camera orientation)^-1: turns the rig as the camera tilts
		const float* rc = &a.pose[cam * 12];
		C3D_Mtx q, mq;
		Mtx_Identity(&q);
		for (int r = 0; r < 3; r++)
		{
			float row[3];
			for (int c = 0; c < 3; c++)
				row[c] = s_camIdle[r * 3] * rc[c * 4] + s_camIdle[r * 3 + 1] * rc[c * 4 + 1] + s_camIdle[r * 3 + 2] * rc[c * 4 + 2];
			q.r[r] = FVec4_New(row[0], row[1], row[2], 0.0f);
		}
		Mtx_Multiply(&mq, &m, &q);
		m = mq;
	}
	Mtx_Translate(&m, -cp[0], -cp[1], -cp[2], true);
	float place[12] = { m.r[0].x, m.r[0].y, m.r[0].z, m.r[0].w,
	                    m.r[1].x, m.r[1].y, m.r[1].z, m.r[1].w,
	                    m.r[2].x, m.r[2].y, m.r[2].z, m.r[2].w };
	memcpy(a.place, place, sizeof(place));
	if (deform)
		actorDeform(set, a);
	rendererDrawActor(a, textures, true);
}
