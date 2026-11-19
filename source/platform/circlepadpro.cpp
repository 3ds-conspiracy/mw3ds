// The Circle Pad Pro over ir:USER (an Old 3DS): the link, its input reports, and their place among the buttons.
// The protocol, as 3dbrew's IR pages describe it:
// - ir:USER takes a shared memory block (InitializeIrNopShared). Its first 0x10 bytes are the link's status (byte 0x08:
//   2 once connected), the next 0x10 the receive buffer's ring (first packet, last, count), then one 8-byte entry per
//   packet slot (offset into the data, size) and the packet data, a ring that wraps
// - RequireConnection(1) asks for the accessory; once connected the game sends it one request (SendIrNop: 0x01, the
//   polling period in ms, a timing byte) and it answers with an input report every period
// - each packet is 0xA5, the network id, the payload's size (one byte, or two when the first has 0x40 set), the
//   payload, and a CRC-8 (polynomial 0x07) of everything before it. ReleaseReceivedData frees the packets read
// - an input report is 0x10, the stick's X and Y as two 12-bit numbers (X in the low bits), a byte of battery level
//   (bits 0-4) and ZL / ZR / R (bits 5 / 6 / 7, set while the button is NOT held), and a spare byte
#include "platform/circlepadpro.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include "components/debug/debuglog.hpp"

static bool s_linked = false;          // reports arrive (on the console: ir:USER connected; a scripted run: CPP:<hex>)
static u32 s_held = 0, s_lastHeld = 0, s_down = 0, s_up = 0;
static int s_stickX = 0, s_stickY = 0; // the stick in the console's stick counts (about +-150 at the rim)
static int s_centreX = -1, s_centreY = -1;
static int s_reach = 0x500;            // the farthest the stick has gone from its centre (it only grows: see stickFrom)
static int s_badSums = 0, s_goodSums = 0;

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

// The stick's 12-bit reading to the console's stick counts. The first report after linking is taken as the centre
// (the stick at rest, when near the middle); the reach grows to the farthest push seen, so a stick that reads short of
// the 12-bit ends still gets full speed at its rim
static void stickFrom(int x, int y)
{
	if (s_centreX < 0)
	{
		s_centreX = abs(x - 0x800) < 0x300 ? x : 0x800;
		s_centreY = abs(y - 0x800) < 0x300 ? y : 0x800;
	}
	int dx = x - s_centreX, dy = y - s_centreY;
	s_reach = std::max(s_reach, std::max(abs(dx), abs(dy)));
	s_stickX = dx * 150 / s_reach;
	s_stickY = dy * 150 / s_reach;
}

static void letGo()
{
	s_linked = false;
	s_held = 0;
	s_stickX = s_stickY = 0;
	s_centreX = s_centreY = -1;
}

bool cppTakePacket(const u8* p, u32 size)
{
	if (size < 4 || p[0] != 0xA5)
		return false;
	u32 head = 3, n = p[2];
	if (p[2] & 0x40)
	{
		head = 4;
		n = ((p[2] & 0x3F) << 8) | p[3];
	}
	if (head + n + 1 > size || n < 5)
		return false;
	if (crc8(p, head + n) != p[head + n])
	{
		if (s_badSums++ == 0)
			logf("cpp: a packet's checksum differs (%u bytes), dropped", (unsigned)size);
		return false;
	}
	if (s_goodSums++ == 0)
		logf("cpp: first input report");
	const u8* r = p + head;
	if (r[0] != 0x10)
		return false;
	int x = r[1] | ((r[2] & 0x0F) << 8);
	int y = (r[2] >> 4) | (r[3] << 4);
	s_linked = true;
	stickFrom(x, y);
	s_held = ((r[4] & 0x20) ? 0 : KEY_ZL) | ((r[4] & 0x40) ? 0 : KEY_ZR) | ((r[4] & 0x80) ? 0 : KEY_R);
	return true;
}

void cppTestLinkLost()
{
	letGo();
	logf("cpp: link lost");
}

u32 cppKeysHeld() { return s_held; }
u32 cppKeysDown() { return s_down; }
u32 cppKeysUp() { return s_up; }
bool cppConnected() { return s_linked; }

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

// The service's buffers: room for 32 packets received (each an 8-byte slot and up to 32 bytes) and 8 sent, at the
// 115200 baud the accessory talks at (bit rate 4)
static const u32 kShared = 0x1000, kRecv = 0x800, kRecvPackets = 32, kSend = 0x200, kSendPackets = 8;
static const u32 kBitRate = 4, kDeviceCirclePadPro = 1;
static const u8 kPollMs = 8;
static const float kTryFor = 3.0f, kWaitBetween = 5.0f, kQuietLost = 1.0f;

enum { LINK_CLOSED, LINK_WAIT, LINK_TRYING, LINK_UP };
static Handle s_ir = 0, s_block = 0;
static u8* s_mem = nullptr;
static bool s_irOn = false;            // InitializeIrNopShared done
static int s_state = LINK_CLOSED;
static u64 s_since = 0, s_lastReport = 0;
static bool s_triedOnce = false;
static aptHookCookie s_hook;

static float secondsSince(u64 tick)
{
	return (float)(svcGetSystemTick() - tick) / SYSCLOCK_ARM11;
}

static Result irCall(u32 header, u32 arg = 0)
{
	u32* cmd = getThreadCommandBuffer();
	cmd[0] = header;
	cmd[1] = arg;
	Result r = svcSendSyncRequest(s_ir);
	return R_FAILED(r) ? r : (Result)cmd[1];
}

static bool irOpen()
{
	if (s_irOn)
		return true;
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
	Result r = svcSendSyncRequest(s_ir);
	if (R_SUCCEEDED(r))
		r = (Result)cmd[1];
	if (R_FAILED(r))
	{
		logf("cpp: ir:USER refused its buffers (0x%08lx)", (unsigned long)r);
		return false;
	}
	s_irOn = true;
	return true;
}

static void irClose()
{
	if (!s_irOn)
		return;
	irCall(IPC_MakeHeader(0x9, 0, 0));         // Disconnect
	irCall(IPC_MakeHeader(0x2, 0, 0));         // FinalizeIrNop
	s_irOn = false;
}

static u32 rd32(u32 at)
{
	u32 v;
	memcpy(&v, (const void*)(s_mem + at), 4);
	return v;
}

// The packets in the receive ring, oldest first, then freed
static void irReceive()
{
	u32 first = rd32(0x10), count = rd32(0x18);
	if (count == 0)
		return;
	count = std::min(count, kRecvPackets);
	const u32 slots = 0x20, data = 0x20 + kRecvPackets * 8, ring = kRecv - kRecvPackets * 8;
	u8 buf[64];
	for (u32 i = 0; i < count; i++)
	{
		u32 slot = (first + i) % kRecvPackets;
		u32 off = rd32(slots + slot * 8), size = rd32(slots + slot * 8 + 4);
		if (size == 0 || size > sizeof(buf))
			continue;
		for (u32 j = 0; j < size; j++)
			buf[j] = s_mem[data + (off + j) % ring];
		if (cppTakePacket(buf, size))
			s_lastReport = svcGetSystemTick();
	}
	irCall(IPC_MakeHeader(0x19, 1, 0), count);  // ReleaseReceivedData
}

static void setState(int st)
{
	s_state = st;
	s_since = svcGetSystemTick();
}

// The HOME menu or the lid shut: the link and the service's buffers let go, looked for again once back
static void onApt(APT_HookType type, void*)
{
	if (s_state == LINK_CLOSED)
		return;
	if (type == APTHOOK_ONSUSPEND || type == APTHOOK_ONSLEEP)
	{
		if (s_linked)
			logf("cpp: link let go for the HOME menu / sleep");
		letGo();
		irClose();
		setState(LINK_WAIT);
	}
	else if (type == APTHOOK_ONRESTORE || type == APTHOOK_ONWAKEUP)
		setState(LINK_WAIT);                    // (back in a second: the wait below counts from here)
}

void cppStart()
{
	if (s_state != LINK_CLOSED)
		return;
	Result r = srvGetServiceHandle(&s_ir, "ir:USER");
	if (R_FAILED(r))
	{
		logf("cpp: no ir:USER (0x%08lx): no Circle Pad Pro", (unsigned long)r);
		return;
	}
	s_mem = (u8*)memalign(0x1000, kShared);
	if (!s_mem)
	{
		svcCloseHandle(s_ir);
		return;
	}
	memset(s_mem, 0, kShared);
	r = svcCreateMemoryBlock(&s_block, (u32)s_mem, kShared, MEMPERM_READ, MEMPERM_READWRITE);
	if (R_FAILED(r))
	{
		logf("cpp: no shared memory for ir:USER (0x%08lx)", (unsigned long)r);
		free(s_mem);
		s_mem = nullptr;
		svcCloseHandle(s_ir);
		return;
	}
	aptHook(&s_hook, onApt, nullptr);
	logf("cpp: ir:USER open: looking for a Circle Pad Pro");
	// (the first look straight away, the next ones kWaitBetween apart)
	s_state = LINK_WAIT;
	s_since = svcGetSystemTick() - (u64)(kWaitBetween * SYSCLOCK_ARM11);
}

void cppStop()
{
	if (s_state == LINK_CLOSED)
		return;
	aptUnhook(&s_hook);
	letGo();
	irClose();
	svcCloseHandle(s_block);
	free(s_mem);
	s_mem = nullptr;
	svcCloseHandle(s_ir);
	s_state = LINK_CLOSED;
}

static void cppLink()
{
	if (s_state == LINK_WAIT && secondsSince(s_since) >= kWaitBetween)
	{
		if (!irOpen())
		{
			setState(LINK_WAIT);
			return;
		}
		Result r = irCall(IPC_MakeHeader(0x6, 1, 0), kDeviceCirclePadPro);   // RequireConnection
		if (R_FAILED(r))
		{
			if (!s_triedOnce)
				logf("cpp: ir:USER refused to look for it (0x%08lx)", (unsigned long)r);
			s_triedOnce = true;
			setState(LINK_WAIT);
			return;
		}
		s_triedOnce = true;
		setState(LINK_TRYING);
	}
	else if (s_state == LINK_TRYING)
	{
		if (s_mem[0x08] == 2)
		{
			// Connected: ask for a report every kPollMs (the third byte, (period + 2) * 4, as the accessory expects)
			static u8 req[3] = { 0x01, kPollMs, (u8)((kPollMs + 2) << 2) };
			u32* cmd = getThreadCommandBuffer();
			cmd[0] = IPC_MakeHeader(0xD, 1, 2);    // SendIrNop
			cmd[1] = sizeof(req);
			cmd[2] = IPC_Desc_StaticBuffer(sizeof(req), 0);
			cmd[3] = (u32)req;
			Result r = svcSendSyncRequest(s_ir);
			if (R_SUCCEEDED(r))
				r = (Result)cmd[1];
			logf("cpp: Circle Pad Pro found, reports asked for (0x%08lx)", (unsigned long)r);
			s_lastReport = svcGetSystemTick();
			setState(LINK_UP);
		}
		else if (secondsSince(s_since) > kTryFor)
		{
			irCall(IPC_MakeHeader(0x9, 0, 0));    // Disconnect: none in reach; looked for again later
			setState(LINK_WAIT);
		}
	}
	else if (s_state == LINK_UP)
	{
		irReceive();
		// (taken off or out of reach: the status drops, or the reports stop)
		if (s_mem[0x08] != 2 || secondsSince(s_lastReport) > kQuietLost)
		{
			logf("cpp: link lost");
			letGo();
			irCall(IPC_MakeHeader(0x9, 0, 0));    // Disconnect
			setState(LINK_WAIT);
		}
	}
}
#else
void cppStart() {}
void cppStop() {}
static void cppLink() {}
#endif

void cppUpdate()
{
	cppLink();
	s_down = s_held & ~s_lastHeld;
	s_up = s_lastHeld & ~s_held;
	s_lastHeld = s_held;
}
