#pragma once
#include <stdlib.h>
#include <errno.h>
#include <string.h>

// Retain the established two-core profile. The four-core A17 default in build
// 19 regressed the user's frame pacing without isolating its cause; workers remain an explicit
// comparison option until whole-game measurements justify a new default.
static inline unsigned pg_engine_default_cores(const char *model,unsigned hardwareCores) {
 (void)model; (void)hardwareCores;
 return 2;
}
static inline unsigned pg_engine_core_configuration(const char *text,const char *model,unsigned hardwareCores) {
 unsigned fallback=pg_engine_default_cores(model,hardwareCores);
 if(!text||!*text)return fallback;
 char *end;errno=0;long value=strtol(text,&end,10);
 if(errno||end==text||*end||value<1||value>8)return fallback;
 return (unsigned)value;
}
