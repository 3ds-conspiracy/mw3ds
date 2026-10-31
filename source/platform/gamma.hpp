#pragma once

// Gamma (Options): a curve on the light the world is drawn with, light ^ (1 / gamma), as Morrowind's own gamma slider.
// Black stays black and full light stays full: only the dim middle comes up, so torches and Light spells still matter.
// Auto gamma adds a lift for bright surroundings (the 3DS screen washes out in sunlight) from a light reading:
// - a New 3DS / New 3DS XL: the inner camera's lux from QTM (the face tracking service), read every few seconds
// - the others (Old 3DS, Old 2DS, New 2DS XL): the inner camera itself, read only while a loading screen is up
// The value used eases toward its target, so the picture never jumps.

extern int g_gammaPct;               // settings.txt gamma=80..180 (percent: 100 is the vanilla look)
extern int g_autoGamma;              // settings.txt autogamma=on|off; -1: not chosen (on with QTM, off with the camera)

static const int kGammaMinPct = 80, kGammaMaxPct = 180, kGammaStepPct = 5;

enum GammaSensor
{
	SENSOR_NONE,                     // no light reading: Auto gamma does nothing
	SENSOR_QTM,                      // QTMS_GetCameraLuminance, in lux
	SENSOR_CAMERA,                   // the inner camera at a fixed exposure, at loading screens
};

void gammaInit();                    // after logInit: which light reading this console has
void gammaExit();
GammaSensor gammaSensor();
bool gammaQtmOpen();                 // QTM's session (the face tracking and its IR LED) is open: only while Auto gamma reads it
bool gammaAutoOn();                  // Auto gamma chosen (or by default) and a reading to go on
float gammaUpdate(float dt);         // once a frame: polls QTM, eases; the value the renderer uses
float gammaValue();                  // the value used now
float gammaLift();                   // the reading's lift (1: dim surroundings or Auto off)
void gammaLoadStart();               // a loading screen went up: the camera reading starts (its own thread)
void gammaLoadEnd();                 // it comes down: the reading is waited for, the value set without easing
// Tests (no sensor natively or in Azahar): a reading as if from QTM (lux) or from the camera at the next load (0..255;
// -1: the camera path with the real camera)
void gammaTestLux(float lux);
void gammaTestLuma(int luma);
