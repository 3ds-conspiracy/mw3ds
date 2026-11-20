#pragma once

#include <3ds.h>

// The Circle Pad Pro on an Old 3DS (a New 3DS has its C-stick, ZL and ZR built in, read through ir:rst, and needs none
// of this). As in the games made for it, the player turns it on (Options > Controls > Pad Pro, g_circlePadPro): only
// then does the game open ir:USER and look for it, for a few seconds, and again only when asked (Look again), when the
// link is lost (standby after 5 minutes unused: ZL or ZR wakes it) or after the HOME menu and sleep. Turned off, the
// infrared is let go. Once linked its stick stands in for the C-stick (cstickRead) and its ZL, ZR and R join the
// console's buttons (cppKeys*), in every layout.

enum CppState
{
	CPP_OFF,                      // the setting off, or a New 3DS: nothing opened
	CPP_SEARCHING,                // looking for it (a few seconds)
	CPP_CALIBRATING,              // found: its calibration read, then its reports asked for
	CPP_CONNECTED,                // reports coming
	CPP_NOT_FOUND,                // the last look found nothing (or it could not be opened): it waits for Look again
};

void cppUpdate(bool enabled);     // each frame on an Old 3DS: the setting, the link kept, the reports read
void cppLookAgain();              // Options: look for it again now
void cppShutdown();               // the game's exit
CppState cppState();
int cppBattery();                 // its battery level as it reports it, 0 to 31 (-1 before a report)
bool cppConnected();
u32 cppKeysHeld();                // KEY_ZL / KEY_ZR / KEY_R held on the accessory
u32 cppKeysDown();                // ... pressed since the last frame
u32 cppKeysUp();                  // ... let go since the last frame

// The second stick: the accessory's while it is linked, else the console's (hidCstickRead: a New 3DS's C-stick)
void cstickRead(circlePosition* cs);

// One packet as ir:USER delivers it (0xA5, network id, size, payload, CRC-8): an input report (0x10) or the calibration
// (0x11). False when dropped. Scripted runs feed it too (CPP:<hex>), CPP:off loses the link
bool cppTakePacket(const u8* data, u32 size);
void cppTestLinkLost();
