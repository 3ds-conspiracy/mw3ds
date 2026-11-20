// The Circle Pad Pro over ir:USER (an Old 3DS): turned on in Options, looked for, read, let go.
// The way the games made for it talk to it, as the emulators that run them answer (Azahar's ir:USER and its Circle Pad
// Pro, service/ir/ir_user.cpp and extra_hid.cpp), 3dbrew's IR Services and Circle Pad Pro pages describe it, and
// ctru-rs's Circle Pad Pro example does it on the console:
// - ir:rst and ir:USER shut each other out: ir:rst stays closed on an Old 3DS (hidShouldUseIrrst, engine.cpp)
// - InitializeIrNopShared hands the service a block of memory (bit rate 4). Its first 0x10 bytes are the link's status
//   (byte 8: 0 none, 1 connecting, 2 connected), the next 0x10 the receive ring (first packet, end, count), then one
//   8-byte entry per packet (offset into the data, size) and the data, a ring that wraps
// - RequireConnection(1) asks for the accessory. ctru-rs gives it a tenth of a second, then Disconnect, a tenth to let
//   go, and asks again: here for kSearchFor at a time
// - once connected: request 2 reads its calibration (sets of the stick's centre and x / y scale, each with a CRC-8;
//   a set with a bad sum is skipped), request 1 asks for an input report every few ms. The request is sent again
//   after every batch read (ctru-rs: "remind the CPP that we're still listening")
// - a packet is 0xA5, the network id, the payload's size (6 bits; 0x40 set: 6 more and the next byte), the payload and
//   a CRC-8 (polynomial 0x07) of all before it. ReleaseReceivedData frees the packets read
// - an input report (0x10): the stick's X and Y as two 12-bit numbers (X low), a byte of battery level (bits 0-4) and
//   ZL / ZR / R (bits 5 / 6 / 7, set while NOT held), a spare byte. The stick is (reading - centre) * scale / 8, about
//   the Circle Pad's own +-0x9C
#include "platform/circlepadpro.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include "components/debug/debuglog.hpp"

static CppState s_state = CPP_OFF;
static bool s_linked = false;          // input reports arrive
static u32 s_held = 0, s_lastHeld = 0, s_down = 0, s_up = 0;
static int s_stickX = 0, s_stickY = 0; // the stick in the console's stick counts
static int s_battery = -1;
// Calibration until the accessory's own is read: the values Azahar carries, read from a real one
static int s_centreX = 0x800, s_centreY = 0x800;
static float s_scaleX = 0.57f, s_scaleY = 0.57f;
static bool s_calibrated = false;      // its own calibration read
static int s_reports = 0, s_badSums = 0;

static u8 crc8(const u8* p, u32 n)
{
	u8 c = 0;
	for (u32 i = 0; i < n; i++)
	{
		c ^= p[i];
		for (int b = 0; b < 8; b++)
			c = (c & 0x80) ? (u8)((c << 1) ^ 0x07) : (u8)(c << 1);
	}
	return c;
}

static void letGo()
{
	s_linked = false;
	s_held = 0;
	s_stickX = s_stickY = 0;
}

// The calibration (0x11): offset, size, then 16-byte sets: a byte, the centre as two 12-bit numbers, the x and y scale
// (floats), 3 bytes, a CRC-8 of the 15 before it. The first good set is taken
static bool takeCalibration(const u8* r, u32 n)
{
	if (n < 5)
		return false;
	u32 size = std::min<u32>(r[3] | (r[4] << 8), n - 5);
	for (u32 at = 5; at + 16 <= 5 + size; at += 16)
	{
		const u8* c = r + at;
		if (crc8(c, 15) != c[15])
			continue;
		float sx, sy;
		memcpy(&sx, c + 4, 4);
		memcpy(&sy, c + 8, 4);
		if (!(sx > 0.1f && sx < 4.0f && sy > 0.1f && sy < 4.0f))
			continue;
		s_centreX = c[1] | ((c[2] & 0x0F) << 8);
		s_centreY = (c[2] >> 4) | (c[3] << 4);
		s_scaleX = sx;
		s_scaleY = sy;
		s_calibrated = true;
		logf("cpp: calibration read (centre %03x %03x, scale %.3f %.3f)", s_centreX, s_centreY, sx, sy);
		return true;
	}
	logf("cpp: calibration answer with no good set (%u bytes): the usual values kept", (unsigned)size);
	return true;
}

bool cppTakePacket(const u8* p, u32 size)
{
	if (size < 4 || p[0] != 0xA5)
		return false;
	u32 head = 3, n = p[2] & 0x3F;
	if (p[2] & 0x40)
	{
		head = 4;
		n = ((p[2] & 0x3F) << 8) | p[3];
	}
	if (n == 0 || head + n + 1 > size)
		return false;
	if (crc8(p, head + n) != p[head + n])
	{
		if (s_badSums++ == 0)
			logf("cpp: a packet's checksum differs (%u bytes), dropped", (unsigned)size);
		return false;
	}
	const u8* r = p + head;
	if (r[0] == 0x11)
		return takeCalibration(r, n);
	if (r[0] != 0x10 || n < 5)
		return false;
	int x = r[1] | ((r[2] & 0x0F) << 8);
	int y = (r[2] >> 4) | (r[3] << 4);
	s_stickX = (int)((x - s_centreX) * s_scaleX / 8.0f);
	s_stickY = (int)((y - s_centreY) * s_scaleY / 8.0f);
	s_battery = r[4] & 0x1F;
	s_held = ((r[4] & 0x20) ? 0 : KEY_ZL) | ((r[4] & 0x40) ? 0 : KEY_ZR) | ((r[4] & 0x80) ? 0 : KEY_R);
	if (s_reports++ == 0)
		logf("cpp: first input report (battery %d)", s_battery);
	s_linked = true;
	s_state = CPP_CONNECTED;
	return true;
}

void cppTestLinkLost()
{
	letGo();
	s_state = CPP_SEARCHING;
	logf("cpp: link lost");
}

CppState cppState() { return s_state; }
int cppBattery() { return s_battery; }
bool cppConnected() { return s_linked; }
u32 cppKeysHeld() { return s_held; }
u32 cppKeysDown() { return s_down; }
u32 cppKeysUp() { return s_up; }

void cstickRead(circlePosition* cs)
{
	if (s_linked)
	{
		cs->dx = (s16)s_stickX;
		cs->dy = (s16)s_stickY;
		return;
	}
	hidCstickRead(cs);
}

#ifdef __3DS__
#include <malloc.h>

// The service's buffers: 16 packets received (an input report is 10 bytes, the calibration answer 75), 4 sent
static const u32 kShared = 0x1000, kRecv = 0x800, kRecvPackets = 16, kSend = 0x200, kSendPackets = 4;
static const u32 kBitRate = 4, kDeviceCirclePadPro = 1;
static const u8 kPollMs = 8;
static const float kAttempt = 0.1f, kLetGo = 0.1f, kSearchFor = 5.0f;
static const float kCalibrateWait = 0.3f, kAskAgain = 0.1f, kFirstReportWait = 2.0f, kQuietLost = 1.0f;

// What the link is doing within the state the game shows
enum { PH_NONE, PH_ATTEMPT, PH_LETGO, PH_CALIBRATE, PH_FIRST_REPORT, PH_REPORTS };
static int s_phase = PH_NONE;
static Handle s_ir = 0, s_block = 0;
static u8* s_mem = nullptr;
static bool s_open = false, s_hooked = false, s_reopen = false;
static u64 s_phaseAt = 0, s_searchAt = 0, s_lastReport = 0, s_lastAsk = 0;
static int s_calibAsks = 0, s_silentLinks = 0, s_seenReports = 0;
static aptHookCookie s_hook;

static float since(u64 tick)
{
	return (float)(svcGetSystemTick() - tick) / SYSCLOCK_ARM11;
}

static void phase(int ph)
{
	s_phase = ph;
	s_phaseAt = svcGetSystemTick();
}

static Result irCall(u32 header, u32 arg = 0)
{
	u32* cmd = getThreadCommandBuffer();
	cmd[0] = header;
	cmd[1] = arg;
	Result r = svcSendSyncRequest(s_ir);
	return R_FAILED(r) ? r : (Result)cmd[1];
}

static unsigned linkStatus()
{
	return ((volatile u8*)s_mem)[0x08];
}

static u32 rd32(u32 at)
{
	u32 v;
	memcpy(&v, (const void*)(s_mem + at), 4);
	return v;
}

// A request to the accessory (SendIrNop, a static buffer)
static Result sendToPad(const u8* data, u32 size)
{
	u32* cmd = getThreadCommandBuffer();
	cmd[0] = IPC_MakeHeader(0xD, 1, 2);
	cmd[1] = size;
	cmd[2] = IPC_Desc_StaticBuffer(size, 0);
	cmd[3] = (u32)data;
	Result r = svcSendSyncRequest(s_ir);
	return R_FAILED(r) ? r : (Result)cmd[1];
}

// Request 1: a report every kPollMs (the third byte (period + 2) * 4, as the games and ctru-rs send it)
static Result askReports()
{
	static const u8 req[3] = { 0x01, kPollMs, (u8)((kPollMs + 2) << 2) };
	s_lastAsk = svcGetSystemTick();
	return sendToPad(req, sizeof(req));
}

// Request 2: the calibration, 0x40 bytes from 0 (four sets)
static Result askCalibration()
{
	static const u8 req[6] = { 0x02, 0x20, 0x00, 0x00, 0x40, 0x00 };
	return sendToPad(req, sizeof(req));
}

// The packets in the receive ring, oldest first, then freed: how many there were
static u32 readPackets()
{
	u32 first = rd32(0x10), count = rd32(0x18);
	if (count == 0)
		return 0;
	count = std::min(count, kRecvPackets);
	const u32 slots = 0x20, data = 0x20 + kRecvPackets * 8, ring = kRecv - kRecvPackets * 8;
	u8 buf[128];
	for (u32 i = 0; i < count; i++)
	{
		u32 slot = (first + i) % kRecvPackets;
		u32 off = rd32(slots + slot * 8), size = rd32(slots + slot * 8 + 4);
		if (size == 0 || size > sizeof(buf))
			continue;
		for (u32 j = 0; j < size; j++)
			buf[j] = s_mem[data + (off + j) % ring];
		if (cppTakePacket(buf, size) && buf[(buf[2] & 0x40) ? 4 : 3] == 0x10)
			s_lastReport = svcGetSystemTick();
	}
	irCall(IPC_MakeHeader(0x19, 1, 0), count);  // ReleaseReceivedData
	return count;
}

static void startSearch()
{
	letGo();
	s_state = CPP_SEARCHING;
	s_searchAt = svcGetSystemTick();
	Result r = irCall(IPC_MakeHeader(0x6, 1, 0), kDeviceCirclePadPro);   // RequireConnection
	logf("cpp: looking for a Circle Pad Pro (RequireConnection 0x%08lx)", (unsigned long)r);
	phase(PH_ATTEMPT);
}

static void notFound(const char* why)
{
	// (with what the service last showed: status 0 nothing, 1 connecting; the byte after it, its try's state)
	logf("cpp: not found: %s (status %u, trying %u)", why, linkStatus(), (unsigned)((volatile u8*)s_mem)[0x09]);
	irCall(IPC_MakeHeader(0x9, 0, 0));             // Disconnect
	letGo();
	s_state = CPP_NOT_FOUND;
	phase(PH_NONE);
}

static void closeIr()
{
	if (!s_open)
		return;
	irCall(IPC_MakeHeader(0x9, 0, 0));             // Disconnect
	irCall(IPC_MakeHeader(0x2, 0, 0));             // FinalizeIrNop
	svcCloseHandle(s_block);
	free(s_mem);
	s_mem = nullptr;
	svcCloseHandle(s_ir);
	s_open = false;
	letGo();
	phase(PH_NONE);
}

static void onApt(APT_HookType type, void*)
{
	if ((type == APTHOOK_ONSUSPEND || type == APTHOOK_ONSLEEP) && s_open)
	{
		logf("cpp: infrared let go for the HOME menu / sleep");
		closeIr();
		s_state = CPP_SEARCHING;
		s_reopen = true;                           // (looked for again once back: cppUpdate)
	}
}

// ir:USER and its block of memory; false (and why logged) when the console refuses
static bool openIr()
{
	Result r = srvGetServiceHandle(&s_ir, "ir:USER");
	if (R_FAILED(r))
	{
		logf("cpp: no ir:USER (0x%08lx)", (unsigned long)r);
		return false;
	}
	s_mem = (u8*)memalign(0x1000, kShared);
	if (!s_mem)
	{
		svcCloseHandle(s_ir);
		return false;
	}
	memset(s_mem, 0, kShared);
	r = svcCreateMemoryBlock(&s_block, (u32)s_mem, kShared, MEMPERM_READ, MEMPERM_READWRITE);
	if (R_SUCCEEDED(r))
	{
		u32* cmd = getThreadCommandBuffer();
		cmd[0] = IPC_MakeHeader(0x18, 6, 2);       // InitializeIrNopShared
		cmd[1] = kShared;
		cmd[2] = kRecv;
		cmd[3] = kRecvPackets;
		cmd[4] = kSend;
		cmd[5] = kSendPackets;
		cmd[6] = kBitRate;
		cmd[7] = IPC_Desc_SharedHandles(1);
		cmd[8] = s_block;
		r = svcSendSyncRequest(s_ir);
		if (R_SUCCEEDED(r))
			r = (Result)cmd[1];
		if (R_FAILED(r))
			svcCloseHandle(s_block);
	}
	if (R_FAILED(r))
	{
		logf("cpp: ir:USER refused its memory (0x%08lx)", (unsigned long)r);
		free(s_mem);
		s_mem = nullptr;
		svcCloseHandle(s_ir);
		return false;
	}
	if (!s_hooked)
	{
		aptHook(&s_hook, onApt, nullptr);
		s_hooked = true;
	}
	s_open = true;
	logf("cpp: ir:USER open");
	return true;
}

static void openAndSearch()
{
	s_reopen = false;
	s_silentLinks = 0;
	if (!s_open && !openIr())
	{
		s_state = CPP_NOT_FOUND;
		return;
	}
	startSearch();
}

void cppLookAgain()
{
	if (s_state == CPP_NOT_FOUND || s_state == CPP_SEARCHING)
		openAndSearch();
}

static void linkStep()
{
	if (!s_open)
		return;
	switch (s_phase)
	{
	case PH_ATTEMPT:
		if (linkStatus() == 2)
		{
			logf("cpp: connected after %.1f s: reading its calibration", since(s_searchAt));
			s_state = CPP_CALIBRATING;
			s_calibAsks = 1;
			askCalibration();
			phase(PH_CALIBRATE);
		}
		else if (since(s_phaseAt) > kAttempt)
		{
			irCall(IPC_MakeHeader(0x9, 0, 0));     // Disconnect, to ask again once it has let go
			phase(PH_LETGO);
		}
		break;
	case PH_LETGO:
		if (since(s_phaseAt) <= kLetGo)
			break;
		if (since(s_searchAt) > kSearchFor)
			notFound("nothing answered in the time given");
		else
		{
			irCall(IPC_MakeHeader(0x6, 1, 0), kDeviceCirclePadPro);
			phase(PH_ATTEMPT);
		}
		break;
	case PH_CALIBRATE:
		readPackets();
		if (s_calibrated || since(s_phaseAt) > kCalibrateWait * s_calibAsks)
		{
			if (!s_calibrated && s_calibAsks < 2)
			{
				s_calibAsks++;
				askCalibration();
				break;
			}
			if (!s_calibrated)
				logf("cpp: no calibration answer: the usual values kept");
			askReports();
			s_seenReports = s_reports;
			phase(PH_FIRST_REPORT);
		}
		break;
	case PH_FIRST_REPORT:
	case PH_REPORTS:
		if (readPackets())
			askReports();                          // (read: reminded it is listened to)
		else if (since(s_lastAsk) > kAskAgain && since(s_lastReport) > kAskAgain)
			askReports();                          // (no report yet, or none lately: asked again)
		if (s_phase == PH_FIRST_REPORT)
		{
			if (s_reports > s_seenReports)
			{
				logf("cpp: Circle Pad Pro connected");
				phase(PH_REPORTS);
			}
			else if (linkStatus() != 2 || since(s_phaseAt) > kFirstReportWait)
			{
				// (connected but silent: looked for again, twice at most)
				if (++s_silentLinks >= 2)
					notFound("it connects but sends no reports");
				else
				{
					irCall(IPC_MakeHeader(0x9, 0, 0));
					startSearch();
				}
			}
		}
		else if (linkStatus() != 2 || since(s_lastReport) > kQuietLost)
		{
			// Taken off, out of reach, or gone to standby (ZL or ZR wakes it): looked for again for a while
			logf("cpp: link lost (status %u, %d reports)", linkStatus(), s_reports);
			irCall(IPC_MakeHeader(0x9, 0, 0));
			s_silentLinks = 0;
			startSearch();
		}
		break;
	}
}

void cppUpdate(bool enabled)
{
	if (!enabled && s_state != CPP_OFF)
	{
		closeIr();
		s_reopen = false;
		s_state = CPP_OFF;
		logf("cpp: turned off: infrared let go");
	}
	else if (enabled && (s_state == CPP_OFF || s_reopen))
		openAndSearch();
	else if (enabled)
		linkStep();
	s_down = s_held & ~s_lastHeld;
	s_up = s_lastHeld & ~s_held;
	s_lastHeld = s_held;
}

void cppShutdown()
{
	closeIr();
	if (s_hooked)
		aptUnhook(&s_hook);
	s_hooked = false;
	s_state = CPP_OFF;
}
#else
// The PC build: no infrared. Turned on, it waits as if looking; scripted packets (CPP:<hex>) connect it
void cppLookAgain()
{
	if (s_state == CPP_NOT_FOUND)
		s_state = CPP_SEARCHING;
}

void cppUpdate(bool enabled)
{
	if (!enabled && s_state != CPP_OFF)
	{
		letGo();
		s_state = CPP_OFF;
	}
	else if (enabled && s_state == CPP_OFF)
		s_state = CPP_SEARCHING;
	s_down = s_held & ~s_lastHeld;
	s_up = s_lastHeld & ~s_held;
	s_lastHeld = s_held;
}

void cppShutdown()
{
	letGo();
	s_state = CPP_OFF;
}
#endif
