
#pragma once
#include <stdio.h>
static void private_native_log313(const char *line)
{
#ifdef _WIN32
   char path[32768];
   const DWORD n=GetEnvironmentVariableA("BB_GPU_USER_DIR",path,sizeof(path)-80);
   if(!n || n>=sizeof(path)-80)return;
   snprintf(path+n,sizeof(path)-n,"\\native-draw-timing-313.log");
   FILE *f=fopen(path,"ab");
   if(f){fputs(line,f);fclose(f);}
#endif
}
#define PRIVATE_NATIVE_LOG313(...) do { char line[384]; \
   snprintf(line,sizeof(line),__VA_ARGS__);private_native_log313(line); \
} while(0)
