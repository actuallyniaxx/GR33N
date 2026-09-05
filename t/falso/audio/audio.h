/* Falsa, copiada de la de verdad que volco deps/sonda-audio.sh. Los
 * numeros y los nombres son los que dijo la consola, no los que yo creia. */
#ifndef FALSO_AUDIO_H
#define FALSO_AUDIO_H
#include <stdint.h>
#include <sys/event_queue.h>

#define AUDIO_BLOCK_SAMPLES   256
#define AUDIO_STATUS_READY    1
#define AUDIO_STATUS_RUN      2
#define AUDIO_STATUS_CLOSE    0x1010
#define AUDIO_PORT_2CH        2
#define AUDIO_PORT_8CH        8
#define AUDIO_PORT_INITLEVEL  0x1000
#define AUDIO_BLOCK_8         8
#define AUDIO_BLOCK_16        16
#define AUDIO_BLOCK_32        32

typedef struct _audio_port_param {
	uint64_t numChannels;
	uint64_t numBlocks;
	uint64_t attrib;
	float    level;
} audioPortParam;

typedef struct _audio_port_config {
	uint32_t readIndex;
	uint32_t status;
	uint64_t channelCount;
	uint64_t numBlocks;
	uint32_t portSize;
	uint32_t audioDataStart;
} audioPortConfig;

int32_t audioInit(void);
int32_t audioQuit(void);
int32_t audioPortOpen(audioPortParam *param, uint32_t *portNum);
int32_t audioPortStart(uint32_t portNum);
int32_t audioPortStop(uint32_t portNum);
int32_t audioGetPortConfig(uint32_t portNum, audioPortConfig *config);
int32_t audioPortClose(uint32_t portNum);
int32_t audioCreateNotifyEventQueue(sys_event_queue_t *q, sys_ipc_key_t *k);
int32_t audioSetNotifyEventQueue(sys_ipc_key_t k);
int32_t audioRemoveNotifyEventQueue(sys_ipc_key_t k);
#endif
