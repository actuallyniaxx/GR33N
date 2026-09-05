#pragma once
#include <ppu-types.h>
typedef u64 sys_ppu_thread_t;
#define THREAD_JOINABLE 0
int sysThreadCreate(sys_ppu_thread_t*, void(*)(void*), void*, int, size_t, int, const char*);
int sysThreadJoin(sys_ppu_thread_t, u64*);
