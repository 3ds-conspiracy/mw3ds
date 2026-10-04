// Host stand-in for libctru's types: the native test build (tools/test/native)
#pragma once
#include <cstdint>
#include <cstddef>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t s8; typedef int16_t s16; typedef int32_t s32; typedef int64_t s64;
typedef volatile u8 vu8; typedef volatile u32 vu32;
typedef int32_t Result;
#define R_SUCCEEDED(r) ((r) >= 0)
#define R_FAILED(r) ((r) < 0)
