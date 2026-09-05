/* GR33N - audio de xCloud. Ver aud.h para el porque de cada decision. */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <ppu-types.h>
#include <ppu-asm.h>
#include <sys/thread.h>
#include <sys/mutex.h>
#include <sys/systime.h>
#include <sys/event_queue.h>
#include <audio/audio.h>

/* <opus/opus.h> y no <opus.h>: el Makefile pone -I$(OPUS)/include y
 * build-opus.sh deja las cabeceras en include/opus/, que es donde las
 * pone opus de serie. Con <opus.h> haria falta un -I extra solo para
 * esto. */
#include <opus/opus.h>

#include "gr33n.h"
#include "link.h"
#include "aud.h"

#define AUD_PRIO   1004        /* por encima del ping, por debajo del video */
#define AUD_STACK  (64 * 1024)

/* El anillo de muestras ya descodificadas, en pares (izq, der).
 *
 * TIENE QUE CABER EL COLCHON **MAS** UNA TRAMA ENTERA, y aqui me la pegue.
 *
 * Estaba en 8192 pares "porque son 170 ms y el colchon son 160". Pero la
 * capacidad util no es 8192: rellenar_pcm solo descodifica mientras quepa
 * una trama del caso peor (AUD_FRAME_MAX, 120 ms), asi que deja de llenar
 * en 8191 - 5760 = 2431 pares. Cincuenta milisegundos. El colchon de 160
 * no se alcanzaba NUNCA y el audio no empezaba a sonar jamas.
 *
 * Lo bonito del fallo es que no habia nada roto: cada pieza hacia
 * exactamente lo suyo y el resultado era silencio. Lo canto la prueba del
 * PC en el primer intento -- nueve comprobaciones a la vez, y ninguna
 * decia "el anillo es pequeno", decian "no suena".
 *
 * 16384 pares = 341 ms. Con el colchon de 160 y la trama de 120 quedan 60
 * de margen. Son 128 KB de los 256 MB de la consola. */
#define PCM_PARES  16384
#define PCM_MASK   (PCM_PARES - 1)

/* Y esto es la guarda que faltaba. Si alguien sube AUD_ESPERA_MS y se
 * olvida del anillo, el compilador lo dice; antes, el sintoma era silencio
 * y a buscar. */
#if (PCM_PARES - 1) < ((AUD_SR / 1000) * AUD_ESPERA_MS + AUD_FRAME_MAX)
#error "el anillo de PCM no puede contener el colchon mas una trama entera"
#endif

/* --------------------------------------------------------------------- */

static sys_mutex_t mtx;
static int         mtx_ok = 0;
#define LOCK()    do { if (mtx_ok) sysMutexLock(mtx, 0); } while (0)
#define UNLOCK()  do { if (mtx_ok) sysMutexUnlock(mtx); } while (0)

static audInfo info;

static sys_ppu_thread_t tid;
static volatile int running = 0;
static int          started = 0;

/* --- el puerto ------------------------------------------------------- */

static u32               puerto = 0;
static audioPortConfig   cfg;
static sys_event_queue_t colaq;
static sys_ipc_key_t     colak;
static int               puerto_abierto = 0;
static int               cola_puesta = 0;
static u32               bloque = 0;

/* --- el reordenador -------------------------------------------------- */

typedef struct {
	u8  datos[AUD_PKT_MAX];
	u32 n;
	u16 seq;
	int lleno;
} audPkt;

static audPkt   ranura[AUD_PKTS];
static int      arrancado = 0;    /* ya sabemos por que seq vamos */
static u16      siguiente = 0;    /* la que toca descodificar     */
static u32      encolados = 0;    /* cuantas ranuras llenas hay   */

/* --- el anillo de muestras ------------------------------------------- */

static f32 pcm[PCM_PARES * AUD_CANALES];
static u32 pcm_w = 0, pcm_r = 0;

static OpusDecoder *dec = NULL;
static int          volumen = 100;

static u64 tb_hz = 0;

/* --------------------------------------------------------------------- */

static void alog(const char *fmt, ...)
{
	char buf[224];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	linkLog("[aud] %s", buf);
}

static void afail(const char *fmt, ...)
{
	va_list ap;

	LOCK();
	va_start(ap, fmt);
	vsnprintf(info.err, sizeof(info.err), fmt, ap);
	va_end(ap);
	UNLOCK();

	linkLog("!! [aud] %s", info.err);
}

/* --------------------------------------------------------------------- */
/* La cabecera RTP                                                       */
/* --------------------------------------------------------------------- */

/* La misma que vjitter.c, y a proposito la misma: son el mismo formato y
 * los mismos errores posibles. Si algun dia hay que arreglar una, hay que
 * arreglar las dos, y por eso llevan el mismo nombre de campos.
 *
 * El relleno importa aqui igual que en video: libwebrtc manda paquetes de
 * relleno para sondear ancho de banda, y unos bytes de basura metidos en
 * opus_decode no dan error, dan un chasquido. */
static int parse_rtp(const u8 *rtp, u32 len, u16 *seq,
                     const u8 **carga, u32 *carga_len)
{
	u32 cab;
	u8  cc;
	int ext, relleno;

	if (rtp == NULL || len < 12) return 0;

	cc      = (u8)(rtp[0] & 0x0f);
	ext     = (rtp[0] & 0x10) != 0;
	relleno = (rtp[0] & 0x20) != 0;

	*seq = (u16)(((u16)rtp[2] << 8) | rtp[3]);

	cab = 12u + 4u * (u32)cc;
	if (len < cab) return 0;

	if (ext) {
		u32 palabras;

		if (len < cab + 4) return 0;
		palabras = ((u32)rtp[cab + 2] << 8) | rtp[cab + 3];
		cab += 4 + 4 * palabras;
		if (len < cab) return 0;
	}

	*carga_len = len - cab;

	if (relleno && *carga_len > 0) {
		u8 pad = rtp[len - 1];
		*carga_len = (pad <= *carga_len) ? *carga_len - pad : 0;
	}

	*carga = rtp + cab;
	return (*carga_len > 0);
}

/* Distancia con vuelta al origen. Positiva = a esta por delante de b. */
static s32 dist_seq(u16 a, u16 b)
{
	return (s32)(s16)(a - b);
}

/* --------------------------------------------------------------------- */
/* Entrada: lo llama el hilo del bombeo                                  */
/* --------------------------------------------------------------------- */

void audRtp(const u8 *pkt, u32 n)
{
	const u8 *carga;
	u32 carga_len;
	u16 seq;
	audPkt *r;

	if (!started || !parse_rtp(pkt, n, &seq, &carga, &carga_len)) return;
	if (carga_len > AUD_PKT_MAX) return;

	LOCK();

	info.rtp_recibidos++;

	if (!arrancado) {
		arrancado = 1;
		siguiente = seq;
		alog("primer paquete de audio, seq %u", (unsigned)seq);
	}

	/* YA HA PASADO DE LARGO.
	 *
	 * Un paquete que llega despues de que su hueco se haya tapado con PLC
	 * no se puede usar: Opus ya ha avanzado su estado. Meterlo ahora seria
	 * reproducir 20 ms del pasado en medio del presente, que se oye peor
	 * que el hueco. Se cuenta y se tira. */
	if (dist_seq(seq, siguiente) < 0) {
		info.rtp_tarde++;
		UNLOCK();
		return;
	}

	/* Demasiado por delante: la red ha dado un salto enorme o hemos
	 * perdido el hilo. Se reancla en vez de llenar el buffer de agujeros. */
	if (dist_seq(seq, siguiente) >= (s32)AUD_PKTS) {
		alog("salto de %d paquetes, reanclando en %u",
		     (int)dist_seq(seq, siguiente), (unsigned)seq);
		memset(ranura, 0, sizeof(ranura));
		encolados = 0;
		siguiente = seq;
	}

	r = &ranura[seq % AUD_PKTS];

	if (r->lleno && r->seq == seq) {
		info.rtp_repetidos++;
		UNLOCK();
		return;
	}

	if (!r->lleno) encolados++;

	memcpy(r->datos, carga, carga_len);
	r->n     = carga_len;
	r->seq   = seq;
	r->lleno = 1;

	UNLOCK();
}

/* --------------------------------------------------------------------- */
/* Descodificar: lo hace el hilo del audio                               */
/* --------------------------------------------------------------------- */

/* Cuantos pares caben todavia. */
static u32 pcm_hueco(void)
{
	u32 usados = (pcm_w - pcm_r) & PCM_MASK;
	return PCM_PARES - 1 - usados;
}

static u32 pcm_usados(void)
{
	return (pcm_w - pcm_r) & PCM_MASK;
}

/* Mete una trama descodificada en el anillo. */
static void pcm_mete(const f32 *src, u32 pares)
{
	u32 i;

	for (i = 0; i < pares; i++) {
		u32 p = (pcm_w + i) & PCM_MASK;
		pcm[p * 2 + 0] = src[i * 2 + 0];
		pcm[p * 2 + 1] = src[i * 2 + 1];
	}
	pcm_w = (pcm_w + pares) & PCM_MASK;
}

/* Descodifica lo que haga falta para tener fondo en el anillo.
 *
 * SE LLAMA SIN EL CANDADO, Y COGE EL SUYO A RATOS. La primera version
 * descodificaba con el candado cogido de punta a punta, y eso es un error
 * de los que no dan sintoma hasta que ya estas mirando otra cosa:
 * opus_decode en un PPE tarda un rato largo por trama, y al arrancar hay
 * que descodificar varias seguidas para llenar el colchon. Todo ese tiempo
 * audRtp() --que lo llama el hilo del BOMBEO-- se queda esperando en el
 * candado.
 *
 * Y el bombeo no puede pararse: es el que manda las comprobaciones de
 * conectividad y saca el video. Pararlo unos milisegundos es exactamente
 * lo que green-nx avisa que desborda la cola de recepcion UDP y se ve como
 * un tiron de video. Habriamos arreglado el audio rompiendo la imagen.
 *
 * Asi que el paquete se COPIA con el candado cogido, se suelta, se
 * descodifica, y se vuelve a coger para meter las muestras.
 *
 * Tres tramas por bloque como mucho: son 60 ms de audio por cada 5,33 ms
 * de reloj del puerto, o sea que recupera veinte veces mas rapido de lo
 * que consume, sin monopolizar nada. */
static void rellenar_pcm(void)
{
	static f32 tmp[AUD_FRAME_MAX * AUD_CANALES];
	static u8  copia[AUD_PKT_MAX];
	int vueltas;

	if (dec == NULL) return;

	for (vueltas = 0; vueltas < 3; vueltas++) {
		audPkt *r;
		u32 n = 0;
		int hay = 0, plc = 0;
		int muestras;

		LOCK();

		if (!arrancado || pcm_hueco() < AUD_FRAME_MAX) {
			UNLOCK();
			return;
		}

		r = &ranura[siguiente % AUD_PKTS];

		if (r->lleno && r->seq == siguiente) {
			memcpy(copia, r->datos, r->n);
			n = r->n;
			r->lleno = 0;
			if (encolados) encolados--;
			hay = 1;
		} else if (encolados > 0) {
			/* EL HUECO SOLO SE TAPA SI HAY ALGO DETRAS.
			 *
			 * Si no hay nada en todo el buffer no es un hueco: es que
			 * todavia no ha llegado. Taparlo seria inventarse audio y,
			 * peor, dejar pasar de largo el paquete bueno cuando llegue.
			 *
			 * opus_decode con NULL es el ocultador de perdidas de Opus:
			 * extiende la trama anterior en vez de meter silencio, que
			 * en 20 ms es la diferencia entre no notarlo y un chasquido. */
			plc = 1;
		}

		if (hay || plc) siguiente++;

		UNLOCK();

		if (!hay && !plc) return;

		/* AQUI, sin candado. */
		muestras = opus_decode_float(dec,
		                             hay ? copia : NULL,
		                             hay ? (opus_int32)n : 0,
		                             tmp, AUD_FRAME_MAX, 0);

		LOCK();
		if (muestras > 0) {
			pcm_mete(tmp, (u32)muestras);
			if (hay) info.tramas++;
			else     info.ocultadas++;
		}
		UNLOCK();
	}
}

/* --------------------------------------------------------------------- */
/* El hilo: un bloque cada vez que el puerto avisa                       */
/* --------------------------------------------------------------------- */

static void aud_thread(void *arg)
{
	/* Cuantos pares hay que tener antes de empezar a soltar. Se calcula
	 * una vez: es AUD_ESPERA_MS de audio. */
	const u32 colchon = (u32)AUD_SR * AUD_ESPERA_MS / 1000u;
	int sonando = 0;

	(void)arg;

	while (running) {
		sys_event_t ev;
		f32 *dst;
		u32 i;
		s32 r;

		/* Con tiempo de espera, y no infinito: si el puerto deja de
		 * avisar --se cerro la salida de audio, el sistema hizo algo--
		 * este hilo tiene que poder enterarse de que hay que salir. */
		r = sysEventQueueReceive(colaq, &ev, 100 * 1000);
		if (r != 0) continue;
		if (!running) break;

		dst = (f32*)(u64)(cfg.audioDataStart +
		                  bloque * cfg.channelCount *
		                  AUDIO_BLOCK_SAMPLES * sizeof(f32));

		/* Fuera del candado: se lo coge ella a ratos. */
		rellenar_pcm();

		LOCK();

		if (!sonando) {
			/* Todavia llenando el colchon: silencio de verdad, no lo que
			 * hubiera en el buffer del sistema. */
			if (pcm_usados() >= colchon) {
				sonando = 1;
				alog("empieza a sonar con %u ms de colchon",
				     (unsigned)(pcm_usados() * 1000u / AUD_SR));
			}
		} else if (pcm_usados() == 0) {
			/* Nos hemos quedado secos. Se vuelve a llenar el colchon en
			 * vez de ir dando tirones bloque si bloque no. */
			sonando = 0;
			info.vacios++;
		}

		if (sonando && pcm_usados() >= AUDIO_BLOCK_SAMPLES) {
			f32 g = (f32)volumen / 100.0f;

			for (i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
				u32 p = (pcm_r + i) & PCM_MASK;
				dst[i * 2 + 0] = pcm[p * 2 + 0] * g;
				dst[i * 2 + 1] = pcm[p * 2 + 1] * g;
			}
			pcm_r = (pcm_r + AUDIO_BLOCK_SAMPLES) & PCM_MASK;
		} else {
			memset(dst, 0,
			       AUDIO_BLOCK_SAMPLES * AUD_CANALES * sizeof(f32));
		}

		info.pcm_ms  = pcm_usados() * 1000u / AUD_SR;
		info.bloques++;

		UNLOCK();

		/* "El primer bloque es el 0, el siguiente el 1, y asi hasta
		 * numBlocks, y entonces vuelve al 0." Lo dice la cabecera de
		 * PSL1GHT, y es la fuente: no se usa readIndex porque no hace
		 * falta y su significado exacto no esta documentado ahi. */
		bloque = (bloque + 1) % (u32)cfg.numBlocks;
	}

	sysThreadExit(0);
}

/* --------------------------------------------------------------------- */

int audInit(void)
{
	sys_mutex_attr_t attr;

	memset(&info, 0, sizeof(info));

	tb_hz = sysGetTimebaseFrequency();
	if (tb_hz == 0) tb_hz = 79800000ull;

	sysMutexAttrInitialize(attr);
	mtx_ok = (sysMutexCreate(&mtx, &attr) == 0);

	if (audioInit() != 0) {
		afail("audioInit fallo: no habra sonido");
		return -1;
	}

	return 0;
}

void audShutdown(void)
{
	audStop();
	audioQuit();

	if (mtx_ok) {
		sysMutexDestroy(mtx);
		mtx_ok = 0;
	}
}

int audStart(void)
{
	audioPortParam param;
	int err = 0;

	if (started) return 0;

	LOCK();
	memset(ranura, 0, sizeof(ranura));
	memset(pcm, 0, sizeof(pcm));
	pcm_w = pcm_r = 0;
	arrancado = 0;
	encolados = 0;
	bloque = 0;
	info.rtp_recibidos = info.rtp_tarde = info.rtp_repetidos = 0;
	info.tramas = info.ocultadas = info.vacios = info.bloques = 0;
	info.err[0] = '\0';
	UNLOCK();

	dec = opus_decoder_create(AUD_SR, AUD_CANALES, &err);
	if (dec == NULL || err != OPUS_OK) {
		afail("opus_decoder_create: %s", opus_strerror(err));
		dec = NULL;
		return -1;
	}

	memset(&param, 0, sizeof(param));
	param.numChannels = AUDIO_PORT_2CH;
	param.numBlocks   = AUDIO_BLOCK_8;
	param.attrib      = 0;
	param.level       = 1.0f;

	if (audioPortOpen(&param, &puerto) != 0) {
		afail("audioPortOpen fallo");
		goto mal;
	}
	puerto_abierto = 1;

	if (audioGetPortConfig(puerto, &cfg) != 0) {
		afail("audioGetPortConfig fallo");
		goto mal;
	}

	/* Se comprueba lo que ha dado el sistema en vez de darlo por hecho.
	 * Pedimos 2 canales; si nos diera otra cosa, el calculo de la
	 * direccion del bloque de mas abajo escribiria fuera. */
	if (cfg.channelCount != AUD_CANALES) {
		afail("el puerto ha salido con %u canales, no %d",
		      (unsigned)cfg.channelCount, AUD_CANALES);
		goto mal;
	}

	if (audioCreateNotifyEventQueue(&colaq, &colak) != 0) {
		afail("audioCreateNotifyEventQueue fallo");
		goto mal;
	}

	if (audioSetNotifyEventQueue(colak) != 0) {
		afail("audioSetNotifyEventQueue fallo");
		goto mal;
	}
	cola_puesta = 1;

	/* Se vacia lo que hubiera encolado antes de empezar: si no, el hilo
	 * arranca con un puñado de avisos viejos y escribe varios bloques de
	 * golpe en el sitio equivocado. */
	sysEventQueueDrain(colaq);

	if (audioPortStart(puerto) != 0) {
		afail("audioPortStart fallo");
		goto mal;
	}

	running = 1;
	if (sysThreadCreate(&tid, aud_thread, NULL, AUD_PRIO, AUD_STACK,
	                    THREAD_JOINABLE, "GR33N audio") != 0) {
		running = 0;
		afail("sysThreadCreate del hilo de audio fallo");
		goto mal;
	}

	started = 1;

	LOCK();
	info.activo = 1;
	info.puerto = (int)puerto;
	UNLOCK();

	alog("puerto %u: %u canales, %u bloques de %d muestras (%u ms), "
	     "colchon %d ms",
	     (unsigned)puerto, (unsigned)cfg.channelCount,
	     (unsigned)cfg.numBlocks, AUDIO_BLOCK_SAMPLES,
	     (unsigned)((u32)cfg.numBlocks * AUDIO_BLOCK_SAMPLES * 1000u / AUD_SR),
	     AUD_ESPERA_MS);

	return 0;

mal:
	audStop();
	return -1;
}

void audStop(void)
{
	if (started) {
		running = 0;
		sysThreadJoin(tid, NULL);
		started = 0;
	}

	if (puerto_abierto) audioPortStop(puerto);

	if (cola_puesta) {
		audioRemoveNotifyEventQueue(colak);
		sysEventQueueDestroy(colaq, 0);
		cola_puesta = 0;
	}

	if (puerto_abierto) {
		audioPortClose(puerto);
		puerto_abierto = 0;
	}

	if (dec) {
		opus_decoder_destroy(dec);
		dec = NULL;
	}

	LOCK();
	info.activo = 0;
	UNLOCK();
}

const audInfo *audStatus(void)
{
	static audInfo snap;

	LOCK();
	snap = info;
	UNLOCK();

	return &snap;
}

void audSetVolumen(int v)
{
	if (v < 0)   v = 0;
	if (v > 100) v = 100;
	volumen = v;
}

int audVolumen(void) { return volumen; }
