#include "screenshot.h"

#include <3ds.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static const int kImgW = 400, kImgH = 480, kRowBytes = kImgW * 3;

// Framebuffers are stored rotated: column-major, 240 pixels per column,
// bottom-to-top. Converts screen pixel (x, y) to BMP BGR bytes.
static void readPixel(const u8* fb, GSPGPU_FramebufferFormat fmt, int x, int y, u8* out)
{
	int idx = x * 240 + (239 - y);
	switch (fmt)
	{
	case GSP_RGBA8_OES:
		out[0] = fb[idx * 4 + 1]; out[1] = fb[idx * 4 + 2]; out[2] = fb[idx * 4 + 3];
		break;
	case GSP_BGR8_OES:
		out[0] = fb[idx * 3 + 0]; out[1] = fb[idx * 3 + 1]; out[2] = fb[idx * 3 + 2];
		break;
	case GSP_RGB565_OES:
	{
		u16 p = ((const u16*)fb)[idx];
		out[0] = (p & 0x1F) << 3; out[1] = ((p >> 5) & 0x3F) << 2; out[2] = (p >> 11) << 3;
		break;
	}
	case GSP_RGB5_A1_OES:
	{
		u16 p = ((const u16*)fb)[idx];
		out[0] = ((p >> 1) & 0x1F) << 3; out[1] = ((p >> 6) & 0x1F) << 3; out[2] = (p >> 11) << 3;
		break;
	}
	default:
		out[0] = out[1] = out[2] = 0xFF;
		break;
	}
}

static void blitScreen(u8* img, gfxScreen_t screen, int w, int yOffset, int xOffset)
{
	const u8* fb = gfxGetFramebuffer(screen, GFX_LEFT, nullptr, nullptr);
	GSPGPU_FramebufferFormat fmt = gfxGetScreenFormat(screen);
	for (int y = 0; y < 240; y++)
	{
		// BMP rows are stored bottom-up
		u8* row = img + (kImgH - 1 - (y + yOffset)) * kRowBytes;
		for (int x = 0; x < w; x++)
			readPixel(fb, fmt, x, y, row + (x + xOffset) * 3);
	}
}

static void put16(u8* p, u16 v) { p[0] = v; p[1] = v >> 8; }
static void put32(u8* p, u32 v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

bool screenshotSave(const char* path)
{
	const u32 imgBytes = kRowBytes * kImgH;
	u8 header[54] = {};
	header[0] = 'B'; header[1] = 'M';
	put32(header + 2, sizeof(header) + imgBytes);
	put32(header + 10, sizeof(header));
	put32(header + 14, 40);
	put32(header + 18, kImgW);
	put32(header + 22, kImgH);
	put16(header + 26, 1);
	put16(header + 28, 24);
	put32(header + 34, imgBytes);

	u8* img = (u8*)calloc(1, imgBytes);
	if (!img)
		return false;
	blitScreen(img, GFX_TOP, 400, 0, 0);
	blitScreen(img, GFX_BOTTOM, 320, 240, 40);

	FILE* f = fopen(path, "wb");
	bool ok = f && fwrite(header, sizeof(header), 1, f) == 1 && fwrite(img, imgBytes, 1, f) == 1;
	if (f)
		fclose(f);
	free(img);
	return ok;
}
