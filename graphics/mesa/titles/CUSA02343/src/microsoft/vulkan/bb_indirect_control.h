#pragma once
/* From Bloodborne's Xbox driver (2026-10-26): single indirect draws and dispatches that need draw
 * constants get them by GPU buffer copies, not a compute rewrite. On unless BB_DZN_INDIRECT_FAST=0
 * (read through GetEnvironmentVariableA: Dozen's msvcrt has its own environment copy). */
#include <stdatomic.h>
#include <windows.h>
static _Atomic int bb_ifast_mode;
static _Atomic unsigned long long bb_ifast_draws, bb_ifast_dispatches, bb_ifast_slabs;
static INIT_ONCE bb_ifast_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK
bb_ifast_read(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
   char value[8];
   const DWORD n = GetEnvironmentVariableA("BB_DZN_INDIRECT_FAST", value, sizeof(value));
   atomic_store(&bb_ifast_mode, !(n && n < sizeof(value) && value[0] == '0'));
   return TRUE;
}
static void
bb_ifast_control(void)
{
   InitOnceExecuteOnce(&bb_ifast_once, bb_ifast_read, NULL, NULL);
}
