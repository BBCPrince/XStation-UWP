#pragma once
#include <stdatomic.h>
#include <stdio.h>
#include <windows.h>
static _Atomic int bb_ifast_mode;
static _Atomic unsigned long long bb_ifast_next,bb_ifast_draws,bb_ifast_dispatches,bb_ifast_slabs;
static atomic_flag bb_ifast_lock=ATOMIC_FLAG_INIT;
static unsigned long long bb_ifast_nonce,bb_ifast_expiry;
static int bb_ifast_default,bb_ifast_default_read;
static void bb_ifast_control(void){
 const unsigned long long now=GetTickCount64();if(now<atomic_load(&bb_ifast_next)||atomic_flag_test_and_set(&bb_ifast_lock))return;
 atomic_store(&bb_ifast_next,now+250);
 if(bb_ifast_expiry&&now>=bb_ifast_expiry){atomic_store(&bb_ifast_mode,bb_ifast_default);bb_ifast_expiry=0;}
 char folder[32768],path[32768];const DWORD n=GetEnvironmentVariableA("BB_GPU_USER_DIR",folder,sizeof(folder)-128);
 if(!n||n>=sizeof(folder)-128){atomic_flag_clear(&bb_ifast_lock);return;}
 if(!bb_ifast_default_read){
  snprintf(path,sizeof(path),"%s\\indirect-fastpath-default.txt",folder);FILE* defaults=fopen(path,"r");int value=0;
  if(defaults){if(fscanf(defaults,"%d",&value)==1&&value==1)bb_ifast_default=1;fclose(defaults);}
  bb_ifast_default_read=1;
 }
 snprintf(path,sizeof(path),"%s\\indirect-fastpath.request",folder);FILE* f=fopen(path,"r");
 if(!f){atomic_store(&bb_ifast_mode,bb_ifast_default);bb_ifast_expiry=0;atomic_flag_clear(&bb_ifast_lock);return;}
 unsigned long long nonce=0;unsigned mode=0,duration=0;const int fields=fscanf(f,"%llu %u %u",&nonce,&mode,&duration);fclose(f);
 if(fields==3&&nonce&&nonce!=bb_ifast_nonce&&mode<=1&&duration>=250&&duration<=180000){
  bb_ifast_nonce=nonce;atomic_store(&bb_ifast_mode,mode);bb_ifast_expiry=mode?now+duration:0;
  snprintf(path,sizeof(path),"%s\\indirect-fastpath-state.txt",folder);f=fopen(path,"w");
  if(f){fprintf(f,"nonce=%llu mode=%u tick=%llu draws=%llu dispatches=%llu slabs=%llu\n",nonce,mode,now,atomic_load(&bb_ifast_draws),atomic_load(&bb_ifast_dispatches),atomic_load(&bb_ifast_slabs));fclose(f);}
 }
 atomic_flag_clear(&bb_ifast_lock);
}

