#pragma once

#include <cstring>
#include <vector>

#include <3ds.h>

// Cell format 7 stores arrays of fixed-size records as byte planes (byte 0 of every record, then
// byte 1, ...) after replacing chosen integer fields by their difference from the previous record:
// smooth geometry compresses about a third better that way. These undo it in place after reading.

// Planes of `count` records of `size` bytes back into records
inline void unplane(u8* data, u32 count, u32 size)
{
	std::vector<u8> tmp(data, data + count * size);
	for (u32 b = 0; b < size; b++)
	{
		const u8* src = &tmp[b * count];
		for (u32 i = 0; i < count; i++)
			data[i * size + b] = src[i];
	}
}

// Running sums of the 16-bit field at `offset` in each record (wrapping, as it was stored)
inline void undelta16(u8* data, u32 count, u32 size, u32 offset)
{
	u16 prev = 0;
	for (u32 i = 0; i < count; i++)
	{
		u16 v;
		memcpy(&v, data + i * size + offset, 2);
		v = (u16)(v + prev);
		memcpy(data + i * size + offset, &v, 2);
		prev = v;
	}
}

inline void undelta32(u8* data, u32 count, u32 size, u32 offset)
{
	u32 prev = 0;
	for (u32 i = 0; i < count; i++)
	{
		u32 v;
		memcpy(&v, data + i * size + offset, 4);
		v += prev;
		memcpy(data + i * size + offset, &v, 4);
		prev = v;
	}
}

// An array of `count` integers of `width` bytes (2 or 4), delta coded then split into planes
inline void unpackInts(u8* data, u32 count, u32 width)
{
	unplane(data, count, width);
	if (width == 2)
		undelta16(data, count, 2, 0);
	else
		undelta32(data, count, 4, 0);
}
