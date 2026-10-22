#pragma once

#include <math.h>
#include <stdint.h>

// Global variables (OpenMW's ESM::Global)

struct GlobalDef { char type; float value; };

// A short or long global's stored float (FLTV) as OpenMW's ESM::Variant reads it: cut to a whole number; one out of an
// int's range (or not a number) is the int's lowest, which as a short is 0. Tribunal.esm's MuseumDonations holds
// leftover bytes that read as 7.6e23: 0, not "two donations made" from the start. A float global is kept as it is
inline float globalStoredValue(char type, float v)
{
	if (type != 's' && type != 'l')
		return v;
	int32_t i = (isnan(v) || v < -2147483648.0f || v > 2147483520.0f) ? INT32_MIN : (int32_t)v;
	return type == 's' ? (float)(int16_t)i : (float)i;
}
