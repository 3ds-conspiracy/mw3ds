#include "mwsound/soundmanagerimp.hpp"
#include "components/files/datapath.hpp"

#include "platform/linear.hpp"

#include <3ds.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <list>
#include <unordered_map>
#include <vector>

#include "components/debug/debuglog.hpp"
#include <algorithm>
#include <array>
#include <functional>
#include <sys/stat.h>
#include <tex3ds.h>
#include <citro2d.h>
#include "mwbase/session.hpp"
#include "mwphysics/physicssystem.hpp"
#include <cstdlib>
#include <malloc.h>

static bool s_enabled = false;
static std::string s_dataDir;

struct SoundHeader { char magic[4]; u32 rate, samples; };

// 'SND3' (tools/convert/dspadpcm.py): the header, s16 coefs[16], u32 envelope bytes, the DSP-ADPCM frames (8 bytes
// for 14 samples), then the envelope (a byte per 256 samples, sqrt(RMS) x 255). The DSP decodes the frames itself:
// a sound is read into linear memory as it is, no CPU decoding, a little over a quarter of PCM16's room
static bool isDsp(const SoundHeader& h) { return memcmp(h.magic, "SND3", 4) == 0; }
static bool isSound(const SoundHeader& h)
{
	return isDsp(h) || memcmp(h.magic, "SND2", 4) == 0 || memcmp(h.magic, "SND1", 4) == 0;
}
static u32 dspBytes(u32 samples) { return (samples + 13) / 14 * 8; }
static const u32 kEnvelopeBlock = 256;

// IMA ADPCM ('SND2', from conversions before 'SND3', encoded by Python's audioop.lin2adpcm: first sample in the high
// nibble)
static const int kImaIndex[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
static const int kImaStep[89] = {
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88,
	97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
	724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660,
	4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
	18500, 20350, 22385, 24623, 27086, 29794, 32767 };

struct AdpcmState
{
	int pred = 0, index = 0;
	bool low = false;          // next sample comes from the low nibble of `byte`
	u8 byte = 0;
};

// Decodes n samples, pulling bytes from `in` (advanced as it goes)
static void adpcmDecode(AdpcmState& st, const u8*& in, s16* out, u32 n)
{
	for (u32 i = 0; i < n; i++)
	{
		int delta;
		if (st.low)
			delta = st.byte & 15;
		else
		{
			st.byte = *in++;
			delta = st.byte >> 4;
		}
		st.low = !st.low;
		int step = kImaStep[st.index];
		st.index += kImaIndex[delta];
		st.index = st.index < 0 ? 0 : st.index > 88 ? 88 : st.index;
		int diff = step >> 3;
		if (delta & 4) diff += step;
		if (delta & 2) diff += step >> 1;
		if (delta & 1) diff += step >> 2;
		st.pred += (delta & 8) ? -diff : diff;
		st.pred = st.pred < -32768 ? -32768 : st.pred > 32767 ? 32767 : st.pred;
		out[i] = (s16)st.pred;
	}
}

struct CachedSound
{
	void* data = nullptr;          // DSP-ADPCM frames ('SND3'), else PCM16
	u32 bytes = 0, samples = 0, rate = 22050;
	bool dsp = false;
	u16 coefs[16] = {};
	std::vector<u8> envelope;      // ('SND3')
	u64 lastUse = 0;
};

static std::unordered_map<std::string, CachedSound> s_cache;
static u32 s_cacheBytes = 0;
// Decoded sounds kept in linear memory (voices, effects), the least recently used freed past this. An Old 3DS has
// 22 MB of linear memory for everything (platform/heaps.cpp): its cache is a third as big, or the voices heard on a walk
// through town took the room the next cells' textures needed

static const int kFxChannels = 23;          // 0..22 effects and voices, 23 music (the DSP has 24)
static const int kMusicChannel = 23;

struct Channel
{
	ndspWaveBuf buf;
	ndspAdpcmData adpcm;           // the DSP's start (and loop) state: the first frame's header, no history
	std::string file;
	bool active = false;
};
static Channel s_channels[kFxChannels];

// Music streaming
static const int kStreamBufs = 3;
static const u32 kStreamSamples = 16384;
static const u32 kStreamFrames = 1170;      // a 'SND3' buffer: 16380 samples in 9360 bytes (whole frames)
static s16* s_streamData[kStreamBufs];
static ndspWaveBuf s_streamBuf[kStreamBufs];
static FILE* s_musicFile = nullptr;
static std::vector<std::string> s_playlist;
static size_t s_track = 0;
static bool s_musicLoop = true;       // audioMusicPlay's loop
static bool s_musicAdpcm = false;
static bool s_musicDsp = false;       // 'SND3': frames go to the DSP as they are
static u16 s_musicCoefs[16];
static ndspAdpcmData s_musicStart;
static bool s_musicFirst = false;     // the next buffer is the track's first (the DSP's state starts over)
static u32 s_musicLeft = 0;
static AdpcmState s_musicState;
static float s_musicVolume = 0.35f;

bool audioEnabled() { return s_enabled; }

// How often each sound file was started, and how often a sound found no free channel (tests read them)
static std::unordered_map<std::string, int> s_started;
static int s_denied = 0;
static std::unordered_map<std::string, float> s_startVolume;   // the volume each file last started at
int audioStartedCount(const std::string& file)
{
	auto it = s_started.find(file);
	return it == s_started.end() ? 0 : it->second;
}
float audioStartVolume(const std::string& file)
{
	auto it = s_startVolume.find(file);
	return it == s_startVolume.end() ? -1.0f : it->second;
}
int audioDeniedCount() { return s_denied; }

bool audioInit(const char* dataDir)
{
	s_dataDir = dataDir;
	Result rc = ndspInit();
	if (R_FAILED(rc))
	{
		logf("audio: ndspInit failed (%08lX) - no DSP firmware? Dump it with DSP1 to /3ds/dspfirm.cdc", rc);
		return false;
	}
	ndspSetOutputMode(NDSP_OUTPUT_STEREO);
	for (int i = 0; i < kStreamBufs; i++)
		s_streamData[i] = (s16*)lockedLinearAlloc(kStreamSamples * 2);
	s_enabled = true;
	logf("audio: ready");
	return true;
}

unsigned audioCacheBytes()
{
	return s_cacheBytes;
}

// A session ending (loading a save): the decoded sounds go, so the next session doesn't load its own on top
void audioCacheClear()
{
	if (!s_enabled)
		return;
	for (int i = 0; i < kMusicChannel; i++)
		ndspChnReset(i);
	for (auto& c : s_cache)
		lockedLinearFree(c.second.data);
	s_cache.clear();
	s_cacheBytes = 0;          // (left counting the freed sounds, the cache was "full" for the next session)
}

void audioExit()
{
	if (!s_enabled)
		return;
	for (int i = 0; i <= kMusicChannel; i++)
		ndspChnReset(i);
	if (s_musicFile)
		fclose(s_musicFile);
	for (auto& c : s_cache)
		lockedLinearFree(c.second.data);
	s_cache.clear();
	for (int i = 0; i < kStreamBufs; i++)
		lockedLinearFree(s_streamData[i]);
	ndspExit();
	s_enabled = false;
}

static bool inUse(const std::string& file)
{
	for (auto& c : s_channels)
		if (c.active && c.file == file && c.buf.status != NDSP_WBUF_DONE)
			return true;
	return false;
}

// Frees least recently used sounds that aren't playing until `incoming` more bytes fit
static void evict(u32 incoming, u32 limit)
{
	while (s_cacheBytes + incoming > limit)
	{
		auto oldest = s_cache.end();
		for (auto it = s_cache.begin(); it != s_cache.end(); ++it)
			if (!inUse(it->first) && (oldest == s_cache.end() || it->second.lastUse < oldest->second.lastUse))
				oldest = it;
		if (oldest == s_cache.end())
			return;
		s_cacheBytes -= oldest->second.bytes;
		lockedLinearFree(oldest->second.data);
		s_cache.erase(oldest);
	}
}

static CachedSound* load(const std::string& file)
{
	auto it = s_cache.find(file);
	if (it != s_cache.end())
	{
		it->second.lastUse = osGetTime();
		return &it->second;
	}
	MarkScope mark("sound read");
	std::string path = shardedPath(s_dataDir, "snd", file);
	FILE* f = fopen(path.c_str(), "rb");
	if (!f)
	{
		logf("audio: missing %s", file.c_str());
		monitorOnce(("snd:" + file).c_str(), "missing sound %s", file.c_str());
		return nullptr;
	}
	SoundHeader h;
	if (fread(&h, sizeof(h), 1, f) != 1 || !isSound(h))
	{
		fclose(f);
		return nullptr;
	}
	CachedSound s;
	s.samples = h.samples;
	s.rate = h.rate;
	s.dsp = isDsp(h);
	u32 envelopeBytes = 0;
	if (s.dsp && (fread(s.coefs, 2, 16, f) != 16 || fread(&envelopeBytes, 4, 1, f) != 1))
	{
		fclose(f);
		return nullptr;
	}
	s.bytes = s.dsp ? dspBytes(h.samples) : h.samples * 2;
	evict(s.bytes, g_profile.soundCacheBytes);
	s.data = lockedLinearAlloc(s.bytes);
	// (linear memory full: every sound not playing goes, then one more try)
	if (!s.data)
	{
		evict(s.bytes, 0);
		s.data = lockedLinearAlloc(s.bytes);
	}
	if (!s.data)
	{
		fclose(f);
		monitorOnce(("snd:" + file).c_str(), "no memory for sound %s", file.c_str());
		return nullptr;
	}
	if (s.dsp)
	{
		size_t got = fread(s.data, 1, s.bytes, f);
		if (got < s.bytes)
			memset((u8*)s.data + got, 0, s.bytes - got);
		s.envelope.resize(envelopeBytes);
		if (envelopeBytes && fread(s.envelope.data(), 1, envelopeBytes, f) != envelopeBytes)
			s.envelope.clear();
	}
	else if (memcmp(h.magic, "SND2", 4) == 0)
	{
		// In pieces through a buffer on the stack: the whole packed sound in one block (150 KB for a long one) was
		// one of the main thread's biggest heap requests, and an Old 3DS late in a session had no such block left
		u8 packed[2048];
		AdpcmState st;
		for (u32 done = 0; done < h.samples;)
		{
			u32 n = h.samples - done < 2 * sizeof(packed) ? h.samples - done : 2 * sizeof(packed);
			size_t bytes = (n + 1) / 2;
			size_t got = fread(packed, 1, bytes, f);
			if (got < bytes)
				memset(packed + got, 0, bytes - got);
			const u8* in = packed;
			adpcmDecode(st, in, (s16*)s.data + done, n);
			done += n;
		}
	}
	else
		fread(s.data, 2, h.samples, f);
	fclose(f);
	DSP_FlushDataCache(s.data, s.bytes);
	s.lastUse = osGetTime();
	s_cacheBytes += s.bytes;
	CachedSound& slot = s_cache[file];
	slot = std::move(s);
	return &slot;
}

void audioPreload(const std::string& file)
{
	if (s_enabled && !file.empty())
		load(file);
}

float audioDuration(const std::string& file)
{
	auto it = s_cache.find(file);
	if (it != s_cache.end())
		return it->second.rate ? (float)it->second.samples / it->second.rate : 0.0f;
	MarkScope mark("sound probe");
	std::string path = shardedPath(s_dataDir, "snd", file);
	FILE* f = fopen(path.c_str(), "rb");
	if (!f)
		return 0.0f;
	SoundHeader h;
	bool ok = fread(&h, sizeof(h), 1, f) == 1 && h.rate > 0;
	fclose(f);
	return ok ? (float)h.samples / h.rate : 0.0f;
}

static float s_fxScale = 1.0f, s_musicScale = 1.0f;

static void setMix(int ch, float volume, float pan)
{
	volume *= ch == kMusicChannel ? s_musicScale : s_fxScale;
	float mix[12] = {};
	mix[0] = volume * (pan > 0.0f ? 1.0f - pan : 1.0f);
	mix[1] = volume * (pan < 0.0f ? 1.0f + pan : 1.0f);
	ndspChnSetMix(ch, mix);
}

int audioPlay(const std::string& file, float volume, float pan, bool loop, float pitch)
{
	if (file.empty())
		return -1;
	if (!s_enabled)
	{
		if (!loop)
		{
			s_started[file]++;               // no sound device (an emulator without the DSP firmware): the start still counts
			s_startVolume[file] = volume;
		}
		return -1;
	}
	CachedSound* s = load(file);
	if (!s)
		return -1;
	for (int ch = 0; ch < kFxChannels; ch++)
	{
		Channel& c = s_channels[ch];
		if (c.active && c.buf.status != NDSP_WBUF_DONE)
			continue;
		ndspChnReset(ch);
		ndspChnSetInterp(ch, NDSP_INTERP_LINEAR);
		ndspChnSetRate(ch, (float)s->rate * pitch);
		ndspChnSetFormat(ch, s->dsp ? NDSP_FORMAT_ADPCM : NDSP_FORMAT_MONO_PCM16);
		setMix(ch, volume, pan);
		memset(&c.buf, 0, sizeof(c.buf));
		c.buf.data_vaddr = s->data;
		if (s->dsp)
		{
			ndspChnSetAdpcmCoefs(ch, s->coefs);
			c.adpcm.index = *(const u8*)s->data;
			c.adpcm.history0 = c.adpcm.history1 = 0;
			c.buf.adpcm_data = &c.adpcm;
		}
		c.buf.nsamples = s->samples;
		c.buf.looping = loop;
		c.file = file;
		c.active = true;
		ndspChnWaveBufAdd(ch, &c.buf);
		s_started[file]++;
		s_startVolume[file] = volume;
		return ch;
	}
	s_denied++;
	monitorOnce(("nochannel:" + file).c_str(), "no free sound channel for %s", file.c_str());
	return -1;
}

bool audioPlaying(int ch)
{
	if (!s_enabled || ch < 0 || ch >= kFxChannels)
		return false;
	const Channel& c = s_channels[ch];
	return c.active && c.buf.status != NDSP_WBUF_DONE;
}

float audioLoudness(int ch)
{
	if (!audioPlaying(ch))
		return 0.0f;
	const Channel& c = s_channels[ch];
	u32 n = c.buf.nsamples, pos = ndspChnGetSamplePos(ch);
	auto it = s_cache.find(c.file);
	if (it != s_cache.end() && it->second.dsp)
	{
		// the converter's envelope: the 512 samples from here, as below
		const std::vector<u8>& env = it->second.envelope;
		u32 b = pos / kEnvelopeBlock;
		if (b >= env.size())
			return 0.0f;
		float a = env[b] * (1.0f / 255.0f), z = (b + 1 < env.size() ? env[b + 1] : env[b]) * (1.0f / 255.0f);
		return sqrtf((a * a * a * a + z * z * z * z) * 0.5f);
	}
	const s16* data = (const s16*)c.buf.data_vaddr;
	u32 end = pos + 512 < n ? pos + 512 : n;
	if (pos >= end)
		return 0.0f;
	float sum = 0.0f;
	for (u32 i = pos; i < end; i += 2)
	{
		float s = data[i] * (1.0f / 32768.0f);
		sum += s * s;
	}
	return sqrtf(sum / ((end - pos + 1) / 2));
}

void audioStop(int ch)
{
	if (!s_enabled || ch < 0 || ch >= kFxChannels)
		return;
	ndspChnWaveBufClear(ch);
	s_channels[ch].active = false;
}

void audioSetMix(int ch, float volume, float pan)
{
	if (s_enabled && ch >= 0 && ch < kFxChannels)
		setMix(ch, volume, pan);
}

// ---- Music

// The rate the music channel was last told, and the open file's own (a channel reset puts it back to the
// DSP's 32728 Hz, and only opening a track sets it again): a stream whose two differ plays at the wrong speed
static u32 s_musicRateSet = 0, s_musicFileRate = 0;

static bool openTrack()
{
	if (s_playlist.empty() || (!s_musicLoop && s_track >= s_playlist.size()))
		return false;
	MarkScope mark("music open");
	for (size_t tries = 0; tries < s_playlist.size(); tries++)
	{
		std::string path = s_dataDir + "/music/" + s_playlist[s_track % s_playlist.size()];
		s_track++;
		s_musicFile = fopen(path.c_str(), "rb");
		SoundHeader h;
		u32 envelopeBytes = 0;
		if (s_musicFile && fread(&h, sizeof(h), 1, s_musicFile) == 1 && isSound(h) &&
			(!isDsp(h) || (fread(s_musicCoefs, 2, 16, s_musicFile) == 16 && fread(&envelopeBytes, 4, 1, s_musicFile) == 1)))
		{
			ndspChnSetRate(kMusicChannel, (float)h.rate);
			s_musicRateSet = s_musicFileRate = h.rate;
			s_musicDsp = isDsp(h);
			s_musicAdpcm = memcmp(h.magic, "SND2", 4) == 0;
			ndspChnSetFormat(kMusicChannel, s_musicDsp ? NDSP_FORMAT_ADPCM : NDSP_FORMAT_MONO_PCM16);
			if (s_musicDsp)
				ndspChnSetAdpcmCoefs(kMusicChannel, s_musicCoefs);
			s_musicLeft = h.samples;
			s_musicState = AdpcmState();
			s_musicFirst = true;
			return true;
		}
		if (s_musicFile)
			fclose(s_musicFile);
		s_musicFile = nullptr;
	}
	return false;
}

// A track starts in a buffer of its own once the last one's buffers have played: the channel's rate, format and
// DSP-ADPCM coefficients are the track's, and are set while nothing of another track is queued
static bool streamIdle()
{
	for (int k = 0; k < kStreamBufs; k++)
		if (s_streamBuf[k].status == NDSP_WBUF_QUEUED || s_streamBuf[k].status == NDSP_WBUF_PLAYING)
			return false;
	return true;
}

static bool fillStream(int i)
{
	if (!s_musicFile && (!streamIdle() || !openTrack()))
		return false;            // (the list is over, played once; or the last track still plays out)
	MarkScope mark("music read");
	u32 got = 0;
	if (s_musicDsp)
	{
		// the frames as they are: the DSP decodes them
		u32 n = kStreamFrames * 14 < s_musicLeft ? kStreamFrames * 14 : s_musicLeft;
		u32 bytes = dspBytes(n);
		u32 read = fread(s_streamData[i], 1, bytes, s_musicFile);
		if (read < bytes)
			n = read / 8 * 14 < n ? read / 8 * 14 : n;
		got = n;
		s_musicLeft -= n;
		if (s_musicLeft == 0 || read < bytes)
		{
			fclose(s_musicFile);
			s_musicFile = nullptr;
		}
	}
	else if (s_musicAdpcm)
	{
		u32 n = kStreamSamples < s_musicLeft ? kStreamSamples : s_musicLeft;
		// bytes still needed: a pending low nibble covers the first sample
		u32 bytes = (n + (s_musicState.low ? 0 : 1)) / 2;
		u8 packed[kStreamSamples / 2 + 1];
		u32 read = fread(packed, 1, bytes, s_musicFile);
		if (read < bytes)
			n = read * 2 + (s_musicState.low ? 1 : 0) < n ? read * 2 + (s_musicState.low ? 1 : 0) : n;
		const u8* in = packed;
		adpcmDecode(s_musicState, in, s_streamData[i], n);
		got = n;
		s_musicLeft -= n;
		if (s_musicLeft == 0 || read < bytes)
		{
			fclose(s_musicFile);
			s_musicFile = nullptr;
		}
	}
	else
	{
		got = fread(s_streamData[i], 2, kStreamSamples, s_musicFile);
		if (got < kStreamSamples)
		{
			fclose(s_musicFile);
			s_musicFile = nullptr;
		}
	}
	if (!got)
		return false;
	DSP_FlushDataCache(s_streamData[i], s_musicDsp ? dspBytes(got) : got * 2);
	memset(&s_streamBuf[i], 0, sizeof(ndspWaveBuf));
	s_streamBuf[i].data_vaddr = s_streamData[i];
	s_streamBuf[i].nsamples = got;
	// a track's first buffer starts the DSP's decoding over; the others carry on from the buffer before
	if (s_musicDsp && s_musicFirst)
	{
		s_musicStart.index = *(const u8*)s_streamData[i];
		s_musicStart.history0 = s_musicStart.history1 = 0;
		s_streamBuf[i].adpcm_data = &s_musicStart;
	}
	s_musicFirst = false;
	if (s_musicRateSet != s_musicFileRate)
		monitorOnce("musicrate", "music channel at rate %u, its file at %u", (unsigned)s_musicRateSet, (unsigned)s_musicFileRate);
	ndspChnWaveBufAdd(kMusicChannel, &s_streamBuf[i]);
	return true;
}

void audioSetScales(float effects, float music)
{
	s_fxScale = effects;
	s_musicScale = music;
	if (s_enabled)
		setMix(kMusicChannel, s_musicVolume, 0.0f);
}

void audioMusicPlay(const std::vector<std::string>& files, float volume, bool loop)
{
	if (!s_enabled || files.empty())
		return;
	s_playlist = files;
	s_track = 0;
	s_musicLoop = loop;
	s_musicVolume = volume;
	// The reset puts the channel back to the DSP's own rate (32728 Hz); only opening a track sets the
	// file's (22050). A track left open mid-stream would carry on at the wrong rate (1.5x fast after a
	// few loads): close it so the first buffer opens the new one and sets its rate
	if (s_musicFile)
	{
		fclose(s_musicFile);
		s_musicFile = nullptr;
	}
	ndspChnReset(kMusicChannel);
	s_musicRateSet = 0;
	ndspChnSetInterp(kMusicChannel, NDSP_INTERP_LINEAR);
	setMix(kMusicChannel, volume, 0.0f);
	// (the reset dropped the queue: no buffer is the DSP's now)
	for (int i = 0; i < kStreamBufs; i++)
		memset(&s_streamBuf[i], 0, sizeof(ndspWaveBuf));
	for (int i = 0; i < kStreamBufs; i++)
		fillStream(i);
}

void audioUpdate()
{
	if (!s_enabled || s_playlist.empty())
		return;
	for (int i = 0; i < kStreamBufs; i++)
		if (s_streamBuf[i].status == NDSP_WBUF_DONE || s_streamBuf[i].status == NDSP_WBUF_FREE)
			fillStream(i);
}

// The sound of water nearby (OpenMW's SoundManager::updateWaterSound and WaterSoundUpdater; the fallback values are
// Morrowind.ini's [Water] NearWater*): under the water, full; outdoors within 1024 units of the water's height, twice
// the share of an 8 x 8 grid of points 1000 units around the player whose land lies under the sea's level (0), so it
// plays at the shore and fades a few steps inland; indoors in a cell with water, within 512 units of its level, louder
// the closer. The grid is sampled four times a second (OpenMW every frame)
void Session::updateAmbience()
{
	static const float kRadius = 1000.0f, kOutdoorTolerance = 1024.0f, kIndoorTolerance = 512.0f;
	static const int kPoints = 8;
	if (w.time >= waterNext || w.time < waterNext - 1.0f)
	{
		waterNext = w.time + 0.25f;
		float volume = 0.0f;
		Cell& here = w.here();
		if (w.current >= 0 && here.hasWater())
		{
			float dist = fabsf(here.waterZ - w.player.feet[2]);
			bool interior = w.cells[w.current].interior;
			if (playerEyeZ(w.player) < here.waterZ)
				volume = 1.0f;
			else if (!interior && dist < kOutdoorTolerance)
			{
				Scene scene = w.scene();
				const float step = kRadius * 2.0f / (kPoints - 1);
				int under = 0;
				for (int x = 0; x < kPoints; x++)
					for (int y = 0; y < kPoints; y++)
					{
						float px = w.player.feet[0] - kRadius + x * step, py = w.player.feet[1] - kRadius + y * step;
						float z = -1e9f, got;
						for (Cell* c : scene.cells)
							if (collisionLandFloor(c->collision, px, py, 1e6f, -1e6f, &got) && got > z)
								z = got;
						under += z < 0.0f;          // (no land there: the open sea)
					}
				volume = under * 2.0f / (kPoints * kPoints);
			}
			else if (interior && dist < kIndoorTolerance)
				volume = (kIndoorTolerance - dist) / kIndoorTolerance;
		}
		waterVolume = fminf(1.0f, volume);
	}
	auto it = w.game.sounds.find(lower(w.game.waterSound));
	if (waterVolume <= 0.0f || w.game.waterSound.empty() || it == w.game.sounds.end())
	{
		if (waterChannel >= 0)
		{
			audioStop(waterChannel);
			waterChannel = -1;
		}
		return;
	}
	float volume = it->second.volume * waterVolume;
	if (waterChannel < 0 || !audioPlaying(waterChannel))
		waterChannel = audioPlay(it->second.file, volume, 0.0f, true);
	else
		audioSetMix(waterChannel, volume, 0.0f);
}

// ---- Sound

void Session::mix3d(const float* pos, float& volume, float& pan, float minDist, float maxDist, bool inverse)
{
	float dx = pos[0] - listener[0], dy = pos[1] - listener[1], dz = pos[2] - listener[2];
	float dist = sqrtf(dx * dx + dy * dy + dz * dz);
	// inverse: OpenAL's inverse distance clamped, as OpenMW plays its sounds (full within minDist, then minDist / dist);
	// past maxDist not played, as the line below has it
	float att = inverse ? (dist <= minDist ? 1.0f : dist > maxDist ? 0.0f : minDist / dist)
		: dist <= minDist ? 1.0f : fmaxf(0.0f, 1.0f - (dist - minDist) / fmaxf(1.0f, maxDist - minDist));
	volume *= att;
	float rel = atan2f(dx, dy) - listener[3];
	pan = dist > 1.0f ? sinf(rel) * 0.8f : 0.0f;
}

// A sound record's range in units (OpenMW's SoundBufferPool::insertSound): its own, or fAudioDefaultMinDistance /
// fAudioDefaultMaxDistance when both are 0, times fAudioMinDistanceMult and fAudioMaxDistanceMult (20 and 50)
static void soundRange(const GameData& g, const SoundDef& d, float& minDist, float& maxDist)
{
	float lo = (float)d.minRange, hi = (float)d.maxRange;
	if (d.minRange == 0 && d.maxRange == 0)
	{
		lo = g.gmstf("faudiodefaultmindistance", 5.0f);
		hi = g.gmstf("faudiodefaultmaxdistance", 40.0f);
	}
	minDist = fmaxf(1.0f, lo * g.gmstf("faudiomindistancemult", 20.0f));
	maxDist = fmaxf(minDist, hi * g.gmstf("faudiomaxdistancemult", 50.0f));
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
		float lo, hi;
		soundRange(w.game, it->second, lo, hi);
		mix3d(pos, volume, pan, lo, hi, true);
	}
	if (volume > 0.01f)
	{
		int ch = audioPlay(it->second.file, volume, pan, false, pitch);
		// A script's sound on a reference is "playing" till it ends
		if (script && ref >= 0 && ch >= 0 && scriptSounds.size() < 16)
			scriptSounds.push_back({ ref, lower(soundId), w.time + audioDuration(it->second.file) / fmaxf(pitch, 0.1f) });
	}
}

void Session::preloadSound(const std::string& soundId)
{
	auto it = w.game.sounds.find(lower(soundId));
	if (it != w.game.sounds.end())
		audioPreload(it->second.file);
}

void Session::playRandomSound(int ref, const std::string& prefix, float volume)
{
	std::vector<std::string> ids;
	for (int k = 1; k <= 9; k++)
		if (w.game.sounds.count(prefix + std::to_string(k)))
			ids.push_back(prefix + std::to_string(k));
	if (!ids.empty())
		playSound(ref, ids[rand() % ids.size()], volume);
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

// Loops whose object is gone (its cell not loaded, or disabled) stop. Before a cell's objects leave memory too: the
// reference list shrinks under their slots, and a stale number reads the freed end of it
void Session::dropDeadLoops()
{
	for (size_t k = 0; k < loopSounds.size();)
	{
		LoopSound& l = loopSounds[k];
		if (l.ref < 0 || l.ref >= (int)w.refs.size() || !w.active(l.ref) || !w.refs[l.ref].visible())
		{
			audioStop(l.channel);
			loopSounds.erase(loopSounds.begin() + k);
			continue;
		}
		k++;
	}
}

// A loop holds a sound channel only while it can be heard, and only the loudest few do: a town's fires or a
// Dwemer ruin's steam and machinery are dozens of loops, which at a channel each (silent or not) used up all
// of them and left every other sound without one
void Session::updateLoopSounds()
{
	static const size_t kMaxLoops = 12;
	dropDeadLoops();
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
		{
			float lo, hi;
			soundRange(w.game, it->second, lo, hi);
			mix3d(pos, volume, pans[k], lo, hi, true);
		}
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
	std::string voiceFile;
	bool voiced = w.game.voiceFile(key, &voiceFile);
	if (voiced)
	{
		float volume = 1.0f, pan = 0.0f;
		if (ref >= 0)
		{
			float pos[3] = { w.refs[ref].pos[0], w.refs[ref].pos[1], w.refs[ref].pos[2] + 110.0f };
			mix3d(pos, volume, pan, 300.0f, 2500.0f);
		}
		channel = audioPlay(voiceFile, volume, pan);
		duration = audioDuration(voiceFile);       // (after: the line just loaded knows its length, no second open)
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
	// Subtitles off: a line that is heard isn't written too (Morrowind's and OpenMW's subtitles setting)
	if (!text.empty() && (g_subtitles || !voiced))
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

// The region's ambient sounds outdoors in fair weather (Morrowind.ini: 1 to 5 s between; each by its chance)
void Session::updateRegionSounds(float dt)
{
	if (w.current >= 0 && w.cells[w.current].underSky() && w.weatherNow <= 3 && (ambientTimer -= dt) <= 0.0f)
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
}
