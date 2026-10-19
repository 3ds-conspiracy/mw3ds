// Movies (the intro and the end)
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

#include "mwsound/soundmanagerimp.hpp"
#include "platform/linear.hpp"
#include "components/debug/debuglog.hpp"
#include <malloc.h>

bool Session::playMovie(const std::string& name)
{
	std::string stem = lower(name);
	size_t dot = stem.rfind(".bik");
	if (dot != std::string::npos)
		stem.resize(dot);
	for (auto& ch : stem)
		if (!isalnum((unsigned char)ch) && ch != '_')
			ch = '_';
	stopMovie();
	char path[256];
	snprintf(path, sizeof(path), "%s/movies/%s.mwv", w.dataDir, stem.c_str());
	FILE* f = testNoMovies ? nullptr : fopen(path, "rb");
	if (!f)
	{
		logf("movie: no %s", path);
		return false;
	}
	char magic[4];
	u32 hdr[4];
	if (fread(magic, 4, 1, f) != 1 || memcmp(magic, "MWV1", 4) != 0 || fread(hdr, 4, 4, f) != 4)
	{
		fclose(f);
		return false;
	}
	movieFile = f;
	movieFrames = hdr[0];
	movieFps100 = hdr[1] ? hdr[1] : 1000;
	movieW = hdr[2];
	movieH = hdr[3];
	movieFrame = -1;
	movieTime = 0.0f;
	std::string snd = "movie_" + stem + ".snd";
	char spath[256];
	snprintf(spath, sizeof(spath), "%s/music/%s", w.dataDir, snd.c_str());
	struct stat st;
	if (stat(spath, &st) == 0)
		audioMusicPlay({ snd }, 0.8f);
	logf("movie: %s, %u frames", stem.c_str(), movieFrames);
	return true;
}

void Session::movieUpdate(float dt)
{
	if (!movieFile)
		return;
	movieTime += dt;
	int want = (int)(movieTime * movieFps100 / 100.0f);
	if (want >= (int)movieFrames)
	{
		stopMovie();
		return;
	}
	// catch up to the frame due (skipping any a slow frame left behind)
	while (movieFrame < want)
	{
		u32 size = 0;
		if (fread(&size, 4, 1, movieFile) != 1 || size > (1 << 20))
		{
			stopMovie();
			return;
		}
		movieFrame++;
		if (movieFrame < want)
		{
			fseek(movieFile, size, SEEK_CUR);
			continue;
		}
		std::vector<u8> buf(size);
		if (fread(buf.data(), 1, size, movieFile) != size)
		{
			stopMovie();
			return;
		}
		LinearGuard guard;
		if (movieTexOk)
			deferredTexDelete(&movieTex);
		Tex3DS_Texture t = Tex3DS_TextureImport(buf.data(), size, &movieTex, nullptr, false);
		movieTexOk = t != nullptr;
		if (t)
		{
			Tex3DS_TextureFree(t);
			C3D_TexSetFilter(&movieTex, GPU_LINEAR, GPU_LINEAR);
		}
	}
}

void Session::movieDraw()
{
	if (!movieTexOk || !movieW || !movieH)
		return;
	Tex3DS_SubTexture sub = { (u16)movieW, (u16)movieH, 0.0f, 1.0f, (float)movieW / movieTex.width,
		1.0f - (float)movieH / movieTex.height };
	C2D_Image img = { &movieTex, &sub };
	float k = 240.0f / movieH;
	C2D_DrawImageAt(img, (400.0f - movieW * k) / 2.0f, 0.0f, 0.5f, nullptr, k, k);
}

void Session::stopMovie()
{
	if (movieFile)
	{
		fclose(movieFile);
		movieFile = nullptr;
		playMusic(false);
	}
	if (movieTexOk)
	{
		LinearGuard guard;
		deferredTexDelete(&movieTex);
		movieTexOk = false;
	}
}
