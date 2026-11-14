// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// ABI layouts adapted to C from shadPS4 videodec.h.
#pragma once
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;


typedef struct OrbisVideodecConfigInfo {
    u64 thisSize;
    u32 codecType;
    u32 profile;
    u32 maxLevel;
    s32 maxFrameWidth;
    s32 maxFrameHeight;
    s32 maxDpbFrameCount;
    u64 videodecFlags;
} OrbisVideodecConfigInfo;

typedef struct OrbisVideodecResourceInfo {
    u64 thisSize;
    u64 cpuMemorySize;
    void* pCpuMemory;
    u64 cpuGpuMemorySize;
    void* pCpuGpuMemory;
    u64 maxFrameBufferSize;
    u32 frameBufferAlignment;
} OrbisVideodecResourceInfo;

typedef struct OrbisVideodecCtrl {
    u64 thisSize;
    void* handle;
    u64 version;
} OrbisVideodecCtrl;

typedef struct OrbisVideodecFrameBuffer {
    u64 thisSize;
    void* pFrameBuffer;
    u64 frameBufferSize;
} OrbisVideodecFrameBuffer;

typedef struct OrbisVideodecAvcInfo {
    u32 numUnitsInTick;
    u32 timeScale;
    u8 fixedFrameRateFlag;
    u8 aspectRatioIdc;
    u16 sarWidth;
    u16 sarHeight;
    u8 colourPrimaries;
    u8 transferCharacteristics;
    u8 matrixCoefficients;
    u8 videoFullRangeFlag;
    u32 frameCropLeftOffset;
    u32 frameCropRightOffset;
    u32 frameCropTopOffset;
    u32 frameCropBottomOffset;
} OrbisVideodecAvcInfo;

typedef union OrbisVideodecCodecInfo {
    u8 reserved[64];
    OrbisVideodecAvcInfo avc;
} OrbisVideodecCodecInfo;

typedef struct OrbisVideodecPictureInfo {
    u64 thisSize;
    u32 isValid;
    u32 codecType;
    u32 frameWidth;
    u32 framePitch;
    u32 frameHeight;
    u32 isErrorPic;
    u64 ptsData;
    u64 attachedData;
    OrbisVideodecCodecInfo codec;
} OrbisVideodecPictureInfo;

typedef struct OrbisVideodecInputData {
    u64 thisSize;
    void* pAuData;
    u64 auSize;
    u64 ptsData;
    u64 dtsData;
    u64 attachedData;
} OrbisVideodecInputData;


#define ORBIS_VIDEODEC_ERROR_API_FAIL UINT32_C(0x80C10000)
#define ORBIS_VIDEODEC_ERROR_CODEC_TYPE UINT32_C(0x80C10001)
#define ORBIS_VIDEODEC_ERROR_STRUCT_SIZE UINT32_C(0x80C10002)
#define ORBIS_VIDEODEC_ERROR_HANDLE UINT32_C(0x80C10003)
#define ORBIS_VIDEODEC_ERROR_CPU_MEMORY_SIZE UINT32_C(0x80C10004)
#define ORBIS_VIDEODEC_ERROR_CPU_MEMORY_POINTER UINT32_C(0x80C10005)
#define ORBIS_VIDEODEC_ERROR_CPU_GPU_MEMORY_SIZE UINT32_C(0x80C10006)
#define ORBIS_VIDEODEC_ERROR_CPU_GPU_MEMORY_POINTER UINT32_C(0x80C10007)
#define ORBIS_VIDEODEC_ERROR_SHADER_CONTEXT_POINTER UINT32_C(0x80C10008)
#define ORBIS_VIDEODEC_ERROR_AU_SIZE UINT32_C(0x80C10009)
#define ORBIS_VIDEODEC_ERROR_AU_POINTER UINT32_C(0x80C1000A)
#define ORBIS_VIDEODEC_ERROR_FRAME_BUFFER_SIZE UINT32_C(0x80C1000B)
#define ORBIS_VIDEODEC_ERROR_FRAME_BUFFER_POINTER UINT32_C(0x80C1000C)
#define ORBIS_VIDEODEC_ERROR_FRAME_BUFFER_ALIGNMENT UINT32_C(0x80C1000D)
#define ORBIS_VIDEODEC_ERROR_CONFIG_INFO UINT32_C(0x80C1000E)
#define ORBIS_VIDEODEC_ERROR_ARGUMENT_POINTER UINT32_C(0x80C1000F)
#define ORBIS_VIDEODEC_ERROR_NEW_SEQUENCE UINT32_C(0x80C10010)
#define ORBIS_VIDEODEC_ERROR_DECODE_AU UINT32_C(0x80C10011)
#define ORBIS_VIDEODEC_ERROR_MISMATCH_SPEC UINT32_C(0x80C10012)
#define ORBIS_VIDEODEC_ERROR_INVALID_SEQUENCE UINT32_C(0x80C10013)
#define ORBIS_VIDEODEC_ERROR_FATAL_STREAM UINT32_C(0x80C10014)
#define ORBIS_VIDEODEC_ERROR_FATAL_STATE UINT32_C(0x80C10015)
