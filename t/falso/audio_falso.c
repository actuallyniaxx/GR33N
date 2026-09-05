/* El puerto de audio de la PS3, de mentira, y un Opus de mentira.
 *
 * El puerto NO va en tiempo real: la prueba le da un empujon por bloque
 * con audio_tick(). Un test que tarde lo que tarda el sonido no lo corre
 * nadie dos veces, y ademas el fallo dependeria del reloj.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <sys/mman.h>

#include "falso/audio/audio.h"
#include "falso/opus/opus.h"

/* --- el buffer del puerto -------------------------------------------- */

#define BLOQUES  8
#define CANALES  2

/* POR DEBAJO DE LOS 4 GB, igual que la lista de direcciones en
 * plataforma.c y por lo mismo: audioPortConfig.audioDataStart es un u32
 * -- el ABI de lv2, donde todo cabe en 32 bits-- y aud.c hace el cast que
 * corresponde a eso. En un PC de 64 bits un buffer estatico esta muy por
 * encima, y el cast se lleva media direccion.
 *
 * Cambiar aud.c para que usara un puntero ancho habria hecho pasar la
 * prueba y roto la consola. Se arregla en la mentira, no en la verdad. */
#define BUF_PARES (BLOQUES * AUDIO_BLOCK_SAMPLES * CANALES)
static float *buffer = NULL;
static float ultimo[AUDIO_BLOCK_SAMPLES * CANALES];
static uint32_t escritos = 0;
static int abierto = 0;
static int arrancado = 0;
/* Cuantos avisos se han soltado. Hace falta porque el bloque que aud.c
 * acaba de escribir NO se puede mirar en el mismo aviso que lo provoco:
 * aud.c escribe DESPUES de que sysEventQueueReceive vuelva.
 *
 * La primera version copiaba el bloque dentro del receive, o sea antes de
 * que aud.c lo escribiera, y "lo ultimo que ha sonado" era lo que hubiera
 * ahi una vuelta entera antes -- ceros. Siete comprobaciones en rojo por
 * un fallo de la mentira, no de la verdad. */
static uint32_t recibidos = 0;

static sem_t avisos;
static int   avisos_ok = 0;
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;

int audio_abierto(void) { return abierto; }

uint32_t audio_bloques_escritos(void)
{
	uint32_t v;
	pthread_mutex_lock(&m);
	v = escritos;
	pthread_mutex_unlock(&m);
	return v;
}

float *audio_ultimo_bloque(void) { return ultimo; }

/* Suelta un aviso: "he consumido un bloque, escribe el siguiente". */
void audio_tick(void)
{
	if (avisos_ok) sem_post(&avisos);
}

/* --- la API ---------------------------------------------------------- */

int32_t audioInit(void) { return 0; }
int32_t audioQuit(void) { return 0; }

int32_t audioPortOpen(audioPortParam *p, uint32_t *portNum)
{
	if (p->numChannels != AUDIO_PORT_2CH) return -1;

	if (buffer == NULL) {
		buffer = mmap(NULL, 4096 * 16, PROT_READ | PROT_WRITE,
		              MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
		if (buffer == MAP_FAILED || (uintptr_t)buffer > 0xffffffffu) {
			fprintf(stderr, "!! no hay memoria por debajo de 4 GB: esta "
			                "prueba no puede imitar el ABI de lv2\n");
			exit(2);
		}
	}

	memset(buffer, 0, BUF_PARES * sizeof(float));
	memset(ultimo, 0, sizeof(ultimo));
	escritos = 0;
	recibidos = 0;
	abierto = 1;
	*portNum = 7;
	return 0;
}

int32_t audioGetPortConfig(uint32_t portNum, audioPortConfig *c)
{
	(void)portNum;
	memset(c, 0, sizeof(*c));
	c->status         = AUDIO_STATUS_READY;
	c->channelCount   = CANALES;
	c->numBlocks      = BLOQUES;
	c->portSize       = BUF_PARES * sizeof(float);
	/* En la consola esto es una direccion de 32 bits. Aqui el puntero es
	 * de 64, asi que el campo se queda corto... y por eso aud.c hace
	 * (f32*)(u64)cfg.audioDataStart. Para que el cast siga siendo valido,
	 * el buffer tiene que vivir por debajo de los 4 GB -- igual que en
	 * t/falso/plataforma.c con net_hostent. */
	c->audioDataStart = (uint32_t)(uintptr_t)buffer;
	return 0;
}

int32_t audioPortStart(uint32_t p) { (void)p; arrancado = 1; return 0; }
int32_t audioPortStop(uint32_t p)  { (void)p; arrancado = 0; return 0; }

int32_t audioPortClose(uint32_t p)
{
	(void)p;
	abierto = 0;
	return 0;
}

int32_t audioCreateNotifyEventQueue(sys_event_queue_t *q, sys_ipc_key_t *k)
{
	sem_init(&avisos, 0, 0);
	avisos_ok = 1;
	*q = 1;
	*k = 1;
	return 0;
}
int32_t audioSetNotifyEventQueue(sys_ipc_key_t k)    { (void)k; return 0; }
int32_t audioRemoveNotifyEventQueue(sys_ipc_key_t k) { (void)k; return 0; }

int32_t sysEventQueueReceive(sys_event_queue_t q, sys_event_t *e, uint64_t us)
{
	struct timespec ts;
	int r;

	(void)q;
	memset(e, 0, sizeof(*e));

	if (!avisos_ok) return -1;

	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_nsec += (long)((us % 1000000ull) * 1000ull);
	ts.tv_sec  += (time_t)(us / 1000000ull) + ts.tv_nsec / 1000000000L;
	ts.tv_nsec %= 1000000000L;

	r = sem_timedwait(&avisos, &ts);
	if (r != 0) return -1;

	/* En ESTE aviso ya esta escrito el bloque del aviso ANTERIOR: aud.c
	 * es un solo hilo y hace receive -> escribe -> receive. Asi que se
	 * mira el de antes, que es el unico que se sabe terminado.
	 *
	 * escritos sube aqui, o sea que la prueba, al esperar a que suba, ya
	 * tiene la garantia de que ese bloque esta completo. Sin eso seria
	 * una carrera con el hilo del audio. */
	pthread_mutex_lock(&m);
	if (recibidos >= 1) {
		uint32_t b = (recibidos - 1) % BLOQUES;
		memcpy(ultimo, &buffer[b * AUDIO_BLOCK_SAMPLES * CANALES],
		       sizeof(ultimo));
		escritos++;
	}
	recibidos++;
	pthread_mutex_unlock(&m);

	return 0;
}

int32_t sysEventQueueDrain(sys_event_queue_t q) { (void)q; return 0; }
int32_t sysEventQueueDestroy(sys_event_queue_t q, int32_t mode)
{
	(void)q; (void)mode;
	if (avisos_ok) { sem_destroy(&avisos); avisos_ok = 0; }
	return 0;
}

/* --- Opus de mentira -------------------------------------------------- */
/*
 * Devuelve 960 pares (20 ms a 48 kHz) con el valor nota/255 repetido, o
 * la nota anterior si se le pide ocultar una perdida -- que es justo lo
 * que hace el PLC de verdad: extender la trama anterior. */

struct OpusDecoder { unsigned char ultima_nota; };
static struct OpusDecoder unico;

OpusDecoder *opus_decoder_create(opus_int32 fs, int ch, int *err)
{
	if (fs != 48000 || ch != 2) { if (err) *err = -1; return NULL; }
	unico.ultima_nota = 0;
	if (err) *err = OPUS_OK;
	return &unico;
}

void opus_decoder_destroy(OpusDecoder *d) { (void)d; }

int opus_decode_float(OpusDecoder *d, const unsigned char *data,
                      opus_int32 len, float *pcm, int frame_size, int fec)
{
	int i;
	unsigned char nota;

	(void)fec;
	if (frame_size < 960) return -1;

	if (data != NULL) {
		/* Un solo byte de carga. Si llegan mas, el relleno de RTP no se
		 * ha quitado y la prueba lo tiene que ver. */
		if (len != 1) return -1;
		nota = data[0];
		d->ultima_nota = nota;
	} else {
		nota = d->ultima_nota;   /* PLC: extiende la anterior */
	}

	for (i = 0; i < 960; i++) {
		pcm[i * 2 + 0] = (float)nota / 255.0f;
		pcm[i * 2 + 1] = (float)nota / 255.0f;
	}

	return 960;
}

const char *opus_strerror(int e) { (void)e; return "opus de mentira"; }
