/* GR33N - decodificador H.264 por hardware (cellVdec) */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>

#include <ppu-types.h>
#include <ppu-asm.h>
#include <sys/systime.h>
#include <sys/thread.h>
#include <sys/mutex.h>
#include <sys/spu.h>
#include <sysmodule/sysmodule.h>
#include <codec/vdec.h>

#include "gr33n.h"
#include "video.h"
#include "link.h"
#include "decoder.h"
#include "copy.h"

/* Nivel del bitstream x10. 31 = H.264 nivel 3.1, que cubre 720p30.
 * Para 720p60 haria falta 32 o 40. Determina la memoria que reserva el
 * decodificador, asi que no conviene pasarse por gusto. */
#define DEC_PROFILE_LEVEL   31

#define DEC_MAX_AUS         4096
#define DEC_PIC_BYTES       (DEC_MAX_W * DEC_MAX_H * 4)

/* Ranuras de imagen. Con una sola, el decodificador no puede empezar la
 * siguiente hasta que el hilo de dibujo se lleva la anterior: eso no es
 * una tuberia, es un embudo con pasos. Tres bastan para desacoplar
 * productor y consumidor sin gastar 10 MB en buffers. */
#define DEC_SLOTS           3

/* Unidades de acceso enviadas sin imagen todavia. El techo real lo dice
 * vdecAttr.cmd_depth (4 en esta consola); esto es solo el tamano del
 * anillo de marcas de tiempo. */
#define DEC_INFLIGHT        8

/* 1 = decodificar a toda velocidad, sin limitar el ritmo.
 *
 * Es lo que hace falta para MEDIR el techo del decodificador: si le pongo
 * un limitador de 30 fps y luego mido 30 fps, lo unico que he medido es
 * mi propio limitador. El clip de prueba se reproduce mas rapido de lo
 * normal, y me da igual: es un banco de pruebas, no una pelicula.
 *
 * En el cliente real el ritmo lo marca la llegada de paquetes. */
#define DEC_FREE_RUN        1
#define DEC_FRAME_US        33333       /* 30 fps, si DEC_FREE_RUN es 0 */

#define DEC_PIC_TIMEOUT_US  2000000ull  /* dos segundos sin imagen = error */

#define VDEC_PPU_PRIO       1000
#define VDEC_PPU_STACK      (256 * 1024)
#define VDEC_SPU_PRIO       200

/* Cuantos SPUs se le dan a cellVdec. Con UNO el ritmo se clava en 23,5 ms
 * por imagen (42 fps) aunque la tuberia este llena, o sea que el cuello no
 * esta en como le damos trabajo sino en quien lo hace.
 *
 * Cada vez que se entra en H264_DEBUG se prueba el siguiente de la lista y
 * se dice en el log. Tres entradas y hay tabla, sin recompilar ni cambiar
 * un numero a mano entre pruebas. */
static const u32 spu_ladder[] = { 1, 2, 4 };
#define SPU_LADDER_N  ((int)(sizeof(spu_ladder)/sizeof(spu_ladder[0])))

#define DEC_THREAD_PRIO     1001
#define DEC_THREAD_STACK    (64 * 1024)

/* --------------------------------------------------------------------- */

typedef struct { u32 off, len; } decAU;

static sys_mutex_t mtx;
static int mtx_ok = 0;

#define LOCK()    do { if (mtx_ok) sysMutexLock(mtx, 0); } while (0)
#define UNLOCK()  do { if (mtx_ok) sysMutexUnlock(mtx); } while (0)

static decInfo info;

static u32   handle = 0;
static int   opened = 0;
static void *vdec_mem = NULL;

/* --- MODO EN VIVO ----------------------------------------------------
 *
 * El modo de siempre (decStart) recibe un clip entero, lo trocea en
 * unidades de acceso y da vueltas. Para xCloud eso no vale: las unidades
 * llegan de la red segun se montan, y cellVdec necesita que el buffer de
 * cada una siga vivo hasta que la haya digerido.
 *
 * Asi que un anillo de ranuras con TRES punteros, no dos:
 *
 *   live_w  donde escribe la red
 *   live_r  de donde saca el hilo del decodificador
 *   live_f  la mas vieja todavia en vuelo, que es la que se libera
 *
 * Con dos no basta: entre "enviada a cellVdec" y "ya ha salido la imagen"
 * la ranura no se puede reutilizar, y esa ventana son varias unidades.
 * Reutilizarla antes de tiempo no da error: le cambia los bytes a una
 * unidad que el decodificador todavia esta leyendo.
 *
 * Se libera en ORDEN, y se puede: cellVdec consume las unidades en el
 * mismo orden en que se le dan, asi que cuando sale una imagen la que se
 * ha terminado es la mas vieja de las que quedan. Es la misma contabilidad
 * que ya llevan sub_head/sub_tail para los tiempos.
 *
 * El tamano de ranura es el mismo tope que produce vjitter.c (VJ_AU_MAX),
 * a proposito: asi NADA de lo que monte el reensamblado puede no caber
 * aqui. Una unidad recortada tiene forma de unidad y el decodificador la
 * intentaria. */
#define DEC_LIVE_SLOTS   6
#define DEC_LIVE_MAX     (512 * 1024)

typedef struct {
	u8  datos[DEC_LIVE_MAX];
	u32 len;
} decLiveAu;

static decLiveAu live[DEC_LIVE_SLOTS];
static u32 live_w = 0, live_r = 0, live_f = 0;
static u32 live_q = 0;     /* encoladas, sin enviar */
static u32 live_v = 0;     /* enviadas, sin imagen todavia */
static int live_mode = 0;
static u32 live_drops = 0; /* unidades tiradas por no haber sitio */

static const u8 *au_stream = NULL;
static u32 au_stream_size = 0;
static decAU aus[DEC_MAX_AUS];

/* Suelta la ranura mas vieja en vuelo. Se llama en los DOS sitios donde
 * baja inflight, que es donde cellVdec dice que ha terminado con una. */
static void live_libera(void)
{
	if (!live_mode || live_v == 0) return;
	live_f = (live_f + 1) % DEC_LIVE_SLOTS;
	live_v--;
}

/* Anillo de imagenes. El hilo del decodificador rellena, el de dibujo
 * consume, y ninguno espera al otro mas de lo imprescindible.
 *
 *   full = tiene imagen sin mostrar
 *   busy = el hilo de dibujo la esta copiando ahora mismo
 *
 * Cuando no hay hueco NO se para: se pisa la mas vieja que no este busy y
 * se cuenta un descarte. Eso es lo que hace un cliente de streaming de
 * verdad — quedarse atras mostrando frames viejos es peor que saltarselos
 * — y ademas es lo unico que deja medir el techo del decodificador. */
typedef struct {
	u8 *pix;
	u32 w, h;
	u64 seq;          /* orden de salida, para saber cual es la mas vieja */
	int full;
	int busy;
} decSlot;

static decSlot slots[DEC_SLOTS];
static u64 out_seq = 0;

static volatile int pic_ready = 0;   /* lo pone el callback de cellVdec */
static volatile int seq_error = 0;

/* Marcas de tiempo de envio, en orden. El clip va sin frames B
 * (-bf 0 al codificarlo), asi que el orden de salida es el de entrada y
 * una FIFO basta para emparejar imagen con envio. Con frames B habria que
 * ir por PTS. */
static u64 submit_t[DEC_INFLIGHT];
static u32 sub_head = 0, sub_tail = 0;
static u32 inflight = 0;
static u32 inflight_max = 2;

static u64 last_out_us = 0;
static u64 pace_sum = 0;
static u32 pace_n = 0;

static sys_ppu_thread_t dec_tid;
static volatile int dec_running = 0;
static int dec_started = 0;

/* Peticion de arranque. decStart() solo la deja puesta y vuelve: abrir el
 * decodificador puede bloquear, y bloquear el bucle de dibujo con eso es
 * como se congelo la consola la primera vez. */
static volatile int start_req = 0;
static const u8 *req_stream = NULL;
static u32 req_size = 0;

static u64 lat_sum = 0;
static u32 lat_n = 0;

static int spu_step = -1;     /* indice en spu_ladder */
static u32 cur_spus = 0;      /* con cuantos esta abierto ahora mismo */
static u32 req_spus = 0;

/* Parar tambien va por peticion al hilo. TODAS las llamadas a cellVdec
 * tienen que salir del MISMO hilo: mientras el hilo del decodificador
 * estaba enviando unidades, el de dibujo llamaba a vdecEndSequence al
 * salir del titulo, y de ahi el 0x80610102. No era una carrera de las
 * mias sobre una variable: eran dos hilos hablandole a la vez a un
 * decodificador que no admite eso. */
static volatile int stop_req = 0;

/* --------------------------------------------------------------------- */
/* RECUPERARSE DE UN ATASCO DE cellVdec                                  */
/*                                                                       */
/* MEDIDO, no supuesto. Log del 5 de septiembre, sesion de 9 minutos:    */
/*                                                                       */
/*   [515.70] dec: 14396 imagenes, 0 tiradas   <- todo perfecto          */
/*   [515.79] AU #14400: 73 bytes, nal 1                                 */
/*   [515.80] FALLO: 3 unidades enviadas y ninguna imagen en 2 s         */
/*   [571.56] dec: 14396 imagenes, 1671 tiradas <- 56 s tirando video    */
/*                                                                       */
/* La red estaba impecable en ese momento: 0 paquetes de audio ocultados,*/
/* 0 tarde, 0 vacios, y el reensamblado siguio montando unidades sin un  */
/* solo hueco. Lo que se paro fue el descodificador de hardware: acepto  */
/* tres unidades y no devolvio ni una imagen mas, nunca.                 */
/*                                                                       */
/* Y NO HABIA SALIDA. fail() ponia DEC_FAILED y ahi se quedaba: imagen   */
/* congelada hasta cerrar la aplicacion. Salir del juego tampoco valia,  */
/* porque decStop() solo hacia vdecEndSequence y se quedaba el handle;   */
/* la siguiente partida heredaba el mismo descodificador atascado.       */
/*                                                                       */
/* Ahora se cierra de verdad y se vuelve a abrir. Y despues hace falta   */
/* un fotograma clave: un descodificador recien abierto no puede         */
/* empezar por la mitad de un GOP.                                       */
/* --------------------------------------------------------------------- */

/* Tres y se rinde. Si cellVdec se atasca tres veces seguidas no es un
 * hipo, es que algo va mal de verdad, y reabrir en bucle solo cambiaria
 * una imagen congelada por un parpadeo eterno. */
#define DEC_REINTENTOS 3

/* Y la cuenta SE OLVIDA si el descodificador ha aguantado un buen rato.
 *
 * Sin esto, tres hipos repartidos por una partida de dos horas cuentan
 * igual que tres seguidos, y la cuarta vez se rinde con un descodificador
 * que en realidad iba bien. Lo que quiere detectar el limite es "esto no
 * levanta", no "esto ha tropezado tres veces en toda la tarde". */
#define DEC_OLVIDO_US  60000000ull   /* un minuto entero bien = borron */

static u32          atascos = 0;
static u64          t_ultimo_atasco = 0;
static volatile int quiere_clave = 0;   /* hay que pedir un IDR */

/* --------------------------------------------------------------------- */

static u64 now_us(void)
{
	u64 sec = 0, nsec = 0;
	sysGetCurrentTime(&sec, &nsec);
	return sec * 1000000ull + nsec / 1000ull;
}

/* __get_opd32() lleva dentro una comprobacion de nulo. Pasandole el
 * nombre de una funcion directamente, GCC ve que nunca puede ser nula y
 * suelta -Waddress en cada compilacion. Metiendolo por un parametro deja
 * de ser constante y el aviso desaparece — que es exactamente como lo usa
 * el propio PSL1GHT en librsx/init.c. Nada de replicar el +16 a mano: si
 * algun dia cambia la convencion, que cambie en un solo sitio. */
static u32 opd32_of(const void *fn)
{
	return (u32)__get_opd32(fn);
}

/* printf va al TTY, que no se ve arrancando desde el XMB o multiMAN.
 * Todo diagnostico del decodificador tiene que salir tambien por el log
 * remoto o no existe. */
static void dlog(const char *fmt, ...)
{
	char buf[192];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	printf("[dec] %s\n", buf);
	linkLog("[dec] %s", buf);
}

static void fail(const char *fmt, ...)
{
	va_list ap;

	LOCK();
	info.state = DEC_FAILED;
	va_start(ap, fmt);
	vsnprintf(info.err, sizeof(info.err), fmt, ap);
	va_end(ap);
	UNLOCK();

	printf("[dec] %s\n", info.err);
	linkLog("[dec] FALLO: %s", info.err);
}

/* --------------------------------------------------------------------- */
/* Troceado en unidades de acceso                                        */
/* --------------------------------------------------------------------- */

/* Heuristica del propio H.264: una unidad de acceso nueva empieza en el
 * primer NAL que puede iniciarla (AUD, SPS, PPS, SEI o slice) DESPUES de
 * haber visto ya un slice. Funciona con streams que llevan AUD y con los
 * que no. */
static int nal_starts_au(u32 type)
{
	return type == 9 || type == 7 || type == 8 || type == 6 ||
	       type == 1 || type == 5;
}

static u32 build_au_index(const u8 *d, u32 size)
{
	u32 n = 0, i = 0, au_start = 0;
	int seen_slice = 0, have_au = 0;

	while (i + 3 < size) {
		u32 type, sc;

		/* Codigos de arranque de 3 o 4 bytes */
		if (d[i] == 0 && d[i+1] == 0 && d[i+2] == 1) sc = 3;
		else if (i + 4 < size && d[i] == 0 && d[i+1] == 0 &&
		         d[i+2] == 0 && d[i+3] == 1) sc = 4;
		else { i++; continue; }

		type = d[i + sc] & 0x1f;

		if (seen_slice && nal_starts_au(type)) {
			if (n < DEC_MAX_AUS) {
				aus[n].off = au_start;
				aus[n].len = i - au_start;
				n++;
			} else {
				printf("[dec] AVISO: mas de %d unidades de acceso, "
				       "el resto del clip se ignora\n", DEC_MAX_AUS);
				return n;
			}
			au_start = i;
			seen_slice = 0;
		}

		if (!have_au) { au_start = i; have_au = 1; }
		if (type == 1 || type == 5) seen_slice = 1;

		i += sc;
	}

	if (have_au && seen_slice && n < DEC_MAX_AUS) {
		aus[n].off = au_start;
		aus[n].len = size - au_start;
		n++;
	}

	return n;
}

/* --------------------------------------------------------------------- */
/* Callback de cellVdec                                                  */
/* --------------------------------------------------------------------- */

/* Lo llama el hilo interno de cellVdec. Aqui no se hace trabajo: solo se
 * levanta una bandera y el hilo del decodificador se ocupa. */
static u32 dec_callback(u32 h, u32 msgtype, u32 msgdata, u32 arg)
{
	(void)h;
	(void)msgdata;
	(void)arg;

	switch (msgtype) {
	case VDEC_CALLBACK_PICOUT:
		pic_ready = 1;
		break;
	case VDEC_CALLBACK_ERROR:
		seq_error = 1;
		break;
	case VDEC_CALLBACK_AUDONE:
	case VDEC_CALLBACK_SEQDONE:
	default:
		break;
	}

	return 0;
}

/* --------------------------------------------------------------------- */
/* Consumo de una imagen                                                 */
/* --------------------------------------------------------------------- */

/* Elige donde escribir la siguiente imagen: primero un hueco libre, y si
 * no hay, la mas vieja que no este siendo copiada. Devuelve -1 solo si
 * TODAS estan busy, que con tres ranuras y un solo consumidor no pasa. */
static int pick_slot(int *dropped)
{
	int i, best = -1;
	u64 oldest = 0;

	*dropped = 0;

	LOCK();

	for (i = 0; i < DEC_SLOTS; i++) {
		if (!slots[i].full && !slots[i].busy) { best = i; break; }
	}

	if (best < 0) {
		for (i = 0; i < DEC_SLOTS; i++) {
			if (slots[i].busy) continue;
			if (best < 0 || slots[i].seq < oldest) {
				best = i;
				oldest = slots[i].seq;
			}
		}
		if (best >= 0) *dropped = 1;
	}

	if (best >= 0) slots[best].busy = 1;   /* reservada para el decodificador */

	UNLOCK();

	return best;
}

/* Devuelve 1 si saco una imagen, 0 si no habia nada, -1 si algo va mal.
 *
 * "No habia nada" NO es un error: con la tuberia llena se drena en bucle
 * hasta que vdecGetPicItem dice que no queda, y esa ultima llamada falla
 * por diseno. Contarla como error fue lo primero que me equivoque al
 * encauzar. */
static int consume_picture(void)
{
	vdecPictureFormat fmt;
	vdecPicture *pic;
	vdecH264Info *h264;
	u32 addr = 0;
	u32 w, h, need;
	u64 t_submit, now;
	s32 ret;
	int sl, dropped;

	/* La ranura se pide PRIMERO. Si todas estan ocupadas por el hilo de
	 * dibujo no se saca la imagen: se deja en la cola de cellVdec y se
	 * vuelve. Escribir en una ranura que alguien esta leyendo seria media
	 * imagen nueva y media vieja, y ese defecto solo se ve en movimiento. */
	sl = pick_slot(&dropped);
	if (sl < 0) return 0;

	ret = vdecGetPicItem(handle, &addr);
	if (ret != 0 || addr == 0) {
		LOCK(); slots[sl].busy = 0; UNLOCK();
		return 0;                       /* no hay imagen, no pasa nada */
	}

	pic = (vdecPicture*)(uintptr_t)addr;

	/* Enviada pero sin imagen: la marca de tiempo hay que consumirla
	 * igual o la FIFO se desincroniza y todas las latencias posteriores
	 * quedan corridas una posicion. */
	if (pic->status == VDEC_PICTURE_SKIPPED) {
		LOCK(); slots[sl].busy = 0; UNLOCK();
		if (inflight) {
			sub_tail = (sub_tail + 1) % DEC_INFLIGHT;
			inflight--;
			live_libera();
		}
		return 1;
	}

	if (pic->codec_specific_addr == 0) {
		LOCK(); slots[sl].busy = 0; UNLOCK();
		fail("sin info de codec en la imagen");
		return -1;
	}

	h264 = (vdecH264Info*)(uintptr_t)pic->codec_specific_addr;
	w = h264->width;
	h = h264->height;

	if (w == 0 || h == 0) {
		LOCK(); slots[sl].busy = 0; UNLOCK();
		fail("el SPS dice %ux%u", (unsigned)w, (unsigned)h);
		return -1;
	}

	/* LA COMPROBACION QUE EVITA EL CUELGUE. vdecGetPicture escribe la
	 * imagen del tamano CODED que diga el bitstream, no del que nos
	 * gustaria. Si no cabe, no se llama y punto. */
	need = w * h * 4;
	if (need > DEC_PIC_BYTES) {
		LOCK(); slots[sl].busy = 0; UNLOCK();
		fail("imagen %ux%u no cabe (%u > %u bytes)",
		     (unsigned)w, (unsigned)h, (unsigned)need,
		     (unsigned)DEC_PIC_BYTES);
		return -1;
	}

	fmt.format_type  = VDEC_PICFMT_ARGB32;
	fmt.color_matrix = VDEC_COLOR_MATRIX_BT709;
	fmt.alpha        = 0xff;

	ret = vdecGetPicture(handle, &fmt, slots[sl].pix);

	now = now_us();

	t_submit = 0;
	if (inflight) {
		t_submit = submit_t[sub_tail];
		sub_tail = (sub_tail + 1) % DEC_INFLIGHT;
		live_libera();
		inflight--;
	}

	LOCK();

	slots[sl].busy = 0;

	if (ret != 0) {
		info.errors++;
		UNLOCK();
		return -1;
	}

	slots[sl].w = w;
	slots[sl].h = h;
	slots[sl].seq = ++out_seq;
	slots[sl].full = 1;

	info.width  = w;
	info.height = h;
	info.frames_decoded++;
	if (dropped) info.drops++;
	info.inflight = inflight;

	/* LATENCIA de esta unidad de acceso concreta */
	if (t_submit) {
		u32 dt = (u32)(now - t_submit);

		info.decode_us_last = dt;
		if (info.decode_us_min == 0 || dt < info.decode_us_min)
			info.decode_us_min = dt;
		if (dt > info.decode_us_max) info.decode_us_max = dt;

		lat_sum += dt;
		lat_n++;
		if (lat_n >= 30) {
			info.decode_us_avg = (u32)(lat_sum / lat_n);
			lat_sum = 0;
			lat_n = 0;
		}
	}

	/* RITMO: hueco entre esta imagen y la anterior. Este es el que dice
	 * si 720p60 cabe. */
	if (last_out_us) {
		u32 dp = (u32)(now - last_out_us);

		info.pace_us_last = dp;
		if (info.pace_us_min == 0 || dp < info.pace_us_min)
			info.pace_us_min = dp;

		pace_sum += dp;
		pace_n++;
		if (pace_n >= 30) {
			info.pace_us_avg = (u32)(pace_sum / pace_n);
			pace_sum = 0;
			pace_n = 0;
		}
	}
	last_out_us = now;

	UNLOCK();

	return 1;
}

/* --------------------------------------------------------------------- */
/* Hilo del decodificador                                                */
/* --------------------------------------------------------------------- */

/* Serie estricta: una unidad de acceso dentro, una imagen fuera. No
 * encauza, o sea que el rendimiento maximo queda limitado por la
 * latencia. Es a proposito: para MEDIR la latencia de decodificacion,
 * que es lo que buscamos, la version serie es la unica honesta. Encauzar
 * (decodificar la N+1 mientras se muestra la N) es lo siguiente, y hace
 * falta para el cliente real. */
static int  open_decoder(void);

/* Y esta tambien, que la usa el bucle para recuperarse de un atasco --y
 * el bucle esta por encima. Declarar las dos juntas evita repetir el
 * error de meter el uso por delante, que llevo tres veces esta semana. */
static void close_decoder(void);

/* Todo lo que puede bloquear vive aqui, en el hilo, no en el bucle de
 * dibujo: trocear el stream, abrir cellVdec y arrancar la secuencia. */
static void do_start(void)
{
	u32 n;
	s32 ret;

	if (live_mode) {
		/* Nada que trocear: las unidades llegan de la red ya montadas
		 * por vjitter.c. */
		n = 0;
		live_w = live_r = live_f = 0;
		live_q = live_v = 0;
		live_drops = 0;
		au_stream = NULL;
		au_stream_size = 0;
		dlog("0/4 EN VIVO, %u ranuras de %u KB - con %u SPU%s",
		     (unsigned)DEC_LIVE_SLOTS, (unsigned)(DEC_LIVE_MAX / 1024),
		     (unsigned)req_spus, req_spus == 1 ? "" : "s");
	} else {
		n = build_au_index(req_stream, req_size);
		if (n == 0) {
			fail("ninguna unidad de acceso en el stream");
			return;
		}

		dlog("0/4 %u unidades de acceso en %u KB - esta ronda con %u "
		     "SPU%s", (unsigned)n, (unsigned)(req_size / 1024),
		     (unsigned)req_spus, req_spus == 1 ? "" : "s");

		au_stream = req_stream;
		au_stream_size = req_size;
	}

	if (open_decoder() != 0) return;

	dlog("4/4 vdecStartSequence...");
	ret = vdecStartSequence(handle);
	if (ret != 0) {
		fail("vdecStartSequence: 0x%08x", (unsigned)ret);
		return;
	}
	dlog("4/4 ok - decodificando");

	LOCK();
	info.au_count = n;
	info.au_index = 0;
	info.state = DEC_PLAYING;
	UNLOCK();
}

/* Drena todas las imagenes que haya listas. Se limpia pic_ready ANTES de
 * drenar: si el callback vuelve a levantarlo mientras estamos aqui, la
 * siguiente vuelta lo ve. Al reves se perderia el aviso. */
static int drain_pictures(void)
{
	int n = 0, r;

	pic_ready = 0;

	while ((r = consume_picture()) > 0) {
		n++;
		if (n > 32) break;    /* por si acaso: nunca girar sin fin aqui */
	}

	if (r < 0) return -1;
	return n;
}

static void dec_thread(void *arg)
{
#if !DEC_FREE_RUN
	u64 next_due = 0;
#endif
	u64 t_last_progress = 0;

	(void)arg;

	while (dec_running) {
		u64 now;
		vdecAU au;
		s32 ret;
		u32 idx;
		int state_ok;

		if (stop_req) {
			int roto;

			stop_req = 0;

			LOCK();
			roto = (info.state == DEC_FAILED);
			UNLOCK();

			if (opened) {
				/* UN DESCODIFICADOR QUE HA FALLADO SE CIERRA, no se
				 * recicla. Terminar la secuencia y quedarse el handle es
				 * una optimizacion buena --reabrir cuesta-- pero solo
				 * cuando lo que se recicla funciona. Heredar un handle
				 * atascado es lo que hacia que, despues de un congelon,
				 * la siguiente partida naciera muerta. */
				if (roto) {
					close_decoder();
					dlog("cerrado del todo: venia fallando");
				} else {
					vdecEndSequence(handle);
					dlog("secuencia terminada");
				}
			}

			sub_head = sub_tail = 0;
			inflight = 0;
			last_out_us = 0;
			atascos = 0;
			t_ultimo_atasco = 0;
			quiere_clave = 0;
			continue;
		}

		if (start_req) {
			start_req = 0;
			do_start();
#if !DEC_FREE_RUN
			next_due = 0;
#endif
			t_last_progress = now_us();
			continue;
		}

		LOCK();
		state_ok = (info.state == DEC_PLAYING);
		idx = info.au_index;
		UNLOCK();

		if (!state_ok) { usleep(1000); continue; }

		/* En vivo, si todavia no ha llegado nada de la red no hay nada
		 * que enviar. Se cosecha igual --puede haber imagenes listas de
		 * lo anterior-- y se duerme poco: una unidad cada 16 ms y este
		 * bucle no puede llegar tarde. */
		if (live_mode && live_q == 0) {
			if (pic_ready) drain_pictures();
			usleep(500);
			continue;
		}

		/* 1. Cosechar. Siempre, no solo cuando el callback avisa: una
		 *    notificacion perdida con la tuberia llena seria un bloqueo
		 *    permanente, y drenar en vacio cuesta una llamada. */
		if (pic_ready || inflight >= inflight_max) {
			int got = drain_pictures();
			if (got < 0) continue;
			if (got > 0) t_last_progress = now_us();
		}

		if (seq_error) {
			seq_error = 0;
			LOCK(); info.errors++; UNLOCK();
		}

		/* 2. Si la tuberia esta llena, no hay nada que enviar. */
		if (inflight >= inflight_max) {
			if (now_us() - t_last_progress > DEC_PIC_TIMEOUT_US) {
				u64 ahora = now_us();

				if (t_ultimo_atasco &&
				    ahora - t_ultimo_atasco > DEC_OLVIDO_US) {
					dlog("el anterior atasco fue hace %u s: la cuenta "
					     "vuelve a empezar",
					     (unsigned)((ahora - t_ultimo_atasco) / 1000000ull));
					atascos = 0;
				}

				t_ultimo_atasco = ahora;
				atascos++;

				dlog("ATASCO %u: %u unidades enviadas y ninguna imagen "
				     "en %u s (%u imagenes hasta ahora)",
				     (unsigned)atascos, (unsigned)inflight,
				     (unsigned)(DEC_PIC_TIMEOUT_US / 1000000ull),
				     (unsigned)info.frames_decoded);

				if (atascos > DEC_REINTENTOS) {
					fail("cellVdec se ha atascado %u veces; me rindo",
					     (unsigned)atascos);
					continue;
				}

				/* CERRAR DE VERDAD, no solo terminar la secuencia.
				 *
				 * vdecEndSequence sobre un handle atascado no lo
				 * desatasca: eso ya se probo sin querer, porque es lo
				 * que hacia decStop, y la partida siguiente heredaba el
				 * mismo problema. */
				close_decoder();

				inflight = 0;
				sub_head = sub_tail = 0;
				last_out_us = 0;
				pic_ready = 0;

				/* En vivo, lo que hubiera encolado ya no vale: son
				 * unidades de mitad de un GOP y el descodificador nuevo
				 * no puede empezar por ahi. Se tiran y se pide un IDR. */
				if (live_mode) {
					LOCK();
					live_w = live_r = live_f = 0;
					live_q = live_v = 0;
					UNLOCK();
				}

				if (open_decoder() != 0) continue;   /* fail() ya dentro */

				if (vdecStartSequence(handle) != 0) {
					fail("vdecStartSequence tras el atasco");
					continue;
				}

				t_last_progress = now_us();
#if !DEC_FREE_RUN
				next_due = 0;
#endif
				quiere_clave = 1;

				LOCK();
				info.state = DEC_PLAYING;
				info.errors++;
				UNLOCK();

				dlog("reabierto tras el atasco; pidiendo fotograma clave");
				continue;
			}
			usleep(200);
			continue;
		}

#if !DEC_FREE_RUN
		{
			u64 t = now_us();
			if (next_due == 0) next_due = t;
			if (t < next_due) { usleep(1000); continue; }
			next_due += DEC_FRAME_US;
			if (next_due < t) next_due = t;   /* nos hemos quedado atras */
		}
#endif

		/* 3. Enviar la siguiente. NO se espera la imagen: mientras el SPU
		 *    masca esta, la PPU ya le esta dando la siguiente. Eso es toda
		 *    la diferencia entre 23 ms por frame y 23 ms UNA vez. */
		memset(&au, 0, sizeof(au));
		if (live_mode) {
			/* La ranura no se suelta aqui: sigue siendo de cellVdec
			 * hasta que salga su imagen. */
			au.packet_addr = (u32)(uintptr_t)live[live_r].datos;
			au.packet_size = live[live_r].len;
		} else {
			au.packet_addr = (u32)(uintptr_t)(au_stream + aus[idx].off);
			au.packet_size = aus[idx].len;
		}
		au.pts.low = VDEC_TS_INVALID;
		au.pts.hi  = VDEC_TS_INVALID;
		au.dts.low = VDEC_TS_INVALID;
		au.dts.hi  = VDEC_TS_INVALID;
		au.userdata = idx;

		now = now_us();

		ret = vdecDecodeAu(handle, VDEC_DECODER_MODE_NORMAL, &au);
		if (ret != 0) {
			/* BUSY significa que cmd_depth es menor de lo que creiamos.
			 * Se baja el techo y se sigue: mas vale una tuberia corta
			 * que un fallo. */
			if (ret == (s32)VDEC_ERROR_BUSY) {
				if (inflight_max > 1) {
					inflight_max--;
					LOCK(); info.inflight_max = inflight_max; UNLOCK();
				}
				usleep(200);
				continue;
			}
			fail("vdecDecodeAu fallo: 0x%08x", (unsigned)ret);
			continue;
		}

		submit_t[sub_head] = now;
		sub_head = (sub_head + 1) % DEC_INFLIGHT;
		inflight++;

		if (live_mode) {
			live_r = (live_r + 1) % DEC_LIVE_SLOTS;
			LOCK(); live_q--; live_v++; UNLOCK();
		}

		LOCK();
		info.inflight = inflight;
		if (!live_mode)
			info.au_index = (info.au_index + 1) % info.au_count;
		UNLOCK();
	}

	sysThreadExit(0);
}

/* --------------------------------------------------------------------- */
/* API publica                                                           */
/* --------------------------------------------------------------------- */

int decInit(void)
{
	sys_mutex_attr_t attr;
	s32 ret;

	memset(&info, 0, sizeof(info));
	info.state = DEC_IDLE;

	sysMutexAttrInitialize(attr);
	if (sysMutexCreate(&mtx, &attr) == 0) mtx_ok = 1;

	/* AQUI ESTABA EL CUELGUE.
	 *
	 * En PS3 cellVdec viene partido en dos modulos: el nucleo (libvdec)
	 * y uno POR CODEC (libvdec_h264). Hay que cargar los dos antes de
	 * tocar nada.
	 *
	 * Y a diferencia de netInitialize(), que carga el suyo por dentro,
	 * libvdec no tiene wrapper en PSL1GHT que lo haga por nosotros. Sin
	 * los modulos cargados, vdecQueryAttr(codec=AVC) no tiene a quien
	 * preguntar y NO VUELVE — ni siquiera devuelve error.
	 *
	 * RPCS3 nunca lo vio porque su cellVdec es HLE: intercepta la llamada
	 * por su identificador, con modulos cargados o sin ellos.
	 *
	 * ERR_DUPLICATE (ya cargado) no es un fallo. */
	ret = sysModuleLoad(SYSMODULE_VDEC);
	dlog("sysModuleLoad(VDEC) -> 0x%08x%s", (unsigned)ret,
	     (ret == 0 || (u32)ret == SYSMODULE_ERR_DUPLICATE) ? " ok" : " FALLO");

	ret = sysModuleLoad(SYSMODULE_VDEC_H264);
	dlog("sysModuleLoad(VDEC_H264) -> 0x%08x%s", (unsigned)ret,
	     (ret == 0 || (u32)ret == SYSMODULE_ERR_DUPLICATE) ? " ok" : " FALLO");

	/* Una ranura por imagen en vuelo. Se reservan al tamano MAXIMO porque
	 * el tamano real no se sabe hasta leer el SPS, y descubrirlo tarde
	 * con un buffer corto es justo el cuelgue que estamos evitando. */
	{
		int i;

		for (i = 0; i < DEC_SLOTS; i++) {
			slots[i].pix = (u8*)memalign(128, DEC_PIC_BYTES);
			if (!slots[i].pix) {
				fail("sin memoria para %d imagenes de %u KB",
				     DEC_SLOTS, (unsigned)(DEC_PIC_BYTES / 1024));
				return -1;
			}
			memset(slots[i].pix, 0, DEC_PIC_BYTES);
		}

		dlog("%d ranuras de imagen, %u KB en total", DEC_SLOTS,
		     (unsigned)((u64)DEC_SLOTS * DEC_PIC_BYTES / 1024));
	}

	return 0;
}

/* Cierra de verdad: secuencia, handle y memoria. Solo desde el hilo del
 * decodificador. */
static void close_decoder(void)
{
	if (!opened) return;

	vdecEndSequence(handle);
	vdecClose(handle);
	opened = 0;
	handle = 0;

	if (vdec_mem) { free(vdec_mem); vdec_mem = NULL; }
}

static int open_decoder(void)
{
	vdecType type;
	vdecAttr vattr;
	vdecConfig cfg;
	vdecClosure cl;
	s32 ret;

	/* Cambiar el numero de SPUs obliga a reabrir: va en vdecConfig y
	 * vdecConfig solo se lee al abrir. */
	if (opened && cur_spus != req_spus) {
		dlog("reabriendo: %u SPUs -> %u", (unsigned)cur_spus,
		     (unsigned)req_spus);
		close_decoder();
	}

	if (opened) return 0;

	type.codec_type    = VDEC_CODEC_TYPE_H264;
	type.profile_level = DEC_PROFILE_LEVEL;

	memset(&vattr, 0, sizeof(vattr));

	dlog("1/4 vdecQueryAttr(codec=%u level=%u)...",
	     (unsigned)type.codec_type, (unsigned)type.profile_level);
	ret = vdecQueryAttr(&type, &vattr);
	if (ret != 0) {
		fail("vdecQueryAttr: 0x%08x", (unsigned)ret);
		return -1;
	}
	dlog("1/4 ok: mem %u KB, cmd_depth %u, ver %u.%u",
	     (unsigned)(vattr.mem_size / 1024), (unsigned)vattr.cmd_depth,
	     (unsigned)vattr.ver_major, (unsigned)vattr.ver_minor);

	/* cmd_depth es cuantas unidades de acceso acepta cellVdec sin haber
	 * devuelto imagen. Ese es el ancho de la tuberia, y no me lo invento:
	 * lo dice el propio decodificador. Se deja una de margen porque
	 * llenarla del todo hace que vdecDecodeAu conteste BUSY constantemente
	 * y eso es girar en vano. */
	inflight_max = vattr.cmd_depth > 1 ? vattr.cmd_depth - 1 : 1;
	if (inflight_max > DEC_INFLIGHT - 1) inflight_max = DEC_INFLIGHT - 1;
	dlog("1/4 tuberia: hasta %u unidades en vuelo", (unsigned)inflight_max);

	LOCK(); info.inflight_max = inflight_max; UNLOCK();

	dlog("2/4 reservando %u KB...", (unsigned)(vattr.mem_size / 1024));
	vdec_mem = memalign(128, vattr.mem_size);
	if (!vdec_mem) {
		fail("sin memoria para el decodificador (%u KB)",
		     (unsigned)(vattr.mem_size / 1024));
		return -1;
	}
	dlog("2/4 ok en %p", vdec_mem);

	memset(&cfg, 0, sizeof(cfg));
	cfg.mem_addr              = (u32)(uintptr_t)vdec_mem;
	cfg.mem_size              = vattr.mem_size;
	cfg.ppu_thread_prio       = VDEC_PPU_PRIO;
	cfg.ppu_thread_stack_size = VDEC_PPU_STACK;
	cfg.spu_thread_prio       = VDEC_SPU_PRIO;
	cfg.num_spus              = req_spus;

	/* AQUI. En PPC64 un puntero a funcion es un descriptor, y libvdec no
	 * tiene wrapper que lo convierta. Sin __get_opd32 esto revienta de
	 * una forma preciosamente dificil de diagnosticar. */
	cl.fn  = opd32_of(dec_callback);
	cl.arg = 0;

	dlog("3/4 vdecOpen(spus=%u prio_spu=%u prio_ppu=%u opd=0x%08x)...",
	     (unsigned)cfg.num_spus, (unsigned)cfg.spu_thread_prio,
	     (unsigned)cfg.ppu_thread_prio, (unsigned)cl.fn);

	ret = vdecOpen(&type, &cfg, &cl, &handle);
	if (ret != 0) {
		fail("vdecOpen: 0x%08x", (unsigned)ret);
		free(vdec_mem);
		vdec_mem = NULL;
		return -1;
	}

	opened = 1;
	cur_spus = req_spus;
	LOCK(); info.spus = cur_spus; UNLOCK();
	dlog("3/4 ok, handle %u", (unsigned)handle);
	return 0;
}

/* No bloquea. Deja la peticion puesta y el hilo se encarga: si cellVdec
 * tarda o no vuelve, la interfaz sigue viva y puede contarlo. */
int decStart(const u8 *stream, u32 size)
{
	int i;

	if (!slots[0].pix) return -1;

	LOCK();
	info.state = DEC_OPENING;
	info.err[0] = '\0';
	info.frames_decoded = info.frames_shown = info.errors = 0;
	info.decode_us_last = info.decode_us_avg = 0;
	info.decode_us_min = info.decode_us_max = 0;
	info.au_index = 0;
	info.au_count = 0;
	info.drops = 0;
	info.pace_us_last = info.pace_us_avg = info.pace_us_min = 0;
	info.inflight = 0;

	for (i = 0; i < DEC_SLOTS; i++) {
		slots[i].full = 0;
		slots[i].busy = 0;
		slots[i].seq  = 0;
	}
	out_seq = 0;

	sub_head = sub_tail = 0;
	inflight = 0;
	last_out_us = 0;
	pace_sum = 0;
	pace_n = 0;
	lat_sum = 0;
	lat_n = 0;
	UNLOCK();

	/* Siguiente escalon de la escalera de SPUs. Se avanza en cada entrada
	 * al titulo, asi que tres visitas dan la tabla completa. */
	spu_step = (spu_step + 1) % SPU_LADDER_N;
	req_spus = spu_ladder[spu_step];

	req_stream = stream;
	req_size = size;
	live_mode = (stream == NULL);

	if (!dec_started) {
		dec_running = 1;
		if (sysThreadCreate(&dec_tid, dec_thread, NULL, DEC_THREAD_PRIO,
		                    DEC_THREAD_STACK, THREAD_JOINABLE,
		                    "GR33N decoder") == 0) {
			dec_started = 1;
		} else {
			dec_running = 0;
			fail("sysThreadCreate del decodificador fallo");
			return -1;
		}
	}

	start_req = 1;
	return 0;
}

/* Arranca en modo EN VIVO: sin clip, esperando que la red empuje.
 *
 * Es el mismo decStart con stream NULL, y no una funcion aparte, para que
 * la puesta a cero del estado --las once lineas de arriba-- no exista dos
 * veces. Un reinicio a medias es de los fallos que solo se ven tres
 * titulos despues. */
int decStartLive(void)
{
	return decStart(NULL, 0);
}

/* Una unidad de acceso desde la red. La llama el hilo de WebRTC.
 *
 * Devuelve 0 si se ha encolado, -1 si no habia sitio o no cabia. Y no
 * recorta NUNCA: una unidad a medias tiene forma de unidad y cellVdec la
 * intentaria -- un aviso no arregla un dato corrupto. */
int decFeedAu(const u8 *au, u32 n)
{
	u32 hueco;

	if (!live_mode || au == NULL || n == 0) return -1;

	if (n > DEC_LIVE_MAX) {
		LOCK(); live_drops++; UNLOCK();
		return -1;
	}

	LOCK();
	hueco = DEC_LIVE_SLOTS - (live_q + live_v);
	UNLOCK();

	if (hueco == 0) {
		/* Sin ranuras: el decodificador no da abasto. Se tira la que
		 * llega, que es la mas nueva, y se cuenta. Tirar la mas vieja
		 * seria peor: esa ya puede estar dentro de cellVdec. */
		LOCK(); live_drops++; UNLOCK();
		return -1;
	}

	memcpy(live[live_w].datos, au, n);
	live[live_w].len = n;
	live_w = (live_w + 1) % DEC_LIVE_SLOTS;

	LOCK(); live_q++; UNLOCK();
	return 0;
}

u32 decLiveDrops(void)
{
	return live_drops;
}

/* No toca cellVdec: deja la peticion y el hilo la atiende entre envios.
 * Un vdecEndSequence desde aqui, con el hilo del decodificador a media
 * unidad de acceso, es exactamente el 0x80610102 que salio en el log. */
int decQuiereClave(void)
{
	int r = quiere_clave;
	quiere_clave = 0;
	return r;
}

void decStop(void)
{
	int i;

	LOCK();
	if (info.state == DEC_PLAYING) info.state = DEC_IDLE;
	for (i = 0; i < DEC_SLOTS; i++) slots[i].full = 0;
	UNLOCK();

	if (dec_started) stop_req = 1;
}

void decShutdown(void)
{
	if (dec_started) {
		u64 rv = 0;
		dec_running = 0;
		sysThreadJoin(dec_tid, &rv);
		dec_started = 0;
	}

	if (opened) {
		vdecEndSequence(handle);
		vdecClose(handle);
		opened = 0;
	}

	if (vdec_mem) { free(vdec_mem); vdec_mem = NULL; }

	{
		int i;
		for (i = 0; i < DEC_SLOTS; i++) {
			if (slots[i].pix) { free(slots[i].pix); slots[i].pix = NULL; }
		}
	}

	sysModuleUnload(SYSMODULE_VDEC_H264);
	sysModuleUnload(SYSMODULE_VDEC);

	if (mtx_ok) { sysMutexDestroy(mtx); mtx_ok = 0; }
}

/* Se lleva la imagen MAS RECIENTE, no la mas vieja, y tira las que se
 * hayan quedado por el camino.
 *
 * En un reproductor eso seria un pecado; en un cliente de streaming es lo
 * correcto. Mostrar un frame viejo porque estaba en la cola es anadir
 * latencia a cambio de nada: nadie quiere ver lo que pasaba hace 30 ms. */
int decTakePicture(gr33nSurface *s)
{
	u32 w, h, stride, y;
	const u8 *src_pix;
	int i, sl = -1, skipped = 0;
	u64 newest = 0;

	if (!s || !slots[0].pix) return 0;

	LOCK();
	for (i = 0; i < DEC_SLOTS; i++) {
		if (!slots[i].full || slots[i].busy) continue;
		if (sl < 0 || slots[i].seq > newest) {
			if (sl >= 0) skipped++;
			sl = i;
			newest = slots[i].seq;
		} else {
			skipped++;
		}
	}
	if (sl >= 0) {
		slots[sl].busy = 1;         /* reservada mientras se copia */
		w = slots[sl].w;
		h = slots[sl].h;
		src_pix = slots[sl].pix;
		/* Las mas viejas se sueltan ya: no las va a ver nadie. */
		for (i = 0; i < DEC_SLOTS; i++)
			if (i != sl && slots[i].full && !slots[i].busy) slots[i].full = 0;
		info.drops += (u32)skipped;
	} else {
		w = h = 0;
		src_pix = NULL;
	}
	UNLOCK();

	if (sl < 0 || w == 0 || h == 0) return 0;

	stride = s->pitch / 4;

	if (w == s->width && h >= s->height) {
		/* Camino rapido: mismo ancho, copia de filas directa. */
		for (y = 0; y < s->height; y++)
			copyBytes(s->pixels + y * stride,
			          src_pix + (size_t)y * w * 4, s->pitch);
	} else {
		/* Escalado por vecino mas cercano. Una fila calculada por fila
		 * origen distinta, igual que el resto del proyecto. */
		static u32 row[GR33N_SURFACE_W] __attribute__((aligned(128)));
		u32 last_src = 0xffffffffu;

		for (y = 0; y < s->height; y++) {
			u32 sy = (y * h) / s->height;

			if (sy != last_src) {
				const u32 *src = (const u32*)(src_pix + (size_t)sy * w * 4);
				u32 x;
				for (x = 0; x < s->width; x++)
					row[x] = src[(x * w) / s->width];
				last_src = sy;
			}

			copyBytes(s->pixels + y * stride, row, s->pitch);
		}
	}

	LOCK();
	slots[sl].full = 0;
	slots[sl].busy = 0;
	info.frames_shown++;
	UNLOCK();

	return 1;
}

const decInfo *decStatus(void)
{
	static decInfo snap;

	LOCK();
	snap = info;
	UNLOCK();

	return &snap;
}
