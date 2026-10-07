#include "actors.h"

#include <sys/stat.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "cell.h"
#include "linear.h"
#include "log.h"
#include "zfile.h"

// ---- 3x4 matrices: rows [r00 r01 r02 tx | r10 r11 r12 ty | r20 r21 r22 tz]

static void mul34(const float* a, const float* b, float* out)
{
	float r[12];
	for (int i = 0; i < 3; i++)
	{
		const float* ar = a + i * 4;
		for (int j = 0; j < 3; j++)
			r[i * 4 + j] = ar[0] * b[j] + ar[1] * b[4 + j] + ar[2] * b[8 + j];
		r[i * 4 + 3] = ar[0] * b[3] + ar[1] * b[7] + ar[2] * b[11] + ar[3];
	}
	memcpy(out, r, sizeof(r));
}

static inline void xform(const float* m, const float* v, float* out)
{
	out[0] = m[0] * v[0] + m[1] * v[1] + m[2] * v[2] + m[3];
	out[1] = m[4] * v[0] + m[5] * v[1] + m[6] * v[2] + m[7];
	out[2] = m[8] * v[0] + m[9] * v[1] + m[10] * v[2] + m[11];
}

// ---- loading

template <typename T> static bool rd(FILE* f, T& v) { return fread(&v, sizeof(T), 1, f) == 1; }

static bool readBatchBuffers(FILE* f, ActorMesh& m, ActorSet& set, const u8* tint)
{
	u32 vbytes = m.numVerts * 24, ibytes = m.numIndices * 2;
	m.verts = lockedLinearAlloc(vbytes);
	m.indices = (u16*)lockedLinearAlloc(ibytes);
	if (!m.verts || !m.indices)
		return false;
	fread(m.verts, vbytes, 1, f);
	fread(m.indices, ibytes, 1, f);
	// Library meshes are lit evenly: the placement's tint (x 64) brings the light where it stands
	if (tint)
		for (u32 i = 0; i < m.numVerts; i++)
		{
			u8* c = (u8*)m.verts + i * 24 + 20;
			for (int k = 0; k < 3; k++)
			{
				u32 v = c[k] * tint[k] / 64;
				c[k] = v > 255 ? 255 : v;
			}
		}
	// Out of the CPU's data cache before the GPU reads them (rigid meshes are never re-written)
	GSPGPU_FlushDataCache(m.verts, vbytes);
	GSPGPU_FlushDataCache(m.indices, ibytes);
	if (m.numIndices & 1)
		fseek(f, 2, SEEK_CUR);
	set.bytes += vbytes + ibytes;
	m.src.resize(m.numVerts * 3);
	for (u32 i = 0; i < m.numVerts; i++)
		memcpy(&m.src[i * 3], (u8*)m.verts + i * 24, 12);
	return true;
}

std::vector<Skeleton> g_sharedSkeletons;
static void sampleVec(const std::vector<VecKey>& keys, float t, float* v);

static void readSkeletons(FILE* f, std::vector<Skeleton>& out, u32 count)
{
	out.resize(count);
	for (auto& sk : out)
	{
		u32 n = 0;
		rd(f, n);
		sk.bones.resize(n);
		for (auto& b : sk.bones)
		{
			fread(b.name, 32, 1, f);
			rd(f, b.parent);
			fread(b.rest, 4, 12, f);
		}
		rd(f, n);
		sk.tracks.resize(n);
		sk.trackOfBone.assign(sk.bones.size(), -1);
		for (u32 i = 0; i < n; i++)
		{
			Track& t = sk.tracks[i];
			u32 k = 0;
			rd(f, t.bone);
			rd(f, k); t.rot.resize(k); fread(t.rot.data(), sizeof(RotKey), k, f);
			rd(f, k); t.trans.resize(k); fread(t.trans.data(), sizeof(VecKey), k, f);
			rd(f, k); t.scale.resize(k); fread(t.scale.data(), sizeof(ScaleKey), k, f);
			if (t.bone >= 0 && t.bone < (int)sk.bones.size())
				sk.trackOfBone[t.bone] = i;
		}
		rd(f, n);
		sk.groups.resize(n);
		fread(sk.groups.data(), sizeof(AnimGroup), n, f);
		sk.numIdle = 0;
		while (sk.numIdle < (int)sk.groups.size() && strncmp(sk.groups[sk.numIdle].name, "Idle", 4) == 0)
			sk.numIdle++;
		sk.accumBone = -1;
		for (size_t i = 0; i < sk.bones.size() && sk.accumBone < 0; i++)
			if (strcasecmp(sk.bones[i].name, "bip01") == 0 || strcasecmp(sk.bones[i].name, "root bone") == 0)
				sk.accumBone = i;
		sk.headBone = -1;
		for (size_t i = 0; i < sk.bones.size(); i++)
			if (strcasecmp(sk.bones[i].name, "bip01 head") == 0)
				sk.headBone = i;
		// How far each group's root walks a second (its loop): walk / run cycles are played at the
		// actor's own speed, so feet don't slide
		sk.groupSpeed.assign(sk.groups.size(), 0.0f);
		int at = sk.accumBone >= 0 ? sk.trackOfBone[sk.accumBone] : -1;
		if (at >= 0 && !sk.tracks[at].trans.empty())
			for (size_t g = 0; g < sk.groups.size(); g++)
			{
				const AnimGroup& ag = sk.groups[g];
				float t0 = ag.loopStart < ag.loopStop ? ag.loopStart : ag.start, t1 = ag.loopStart < ag.loopStop ? ag.loopStop : ag.stop;
				if (t1 - t0 < 0.1f)
					continue;
				float a[3], b[3];
				sampleVec(sk.tracks[at].trans, t0, a);
				sampleVec(sk.tracks[at].trans, t1, b);
				sk.groupSpeed[g] = sqrtf((b[0] - a[0]) * (b[0] - a[0]) + (b[1] - a[1]) * (b[1] - a[1])) / (t1 - t0);
			}
	}
}

bool skeletonsLoadInto(const char* path, std::vector<Skeleton>& out)
{
	FILE* f = zopen(path);
	if (!f)
		return false;
	char magic[4];
	u32 count = 0;
	fread(magic, 4, 1, f);
	rd(f, count);
	if (memcmp(magic, "MWS1", 4) == 0)
		readSkeletons(f, out, count);
	zclose(f);
	return !out.empty();
}

static std::string s_skelDir;
static std::vector<bool> s_skelLoaded;
static LightLock s_skelLock;

void skeletonEnsure(int i)
{
	if (s_skelDir.empty() || i < 0 || i >= (int)s_skelLoaded.size())
		return;
	LightLock_Lock(&s_skelLock);
	if (!s_skelLoaded[i])
	{
		std::vector<Skeleton> one;
		char path[256];
		snprintf(path, sizeof(path), "%s/skel_%d.skl", s_skelDir.c_str(), i);
		if (skeletonsLoadInto(path, one))
			g_sharedSkeletons[i] = std::move(one[0]);
		else
			logf("actors: cannot read %s", path);
		s_skelLoaded[i] = true;
	}
	LightLock_Unlock(&s_skelLock);
}

bool skeletonsLoad(const char* path)
{
	// One file per skeleton next to it: only count them now, read each when first needed
	std::string dir = path;
	dir = dir.substr(0, dir.rfind('/'));
	struct stat st;
	int files = 0;
	char one[256];
	while (snprintf(one, sizeof(one), "%s/skel_%d.skl", dir.c_str(), files), stat(one, &st) == 0)
		files++;
	if (files > 0)
	{
		LightLock_Init(&s_skelLock);
		s_skelDir = dir;
		g_sharedSkeletons.assign(files, Skeleton());
		s_skelLoaded.assign(files, false);
		logf("actors: %d shared skeletons (read as needed)", files);
		return true;
	}
	FILE* f = zopen(path);
	if (!f)
		return false;
	char magic[4];
	u32 count = 0;
	fread(magic, 4, 1, f);
	rd(f, count);
	if (memcmp(magic, "MWS1", 4) == 0)
		readSkeletons(f, g_sharedSkeletons, count);
	zclose(f);
	logf("actors: %d shared skeletons", (int)g_sharedSkeletons.size());
	return !g_sharedSkeletons.empty();
}

// One mesh after its texture: render state, buffers, skinning, morphs
static bool readMesh(FILE* f, ActorMesh& m, ActorSet& set, const u8* tint)
{
		rd(f, m.flags); rd(f, m.alphaRef); rd(f, m.kind); rd(f, m.hasMorph);
		rd(f, m.bone);
		rd(f, m.numVerts);
		rd(f, m.numIndices);
		if (!readBatchBuffers(f, m, set, tint))
		{
			logf("actors: out of linear memory");
			return false;
		}
		if (m.kind == ACTOR_SKINNED)
		{
			u32 pal = 0;
			rd(f, pal);
			m.palBone.resize(pal);
			m.palMat.resize(pal * 12);
			for (u32 p = 0; p < pal; p++)
			{
				rd(f, m.palBone[p]);
				fread(&m.palMat[p * 12], 4, 12, f);
			}
			std::vector<u8> inf(m.numVerts * 8);
			fread(inf.data(), 1, inf.size(), f);
			m.infIndex.assign(m.numVerts * 4, 0);
			m.infWeight.assign(m.numVerts * 4, 0.0f);
			m.infCount.assign(m.numVerts, 0);
			for (u32 v = 0; v < m.numVerts; v++)
			{
				int n = 0;
				for (int k = 0; k < 4; k++)
					if (inf[v * 8 + 4 + k])
					{
						m.infIndex[v * 4 + n] = inf[v * 8 + k];
						m.infWeight[v * 4 + n] = inf[v * 8 + 4 + k] * (1.0f / 255.0f);
						n++;
					}
				m.infCount[v] = n;
			}
		}
		if (m.hasMorph)
		{
			fread(m.talk, 4, 2, f);
			fread(m.blink, 4, 2, f);
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
				fread(mt.delta.data(), 4, mt.delta.size(), f);
			}
		}
	return true;
}

// 'MWL1' library file: an NPC / creature record's meshes, textures by name (looked up in the cell's list)
static bool readLibrary(ActorSet& set, Actor& a, const std::string& path, const std::vector<std::string>& texNames,
	const u8* tint, Cell* addTo = nullptr, TextureCache* cache = nullptr, const char* dataDir = nullptr)
{
	FILE* f = zopen(path.c_str());
	if (!f)
	{
		logf("actors: no %s", path.c_str());
		return true;              // drawn without meshes, but the cell still loads
	}
	char magic[4];
	u32 version = 0, count = 0;
	fread(magic, 4, 1, f);
	rd(f, version);
	rd(f, count);
	if (memcmp(magic, "MWL1", 4) != 0)
	{
		zclose(f);
		return true;
	}
	a.meshes.resize(count);
	for (auto& m : a.meshes)
	{
		u16 len = 0;
		rd(f, len);
		std::string name(len, '\0');
		fread(&name[0], 1, len, f);
		m.tex = -1;
		for (size_t k = 0; k < texNames.size() && !name.empty(); k++)
			if (texNames[k] == name)
				m.tex = k;
		// Placed while playing: a texture the cell doesn't have yet joins its list
		if (m.tex < 0 && !name.empty() && addTo && cache)
		{
			m.tex = addTo->textureNames.size();
			addTo->textureNames.push_back(name);
			addTo->textures.push_back(cache->acquire(dataDir, name));
		}
		if (!readMesh(f, m, set, tint))
		{
			zclose(f);
			return false;
		}
	}
	zclose(f);
	return true;
}

int actorsAddFromLibrary(ActorSet& set, Cell& cell, TextureCache& cache, const char* dataDir, const std::string& lib,
	int ref, int skeleton, const float place[12])
{
	if (set.skeletons.empty())
		skeletonEnsure(skeleton);
	set.actors.emplace_back();
	Actor& a = set.actors.back();
	a.ref = ref;
	a.skeleton = skeleton;
	memcpy(a.place, place, sizeof(a.place));
	memset(a.idle, 0, sizeof(a.idle));
	static const u8 even[4] = { 64, 64, 64, 0 };
	if (!readLibrary(set, a, std::string(dataDir) + "/" + lib, cell.textureNames, even, &cell, &cache, dataDir)
		|| a.meshes.empty())
	{
		set.actors.pop_back();
		return -1;
	}
	const Skeleton& sk = actorSkeleton(set, a.skeleton);
	a.pose.assign(sk.bones.size() * 12, 0.0f);
	a.time = sk.groups.empty() ? 0.0f : sk.groups[0].start;
	a.blinkTimer = 1.0f;
	return set.actors.size() - 1;
}

bool actorsLoad(ActorSet& set, const char* path, const char* dataDir, const std::vector<std::string>& texNames)
{
	FILE* f = zopen(path);
	if (!f)
	{
		logf("actors: no %s", path);
		return false;
	}
	char magic[4];
	u32 version = 0, numSkeletons = 0;
	fread(magic, 4, 1, f);
	rd(f, version);
	rd(f, numSkeletons);
	if (memcmp(magic, "MWA1", 4) != 0)
	{
		zclose(f);
		return false;
	}
	readSkeletons(f, set.skeletons, numSkeletons);

	u32 numActors = 0;
	rd(f, numActors);
	set.actors.resize(numActors);
	for (auto& a : set.actors)
	{
		rd(f, a.ref);
		rd(f, a.skeleton);
		if (set.skeletons.empty())
			skeletonEnsure(a.skeleton);
		fread(a.place, 4, 12, f);
		fread(a.idle, 1, 8, f);
		if (version >= 3)
		{
			// The meshes are in the record's library file, shared by every placement
			u8 tint[4];
			fread(tint, 1, 4, f);
			u16 len = 0;
			rd(f, len);
			std::string lib(len, '\0');
			fread(&lib[0], 1, len, f);
			if (!readLibrary(set, a, std::string(dataDir) + "/" + lib, texNames, tint))
			{
				zclose(f);
				return false;
			}
		}
		else
		{
			u32 numMeshes = 0;
			rd(f, numMeshes);
			a.meshes.resize(numMeshes);
			for (auto& m : a.meshes)
			{
				rd(f, m.tex);
				if (!readMesh(f, m, set, nullptr))
				{
					zclose(f);
					return false;
				}
			}
		}
		const Skeleton& sk = actorSkeleton(set, a.skeleton);
		a.pose.assign(sk.bones.size() * 12, 0.0f);
		// Desynchronize idles so actors don't breathe in unison
		a.time = sk.groups.empty() ? 0.0f : sk.groups[0].start + (rand() % 1000) / 1000.0f * (sk.groups[0].stop - sk.groups[0].start);
		a.blinkTimer = 1.0f + (rand() % 3000) / 1000.0f;
	}
	zclose(f);
	logf("actors: %d actors, %d skeletons, %lu KB", (int)set.actors.size(), (int)set.skeletons.size(), set.bytes / 1024);
	return true;
}

void actorsFree(ActorSet& set)
{
	for (auto& a : set.actors)
		for (auto& m : a.meshes)
		{
			lockedLinearFree(m.verts);
			lockedLinearFree(m.indices);
		}
	set = ActorSet();
}

// ---- animation

int actorFindGroup(const Skeleton& sk, const char* name)
{
	for (size_t i = 0; i < sk.groups.size(); i++)
		if (strncmp(sk.groups[i].name, name, sizeof(sk.groups[i].name)) == 0)
			return i;
	return -1;
}

bool actorPlay(ActorSet& set, Actor& a, const char* group, AnimMode mode)
{
	// Swimming: the water's versions of walking, standing, being hit and dying (when the skeleton has them)
	if (a.swimming)
	{
		static const char* kSwim[][2] = { { "WalkForward", "SwimWalkForward" }, { "RunForward", "SwimRunForward" },
			{ "Idle", "IdleSwim" }, { "Hit1", "SwimHit1" }, { "Hit2", "SwimHit2" }, { "Hit3", "SwimHit3" },
			{ "Death1", "SwimDeath" }, { "KnockDown", "SwimKnockDown" }, { "KnockOut", "SwimKnockOut" },
			{ "TurnLeft", "SwimTurnLeft" }, { "TurnRight", "SwimTurnRight" } };
		for (auto& s : kSwim)
			if (strcasecmp(group, s[0]) == 0)
			{
				int sg = actorFindGroup(actorSkeleton(set, a.skeleton), s[1]);
				if (sg >= 0)
					group = s[1];
				break;
			}
	}
	int g = actorFindGroup(actorSkeleton(set, a.skeleton), group);
	if (g < 0)
		return false;
	if (g == a.group && a.mode == mode && mode != ANIM_ONCE)
		return true;                   // already playing
	a.group = g;
	a.mode = mode;
	a.loopsLeft = -1;
	a.time = actorSkeleton(set, a.skeleton).groups[g].start;
	return true;
}

void actorSetPlacement(Actor& a, float x, float y, float z, float yaw)
{
	float sxy = sqrtf(a.place[0] * a.place[0] + a.place[4] * a.place[4] + a.place[8] * a.place[8]);
	float sz = sqrtf(a.place[2] * a.place[2] + a.place[6] * a.place[6] + a.place[10] * a.place[10]);
	float c = cosf(yaw), s = sinf(yaw);
	float m[12] = { c * sxy, s * sxy, 0, x,
	                -s * sxy, c * sxy, 0, y,
	                0, 0, sz, z };
	memcpy(a.place, m, sizeof(m));
}

template <typename K>
static size_t keyBefore(const std::vector<K>& keys, float t)
{
	// last key with time <= t (keys sorted by time)
	size_t lo = 0, hi = keys.size();
	while (hi - lo > 1)
	{
		size_t mid = (lo + hi) / 2;
		if (keys[mid].t <= t) lo = mid; else hi = mid;
	}
	return lo;
}

static void sampleRot(const std::vector<RotKey>& keys, float t, float* q)
{
	size_t i = keyBefore(keys, t);
	const RotKey& a = keys[i];
	if (i + 1 >= keys.size() || t <= a.t)
	{
		q[0] = a.w; q[1] = a.x; q[2] = a.y; q[3] = a.z;
		return;
	}
	const RotKey& b = keys[i + 1];
	float f = (t - a.t) / fmaxf(b.t - a.t, 1e-6f);
	float s = (a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z) < 0.0f ? -1.0f : 1.0f;
	q[0] = a.w + (s * b.w - a.w) * f;
	q[1] = a.x + (s * b.x - a.x) * f;
	q[2] = a.y + (s * b.y - a.y) * f;
	q[3] = a.z + (s * b.z - a.z) * f;
	float len = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
	for (int k = 0; k < 4; k++)
		q[k] /= len;
}

static void sampleVec(const std::vector<VecKey>& keys, float t, float* v)
{
	size_t i = keyBefore(keys, t);
	const VecKey& a = keys[i];
	if (i + 1 >= keys.size() || t <= a.t)
	{
		v[0] = a.x; v[1] = a.y; v[2] = a.z;
		return;
	}
	const VecKey& b = keys[i + 1];
	float f = (t - a.t) / fmaxf(b.t - a.t, 1e-6f);
	v[0] = a.x + (b.x - a.x) * f;
	v[1] = a.y + (b.y - a.y) * f;
	v[2] = a.z + (b.z - a.z) * f;
}

static float sampleScale(const std::vector<ScaleKey>& keys, float t)
{
	size_t i = keyBefore(keys, t);
	if (i + 1 >= keys.size() || t <= keys[i].t)
		return keys[i].s;
	float f = (t - keys[i].t) / fmaxf(keys[i + 1].t - keys[i].t, 1e-6f);
	return keys[i].s + (keys[i + 1].s - keys[i].s) * f;
}

static float sampleWeight(const std::vector<float>& keys, float t)
{
	size_t n = keys.size() / 2;
	if (n == 0)
		return 0.0f;
	if (t <= keys[0])
		return keys[1];
	for (size_t i = 0; i + 1 < n; i++)
		if (t <= keys[(i + 1) * 2])
		{
			float t0 = keys[i * 2], t1 = keys[(i + 1) * 2];
			float f = (t - t0) / fmaxf(t1 - t0, 1e-6f);
			return keys[i * 2 + 1] + (keys[(i + 1) * 2 + 1] - keys[i * 2 + 1]) * f;
		}
	return keys[(n - 1) * 2 + 1];
}

// Next idle as Morrowind's wander AI picks it: most of the time the plain Idle; otherwise each
// Idle2..9 rolls against its chance and the highest successful roll wins.
static int pickIdle(const Skeleton& sk, const Actor& a)
{
	if ((rand() % 1000) / 1000.0f > 0.75f)      // fIdleChanceMultiplier
		return 0;
	int best = 0;
	float bestRoll = 0.0f;
	for (int i = 0; i < 8; i++)
	{
		float roll = (rand() % 1000) / 10.0f;
		if (roll <= a.idle[i] && roll > bestRoll && i + 1 < sk.numIdle)
		{
			best = i + 1;
			bestRoll = roll;
		}
	}
	return best;
}

void actorAnimate(ActorSet& set, Actor& a, float dt)
{
	const Skeleton& sk = actorSkeleton(set, a.skeleton);
	if (!sk.groups.empty())
	{
		// a walk / run cycle keeps pace with the actor (its root's speed at the actor's scale)
		float gs = a.group < (int)sk.groupSpeed.size() ? sk.groupSpeed[a.group] : 0.0f;
		float x = a.place[3], y = a.place[7];
		if (gs > 20.0f && dt > 0.0f && a.lastXY[0] < 1e8f)
		{
			float moved = sqrtf((x - a.lastXY[0]) * (x - a.lastXY[0]) + (y - a.lastXY[1]) * (y - a.lastXY[1])) / dt;
			float scale = sqrtf(a.place[0] * a.place[0] + a.place[4] * a.place[4] + a.place[8] * a.place[8]);
			float want = fmaxf(0.5f, fminf(1.8f, moved / (gs * scale)));
			a.rate += (want - a.rate) * fminf(1.0f, dt * 6.0f);
		}
		else
			a.rate = 1.0f;
		a.lastXY[0] = x;
		a.lastXY[1] = y;
		a.time += dt * a.rate;
		const AnimGroup& g = sk.groups[a.group];
		if (a.time >= g.stop)
		{
			if (a.mode == ANIM_LOOP && a.loopsLeft != 0)
			{
				if (a.loopsLeft > 0)
					a.loopsLeft--;
				a.time = g.loopStart < g.stop ? g.loopStart : g.start;
			}
			else if (a.mode == ANIM_HOLD)
				a.time = g.stop;
			else if (sk.numIdle > 0)
			{
				a.mode = ANIM_IDLE;
				a.group = pickIdle(sk, a);
				a.time = sk.groups[a.group].start;
			}
			else
				a.time = g.start;
		}
	}
	a.blinkTimer -= dt;
	if (a.blinkTimer <= 0.0f && a.blinkTime < 0.0f)
		a.blinkTime = 0.0f;
	if (a.blinkTime >= 0.0f)
	{
		a.blinkTime += dt;
		if (a.blinkTime > 0.3f)
		{
			a.blinkTime = -1.0f;
			a.blinkTimer = 2.0f + (rand() % 4000) / 1000.0f;
		}
	}

	for (size_t i = 0; i < sk.bones.size(); i++)
	{
		const SkelBone& b = sk.bones[i];
		float local[12];
		memcpy(local, b.rest, sizeof(local));
		int ti = sk.trackOfBone[i];
		if (ti >= 0)
		{
			const Track& tr = sk.tracks[ti];
			float scale = tr.scale.empty() ? sqrtf(b.rest[0] * b.rest[0] + b.rest[4] * b.rest[4] + b.rest[8] * b.rest[8])
			                               : sampleScale(tr.scale, a.time);
			if (!tr.rot.empty())
			{
				float q[4];
				sampleRot(tr.rot, a.time, q);
				float w = q[0], x = q[1], y = q[2], z = q[3];
				float r[9] = { 1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y),
				               2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x),
				               2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y) };
				for (int row = 0; row < 3; row++)
					for (int col = 0; col < 3; col++)
						local[row * 4 + col] = r[row * 3 + col] * scale;
			}
			if (!tr.trans.empty())
			{
				float v[3];
				sampleVec(tr.trans, a.time, v);
				if ((int)i == sk.accumBone)
				{
					v[0] = b.rest[3];        // the walked distance stays out: only the bob up and down
					v[1] = b.rest[7];
				}
				local[3] = v[0]; local[7] = v[1]; local[11] = v[2];
			}
		}
		float* out = &a.pose[i * 12];
		if (b.parent >= 0)
			mul34(&a.pose[b.parent * 12], local, out);
		else
			memcpy(out, local, sizeof(local));
		// the head turned toward the player: its rotation turned around the actor's up axis (the
		// bones under it follow, being worked out after it)
		if ((int)i == sk.headBone && a.headYaw != 0.0f)
		{
			float c = cosf(a.headYaw), s = sinf(a.headYaw);
			for (int col = 0; col < 3; col++)
			{
				float x = out[col], y = out[4 + col];
				out[col] = c * x - s * y;
				out[4 + col] = s * x + c * y;
			}
		}
	}
	a.poseSerial++;
}

float actorMorphWeight(const Actor& a)
{
	float best = 0.0f;
	for (auto& m : a.meshes)
		for (auto& mt : m.morphs)
			best = fmaxf(best, sampleWeight(mt.keys, a.driveT));
	return best;
}

void actorDeform(ActorSet& set, Actor& a)
{
	// Skins are redone only for a new pose, head morphs only when the mouth or eyes moved
	bool newPose = !a.deformed || a.skinSerial != a.poseSerial;
	a.skinSerial = a.poseSerial;
	if (!a.deformed || newPose)
	{
		// Rigid parts ride the pose the skins are built from, not the newer one: re-skinning is throttled, and
		// a hand (skinned) lagging behind its forearm (rigid) looked detached
		bool skinned = false;
		for (auto& m : a.meshes)
			skinned |= m.kind == ACTOR_SKINNED;
		if (skinned)
			a.skinPose = a.pose;
		else
			a.skinPose.clear();
	}
	for (auto& m : a.meshes)
	{
		if (m.kind == ACTOR_SKINNED)
		{
			if (!newPose)
				continue;
			// world = place x bone pose x skin-to-bone, blended by the vertex weights
			size_t pal = m.palBone.size();
			static std::vector<float> mats;
			if (mats.size() < pal * 12)
				mats.resize(pal * 12);
			for (size_t p = 0; p < pal; p++)
			{
				float boneWorld[12];
				// Actor space: the placement is applied when drawing (actorMeshMatrix), so an actor that
				// only moves or turns needs no new skinning
				memcpy(boneWorld, &a.pose[m.palBone[p] * 12], sizeof(boneWorld));
				mul34(boneWorld, &m.palMat[p * 12], &mats[p * 12]);
			}
			u8* out = (u8*)m.verts;
			const float* s = m.src.data();
			const u8* ix = m.infIndex.data();
			const float* wt = m.infWeight.data();
			for (u32 v = 0; v < m.numVerts; v++, s += 3, ix += 4, wt += 4)
			{
				float* o = (float*)(out + v * 24);
				int n = m.infCount[v];
				if (n == 1)
				{
					xform(&mats[ix[0] * 12], s, o);
					continue;
				}
				float acc[3] = { 0, 0, 0 };
				for (int k = 0; k < n; k++)
				{
					float p[3];
					xform(&mats[ix[k] * 12], s, p);
					acc[0] += p[0] * wt[k]; acc[1] += p[1] * wt[k]; acc[2] += p[2] * wt[k];
				}
				o[0] = acc[0]; o[1] = acc[1]; o[2] = acc[2];
			}
			GSPGPU_FlushDataCache(m.verts, m.numVerts * 24);
		}
		else if (m.hasMorph)
		{
			// Head: mouth follows voice loudness over the "Talk" range, else the blink range
			float t;
			if (a.driveMorph)
				t = a.driveT;
			else if (a.talkLevel > 0.01f)
				t = m.talk[0] + (m.talk[1] - m.talk[0]) * fminf(1.0f, a.talkLevel * 2.0f);
			else if (a.blinkTime >= 0.0f)
				t = m.blink[0] + (m.blink[1] - m.blink[0]) * (a.blinkTime / 0.3f);
			else
				t = m.talk[0];
			if (t == m.morphT && a.deformed)
				continue;
			m.morphT = t;
			u8* out = (u8*)m.verts;
			float w[8] = {};
			size_t nm = m.morphs.size() < 8 ? m.morphs.size() : 8;
			for (size_t k = 0; k < nm; k++)
				w[k] = sampleWeight(m.morphs[k].keys, t);
			for (u32 v = 0; v < m.numVerts; v++)
			{
				float p[3] = { m.src[v * 3], m.src[v * 3 + 1], m.src[v * 3 + 2] };
				for (size_t k = 0; k < nm; k++)
					if (w[k] != 0.0f)
					{
						const float* d = &m.morphs[k].delta[v * 3];
						p[0] += d[0] * w[k]; p[1] += d[1] * w[k]; p[2] += d[2] * w[k];
					}
				memcpy(out + v * 24, p, 12);
			}
			GSPGPU_FlushDataCache(m.verts, m.numVerts * 24);
		}
	}
}

void actorMeshMatrix(const Actor& a, const ActorMesh& m, C3D_Mtx* out)
{
	float w[12];
	if (m.bone >= 0 && m.kind != ACTOR_SKINNED)
		mul34(a.place, &(a.skinPose.size() == a.pose.size() ? a.skinPose : a.pose)[m.bone * 12], w);
	else
		memcpy(w, a.place, sizeof(w));
	for (int r = 0; r < 3; r++)
		out->r[r] = FVec4_New(w[r * 4], w[r * 4 + 1], w[r * 4 + 2], w[r * 4 + 3]);
	out->r[3] = FVec4_New(0, 0, 0, 1);
}

// A session ending: the shared skeletons go now, not when the next session's replace them (both at once
// don't fit next to the game data)
void skeletonsFree()
{
	std::vector<Skeleton>().swap(g_sharedSkeletons);
	std::vector<bool>().swap(s_skelLoaded);
}
