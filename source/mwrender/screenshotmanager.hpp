#pragma once
#include <citro3d.h>

// Saves both screens as one 400x480 24-bit BMP (top screen above bottom).
// Call right after C3D_FrameBegin so the last frame's transfers are done.
bool screenshotSave(const char* path);
// A render target texture (the local map's) as a BMP, at the end of the frame being drawn. The PC build only
// (tools/convert/maprender.py bakes the world map with it); the 3DS build returns false
bool screenshotSaveTexture(C3D_Tex* tex, const char* path);
// The PC build draws only frames that save something: this one is drawn, so a render to a texture saved after it
// happens (the 3DS draws every frame)
void screenshotDrawThisFrame();
