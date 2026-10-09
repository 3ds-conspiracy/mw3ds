#include "audio.h"
#include "datapath.h"

#include "linear.h"

#include <3ds.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <list>
#include <unordered_map>
#include <vector>

#include "log.h"

static bool s_enabled = false;
static std::string s_dataDir;

struct SoundHeader { char magic[4]; u32 rate, samples; };

// IMA ADPCM ('SND2', encoded by Python's audioop.lin2adpcm: first sample in the high nibble)
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
	s16* data = nullptr;
	u32 samples = 0, rate = 22050;
	u64 lastUse = 0;
};

static std::unordered_map<std::string, CachedSound> s_cache;
static u32 s_cacheBytes = 0;
static const u32 kCacheLimit = 6 * 1024 * 1024;

static const int kFxChannels = 23;          // 0..22 effects and voices, 23 music (the DSP has 24)
static const int kMusicChannel = 23;

struct Channel
{
	ndspWaveBuf buf;
	std::string file;
	bool active = false;
};
static Channel s_channels[kFxChannels];

// Music streaming
static const int kStreamBufs = 3;
static const u32 kStreamSamples = 16384;
static s16* s_streamData[kStreamBufs];
static ndspWaveBuf s_streamBuf[kStreamBufs];
static FILE* s_musicFile = nullptr;
static std::vector<std::string> s_playlist;
static size_t s_track = 0;
static bool s_musicAdpcm = false;
static u32 s_musicLeft = 0;
static AdpcmState s_musicState;
static float s_musicVolume = 0.35f;

bool audioEnabled() { return s_enabled; }

// How often each sound file was started, and how often a sound found no free channel (tests read them)
static std::unordered_map<std::string, int> s_started;
static int s_denied = 0;
int audioStartedCount(const std::string& file)
{
	auto it = s_started.find(file);
	return it == s_started.end() ? 0 : it->second;
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
static void evict(u32 incoming)
{
	while (s_cacheBytes + incoming > kCacheLimit)
	{
		auto oldest = s_cache.end();
		for (auto it = s_cache.begin(); it != s_cache.end(); ++it)
			if (!inUse(it->first) && (oldest == s_cache.end() || it->second.lastUse < oldest->second.lastUse))
				oldest = it;
		if (oldest == s_cache.end())
			return;
		s_cacheBytes -= oldest->second.samples * 2;
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
	if (fread(&h, sizeof(h), 1, f) != 1 || (memcmp(h.magic, "SND1", 4) != 0 && memcmp(h.magic, "SND2", 4) != 0))
	{
		fclose(f);
		return nullptr;
	}
	bool adpcm = memcmp(h.magic, "SND2", 4) == 0;
	evict(h.samples * 2);
	CachedSound s;
	s.samples = h.samples;
	s.rate = h.rate;
	s.data = (s16*)lockedLinearAlloc(h.samples * 2);
	if (!s.data)
	{
		fclose(f);
		monitorOnce(("snd:" + file).c_str(), "no memory for sound %s", file.c_str());
		return nullptr;
	}
	if (adpcm)
	{
		std::vector<u8> packed((h.samples + 1) / 2);
		fread(packed.data(), 1, packed.size(), f);
		AdpcmState st;
		const u8* in = packed.data();
		adpcmDecode(st, in, s.data, h.samples);
	}
	else
		fread(s.data, 2, h.samples, f);
	fclose(f);
	DSP_FlushDataCache(s.data, h.samples * 2);
	s.lastUse = osGetTime();
	s_cacheBytes += h.samples * 2;
	CachedSound& slot = s_cache[file];
	slot = s;
	return &slot;
}

float audioDuration(const std::string& file)
{
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
			s_started[file]++;               // no sound device (an emulator without the DSP firmware): the start still counts
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
		ndspChnSetFormat(ch, NDSP_FORMAT_MONO_PCM16);
		setMix(ch, volume, pan);
		memset(&c.buf, 0, sizeof(c.buf));
		c.buf.data_vaddr = s->data;
		c.buf.nsamples = s->samples;
		c.buf.looping = loop;
		c.file = file;
		c.active = true;
		ndspChnWaveBufAdd(ch, &c.buf);
		s_started[file]++;
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
	const s16* data = (const s16*)c.buf.data_vaddr;
	u32 n = c.buf.nsamples, pos = ndspChnGetSamplePos(ch);
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
	if (s_playlist.empty())
		return false;
	MarkScope mark("music open");
	for (size_t tries = 0; tries < s_playlist.size(); tries++)
	{
		std::string path = s_dataDir + "/music/" + s_playlist[s_track % s_playlist.size()];
		s_track++;
		s_musicFile = fopen(path.c_str(), "rb");
		SoundHeader h;
		if (s_musicFile && fread(&h, sizeof(h), 1, s_musicFile) == 1 &&
			(memcmp(h.magic, "SND1", 4) == 0 || memcmp(h.magic, "SND2", 4) == 0))
		{
			ndspChnSetRate(kMusicChannel, (float)h.rate);
			s_musicRateSet = s_musicFileRate = h.rate;
			s_musicAdpcm = memcmp(h.magic, "SND2", 4) == 0;
			s_musicLeft = h.samples;
			s_musicState = AdpcmState();
			return true;
		}
		if (s_musicFile)
			fclose(s_musicFile);
		s_musicFile = nullptr;
	}
	return false;
}

static bool fillStream(int i)
{
	MarkScope mark("music read");
	u32 got = 0;
	while (got < kStreamSamples)
	{
		if (!s_musicFile && !openTrack())
			return false;
		if (s_musicAdpcm)
		{
			u32 n = kStreamSamples - got < s_musicLeft ? kStreamSamples - got : s_musicLeft;
			// bytes still needed: a pending low nibble covers the first sample
			u32 bytes = (n + (s_musicState.low ? 0 : 1)) / 2;
			u8 packed[kStreamSamples / 2 + 1];
			u32 read = fread(packed, 1, bytes, s_musicFile);
			if (read < bytes)
				n = read * 2 + (s_musicState.low ? 1 : 0) < n ? read * 2 + (s_musicState.low ? 1 : 0) : n;
			const u8* in = packed;
			adpcmDecode(s_musicState, in, s_streamData[i] + got, n);
			got += n;
			s_musicLeft -= n;
			if (s_musicLeft == 0 || read < bytes)
			{
				fclose(s_musicFile);
				s_musicFile = nullptr;
			}
			continue;
		}
		got += fread(s_streamData[i] + got, 2, kStreamSamples - got, s_musicFile);
		if (got < kStreamSamples)
		{
			fclose(s_musicFile);
			s_musicFile = nullptr;
		}
	}
	DSP_FlushDataCache(s_streamData[i], kStreamSamples * 2);
	memset(&s_streamBuf[i], 0, sizeof(ndspWaveBuf));
	s_streamBuf[i].data_vaddr = s_streamData[i];
	s_streamBuf[i].nsamples = kStreamSamples;
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

void audioMusicPlay(const std::vector<std::string>& files, float volume)
{
	if (!s_enabled || files.empty())
		return;
	s_playlist = files;
	s_track = 0;
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
	ndspChnSetFormat(kMusicChannel, NDSP_FORMAT_MONO_PCM16);
	setMix(kMusicChannel, volume, 0.0f);
	for (int i = 0; i < kStreamBufs; i++)
		fillStream(i);
}

void audioUpdate()
{
	if (!s_enabled || s_playlist.empty())
		return;
	for (int i = 0; i < kStreamBufs; i++)
		if (s_streamBuf[i].status == NDSP_WBUF_DONE)
			fillStream(i);
}
