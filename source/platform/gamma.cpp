#include <3ds.h>
#include <math.h>
#include <stdlib.h>
#include "platform/gamma.hpp"
#include "components/debug/debuglog.hpp"

int g_gammaPct = 100;
int g_autoGamma = -1;

// Where the light reading ends: at or under kDimLux no lift, at or over kBrightLux kMaxLift, between them along log(lux).
// Tuned from a New 3DS's own log: QTM read 27-61 lux in the dark, about 110 in a dim room and 1400-1900 in bright light
// (it reads far lower than a light meter: 500 / 10000 left the lift at 1.15 in the brightest light it saw). The full
// lift, 2 (1.5 was too little in bright light on the console), comes at 800, under the bright light it read, so a lit
// room already gets most of it; a dim room none. The camera's 0..255 mean has its own two ends and lift, still guesses
// (internal plan: gamma.md)
static const float kMaxLift = 2.0f;
static const float kMaxCameraLift = 2.0f;   // (as QTM's: at 1.5 a 2DS in sunlight still showed the dark too dark)
static const float kDimLux = 200.0f, kBrightLux = 800.0f;
static const float kDimLuma = 60.0f, kBrightLuma = 200.0f;
static const float kMinGamma = 0.8f, kMaxGamma = 2.5f;     // (Gamma's slider over 100 still adds to the full lift)
static const float kPollSecs = 3.0f;             // QTM read this often
static const float kLuxSmoothSecs = 6.0f;        // a thumb over the camera or an exposure jump doesn't pump the picture
static const float kEasePerSec = 0.5f;           // the value used moves at most this far a second
#ifdef __3DS__
static const s8 kCamExposure = 0;                // fixed (auto-exposure averages every scene to grey); Nintendo's -5..5
static const int kCamFrames = 6;                 // the sensor settles over its first frames: the last is read
#endif

static GammaSensor s_sensor = SENSOR_NONE;
static float s_lux = -1.0f;                      // smoothed QTM reading (-1: none yet)
static float s_lift = 1.0f;
static float s_loggedLift = -1.0f;
static float s_value = -1.0f;                    // the value used (-1: not yet, the first update sets it whole)
static float s_pollIn = 0.0f;
static float s_testLux = -1.0f;
static int s_testLuma = -1;
static volatile int s_camLuma = -1;              // the loading screen's camera reading (written by its thread)
static Thread s_camThread = nullptr;
static bool s_camRunning = false;
static int s_camFailures = 0;

static float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static float liftFor(float t, float maxLift)
{
	return 1.0f + (maxLift - 1.0f) * clampf(t, 0.0f, 1.0f);
}

static void setLift(float lift, const char* from, float reading)
{
	s_lift = lift;
	if (fabsf(lift - s_loggedLift) >= 0.05f)
	{
		logf("gamma: %s %.0f, lift %.2f", from, reading, lift);
		s_loggedLift = lift;
	}
}

static void useLux(float lux)
{
	if (!(lux >= 0.0f) || !isfinite(lux))
		return;
	// Smoothed on log(lux), as the lift is read: in lux a drop from 2000 to 100 took half a minute to show
	float k = s_lux < 0.0f ? 1.0f : fminf(1.0f, kPollSecs / kLuxSmoothSecs);
	if (k >= 1.0f)
		s_lux = lux;
	else
		s_lux = expf(logf(fmaxf(s_lux, 1.0f)) + (logf(fmaxf(lux, 1.0f)) - logf(fmaxf(s_lux, 1.0f))) * k);
	float t = (logf(fmaxf(s_lux, 1.0f)) - logf(kDimLux)) / (logf(kBrightLux) - logf(kDimLux));
	setLift(liftFor(t, kMaxLift), "lux", lux);
}

static void useLuma(int luma)
{
	if (luma < 0)
		return;
	setLift(liftFor((luma - kDimLuma) / (kBrightLuma - kDimLuma), kMaxCameraLift), "camera luma", (float)luma);
}

#ifdef __3DS__
// One frame's mean brightness (0..255) from the inner camera at a fixed exposure; -1: no reading
static int cameraLuma()
{
	if (R_FAILED(camInit()))
		return -1;
	const s16 w = 160, h = 120;           // QQVGA: about 38 KB, freed at once
	u16* buf = (u16*)malloc(w * h * 2);
	u32 bytes = 0;
	int luma = -1;
	if (buf && R_SUCCEEDED(CAMU_SetSize(SELECT_IN1, SIZE_QQVGA, CONTEXT_A))
		&& R_SUCCEEDED(CAMU_SetOutputFormat(SELECT_IN1, OUTPUT_RGB_565, CONTEXT_A))
		&& R_SUCCEEDED(CAMU_SetFrameRate(SELECT_IN1, FRAME_RATE_30))
		&& R_SUCCEEDED(CAMU_SetAutoExposure(SELECT_IN1, false))
		&& R_SUCCEEDED(CAMU_SetExposure(SELECT_IN1, kCamExposure))
		&& R_SUCCEEDED(CAMU_SetTrimming(PORT_CAM1, false))
		&& R_SUCCEEDED(CAMU_GetMaxBytes(&bytes, w, h))
		&& R_SUCCEEDED(CAMU_SetTransferBytes(PORT_CAM1, bytes, w, h))
		&& R_SUCCEEDED(CAMU_Activate(SELECT_IN1)))
	{
		CAMU_ClearBuffer(PORT_CAM1);
		bool got = false;
		if (R_SUCCEEDED(CAMU_StartCapture(PORT_CAM1)))
		{
			for (int f = 0; f < kCamFrames; f++)
			{
				Handle ev = 0;
				if (R_FAILED(CAMU_SetReceiving(&ev, buf, PORT_CAM1, w * h * 2, (s16)bytes)))
				{
					got = false;
					break;
				}
				// (a timeout is not an R_FAILED result: 0 is the frame)
				got = svcWaitSynchronization(ev, 300000000LL) == 0;
				svcCloseHandle(ev);
				if (!got)
					break;
			}
			CAMU_StopCapture(PORT_CAM1);
		}
		CAMU_Activate(SELECT_NONE);
		if (got)
		{
			u32 sum = 0;
			for (int i = 0; i < w * h; i++)
			{
				u16 p = buf[i];
				u32 r = ((p >> 11) & 31) * 255 / 31, g = ((p >> 5) & 63) * 255 / 63, b = (p & 31) * 255 / 31;
				sum += (r * 77 + g * 150 + b * 29) >> 8;
			}
			luma = (int)(sum / (w * h));
		}
	}
	free(buf);
	camExit();
	return luma;
}
#endif

static void camThread(void*)
{
#ifdef __3DS__
	s_camLuma = cameraLuma();
#else
	s_camLuma = -1;
#endif
}

void gammaInit()
{
	s_sensor = SENSOR_NONE;
#ifdef __3DS__
	// QTM is a New 3DS service; it says "unavailable" (and lux 400) on a New 2DS XL, and for blacklisted titles
	if (qtmCheckServicesRegistered() && R_SUCCEEDED(qtmInit(QTM_SERVICE_SYSTEM)))
	{
		QtmStatus status = QTM_STATUS_UNAVAILABLE;
		if (R_SUCCEEDED(QTMS_GetQtmStatus(&status)) && status != QTM_STATUS_UNAVAILABLE)
			s_sensor = SENSOR_QTM;
		else
			qtmExit();
	}
	// Every model has the inner camera
	if (s_sensor == SENSOR_NONE)
		s_sensor = SENSOR_CAMERA;
#endif
	logf("gamma: light reading %s", s_sensor == SENSOR_QTM ? "QTM lux" : s_sensor == SENSOR_CAMERA
		? "inner camera at loading screens" : "none");
}

void gammaExit()
{
	gammaLoadEnd();
#ifdef __3DS__
	if (qtmIsInitialized())
		qtmExit();
#endif
}

GammaSensor gammaSensor()
{
	return s_sensor;
}

bool gammaAutoOn()
{
	return s_sensor != SENSOR_NONE && (g_autoGamma > 0 || (g_autoGamma < 0 && s_sensor == SENSOR_QTM));
}

float gammaLift()
{
	return gammaAutoOn() ? s_lift : 1.0f;
}

float gammaValue()
{
	return s_value < 0.0f ? 1.0f : s_value;
}

static float gammaTarget()
{
	return clampf(g_gammaPct / 100.0f * gammaLift(), kMinGamma, kMaxGamma);
}

float gammaUpdate(float dt)
{
	if (s_sensor == SENSOR_QTM && gammaAutoOn() && (s_pollIn -= dt) <= 0.0f)
	{
		s_pollIn = kPollSecs;
		float lux = s_testLux;
#ifdef __3DS__
		if (s_testLux < 0.0f && R_FAILED(QTMS_GetCameraLuminance(&lux)))
			lux = -1.0f;
#endif
		useLux(lux);
	}
	float target = gammaTarget();
	if (s_value < 0.0f)
		s_value = target;
	else if (s_value < target)
		s_value = fminf(target, s_value + kEasePerSec * dt);
	else if (s_value > target)
		s_value = fmaxf(target, s_value - kEasePerSec * dt);
	return s_value;
}

void gammaLoadStart()
{
	if (s_sensor != SENSOR_CAMERA || !gammaAutoOn() || s_camRunning)
		return;
	if (s_testLuma >= 0)
	{
		s_camLuma = s_testLuma;
		s_camRunning = true;
		return;
	}
	// Its own thread while the load runs (the reading waits on the camera most of its half second); none: on the spot
	s_camLuma = -1;
	s_camRunning = true;
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	s_camThread = threadCreate(camThread, nullptr, 16 * 1024, prio + 1, -2, false);
	if (!s_camThread)
		camThread(nullptr);
}

void gammaLoadEnd()
{
	if (!s_camRunning)
		return;
	s_camRunning = false;
	if (s_camThread)
	{
		threadJoin(s_camThread, U64_MAX);
		threadFree(s_camThread);
		s_camThread = nullptr;
	}
	if (s_camLuma < 0)
	{
		if (s_camFailures++ == 0)
			logf("gamma: no camera reading at this load (the last one stays)");
		return;
	}
	useLuma(s_camLuma);
	s_value = gammaTarget();         // (behind the loading screen: no easing)
}

void gammaTestLux(float lux)
{
	s_sensor = SENSOR_QTM;
	s_testLux = lux;
	s_pollIn = 0.0f;
}

void gammaTestLuma(int luma)
{
	s_sensor = SENSOR_CAMERA;
	s_testLux = -1.0f;
	s_testLuma = luma;
}
