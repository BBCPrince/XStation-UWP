#pragma once
#include <stdint.h>
struct TlouXboxProfileCounter { uint64_t segments,wall_ns,cycles,max_ns; };
struct TlouXboxProfileSite { char name[160]; TlouXboxProfileCounter counter; };
struct TlouXboxProfileSnapshot {
 uint32_t version,bytes,lanes,phases;
 uint64_t tick_ms,cycle_failures,site_overflow;
 uint64_t seen[3],selected[3],root_ns[3];
 TlouXboxProfileCounter time[3][21];
 TlouXboxProfileSite sites[128];
};
