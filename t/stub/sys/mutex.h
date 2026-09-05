#pragma once
#include <ppu-types.h>
typedef u64 sys_mutex_t;
typedef struct { int x; } sys_mutex_attr_t;
void sysMutexAttrInitialize(sys_mutex_attr_t);
int sysMutexCreate(sys_mutex_t*, sys_mutex_attr_t*);
int sysMutexLock(sys_mutex_t, u32);
int sysMutexUnlock(sys_mutex_t);
