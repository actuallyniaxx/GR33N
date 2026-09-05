#ifndef FALSO_THREAD_H
#define FALSO_THREAD_H
#include <stdint.h>
typedef uint64_t sys_ppu_thread_t;
#define THREAD_JOINABLE 0
int  sysThreadCreate(sys_ppu_thread_t *id, void (*entry)(void*), void *arg,
                     int prio, size_t stack, int flags, const char *name);
int  sysThreadJoin(sys_ppu_thread_t id, uint64_t *ret);
void sysThreadExit(int code);
#endif
