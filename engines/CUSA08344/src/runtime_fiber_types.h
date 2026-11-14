#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>
using u8=uint8_t;
using s8=int8_t;
using u16=uint16_t;
using u32=uint32_t;
using s32=int32_t;
using u64=uint64_t;
#define PS4_SYSV_ABI __attribute__((sysv_abi))
constexpr s32 ORBIS_OK=0;
#define LOG_WARNING(category,...) std::fprintf(stderr,"Fiber: %s\n",fmt::format(__VA_ARGS__).c_str())
#define UNREACHABLE_MSG(...) do { std::fprintf(stderr,"Fiber fatal: %s\n",fmt::format(__VA_ARGS__).c_str()); std::abort(); } while (0)
