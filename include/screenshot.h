#pragma once

// Saves both screens as one 400x480 24-bit BMP (top screen above bottom).
// Call right after C3D_FrameBegin so the last frame's transfers are done.
bool screenshotSave(const char* path);
