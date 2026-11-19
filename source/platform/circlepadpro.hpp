#pragma once

#include <3ds.h>

// The Circle Pad Pro on an Old 3DS. It talks to the console over infrared through the ir:USER service (a New 3DS has
// its C-stick, ZL and ZR built in, read through ir:rst, and needs none of this). On an Old 3DS the game asks ir:USER
// for the accessory every few seconds; once linked, the accessory sends an input report every few milliseconds: its
// stick and its ZL, ZR and R. Those join the console's own buttons (cppKeys*) and stand in for the C-stick
// (cstickRead) while the link holds. Lost (taken off, out of reach, the HOME menu), it lets go and is looked for again.

void cppStart();                  // an Old 3DS: open ir:USER and start looking for the accessory
void cppStop();                   // close it (the game's exit)
void cppUpdate();                 // each frame: the reports received, the link kept or looked for again
u32 cppKeysHeld();                // KEY_ZL / KEY_ZR / KEY_R held on the accessory
u32 cppKeysDown();                // ... pressed since the last frame
u32 cppKeysUp();                  // ... let go since the last frame
bool cppConnected();

// The second stick: the accessory's while it is linked, else the console's (hidCstickRead: a New 3DS's C-stick)
void cstickRead(circlePosition* cs);

// One packet as ir:USER delivers it (0xA5, network id, size, payload, CRC-8): an input report updates the state;
// anything else, or a wrong checksum, is dropped. False when dropped. Scripted runs feed it too (CPP:<hex>)
bool cppTakePacket(const u8* data, u32 size);
void cppTestLinkLost();           // a scripted run: the link lost (CPP:off)
