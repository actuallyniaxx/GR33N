#ifndef FALSO_MUTEX_H
#define FALSO_MUTEX_H
#include <stdint.h>
typedef uint64_t sys_mutex_t;
typedef struct { int dummy; } sys_mutex_attr_t;
#define sysMutexAttrInitialize(a) do { (a).dummy = 0; } while (0)
int sysMutexCreate(sys_mutex_t *m, sys_mutex_attr_t *a);
int sysMutexDestroy(sys_mutex_t m);
int sysMutexLock(sys_mutex_t m, unsigned timeout);
int sysMutexUnlock(sys_mutex_t m);
#endif
