#ifndef FALSO_EVQ_H
#define FALSO_EVQ_H
#include <stdint.h>
typedef uint32_t sys_event_queue_t;
typedef uint64_t sys_ipc_key_t;
typedef struct sys_event { uint64_t source, data1, data2, data3; } sys_event_t;
int32_t sysEventQueueReceive(sys_event_queue_t q, sys_event_t *e, uint64_t us);
int32_t sysEventQueueDrain(sys_event_queue_t q);
int32_t sysEventQueueDestroy(sys_event_queue_t q, int32_t mode);
#endif
