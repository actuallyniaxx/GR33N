/* GR33N - cliente de streaming para PS3
 *
 * Interfaz con pestanas, biblioteca y ajustes.
 *
 *   Biblioteca  ->  X sobre un titulo  ->  pantalla completa
 *                   SELECT + O para volver
 *   Ajustes     ->  Debug: panel de estadisticas + VIDEO_DEBUG
 *
 * Titulos:
 *   VIDEO_DEBUG  carta de ajuste local, sin red
 *   H264_DEBUG   clip empotrado por cellVdec, con la tuberia llena
 *   LOOP_TEST    el lazo completo contra el PC (aparece con el enlace UP)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>

#include <ppu-types.h>
#include <ppu-asm.h>
#include <sys/process.h>
#include <sys/systime.h>
#include <sys/spu.h>
#include <sysutil/sysutil.h>

#include "gr33n.h"
#include "video.h"
#include "input.h"
#include "link.h"
#include "decoder.h"
#include "text.h"
#include "ui.h"
#include "copy.h"
#include "store.h"
#include "net_tls.h"
#include "auth.h"
#include "imgdec.h"
#include "catalog.h"
#include "session.h"
#include "webrtc.h"
#include "ping.h"
#include "aud.h"
#include "i18n.h"

/* Generada por el Makefile a partir de data/clip.h264 (bin2s). */
#include "clip_h264.h"

SYS_PROCESS_PARAM(1001, 0x100000)

#define E2E_WINDOW      120         /* muestras antes de refrescar min/max/avg */
#define FRAME_STALE_US  500000ull   /* medio segundo sin frame -> patron */
#define FRAME_WAIT_US   4000        /* techo de espera al frame del servidor */

static volatile int running = 1;

/* Buffer de una fila. Vive en memoria principal (cacheada): construir la
 * fila aqui y volcarla de golpe es bastante mas rapido que escribir pixel
 * a pixel en la superficie.
 *
 * Alineado a 128 para que copyBytes pueda tirar de la unidad vectorial:
 * vec_ld ignora los cuatro bits bajos de la direccion, asi que un origen
 * desalineado no es lento, es incorrecto - y copyBytes lo comprueba y se
 * cae al camino escalar. Con esto no se cae. */
static u32 rowbuf[GR33N_SURFACE_W] __attribute__((aligned(128)));

typedef struct {
	u32 last, avg, min, max;
	int valid;
} e2eStats;

static u64 now_us(void)
{
	u64 sec = 0, nsec = 0;
	sysGetCurrentTime(&sec, &nsec);
	return sec * 1000000ull + nsec / 1000ull;
}

/* --------------------------------------------------------------------- */
/* Cronometraje del frame por fases                                      */
/*                                                                       */
/* Se mide con el registro de timebase (__gettime), no con               */
/* sysGetCurrentTime: aquel es una lectura de registro y este una llamada */
/* al sistema. Cinco llamadas al sistema por frame para medir el frame    */
/* seria medir el termometro.                                            */
/*                                                                       */
/* Existe porque una hipotesis mia sobre el rendimiento del menu resulto  */
/* ser falsa y adivinar dos veces seguidas es un metodo, pero malo.       */
/* --------------------------------------------------------------------- */

static u64 tb_hz = 79800000ull;   /* se pisa en main() con el valor real */

typedef struct {
	u64 surf, cont, hud, flip;   /* acumuladores en ticks */
	u32 n;
	u32 ms_surf, ms_cont, ms_hud, ms_flip;   /* medias en microsegundos */
} phaseStats;

static u32 tb_to_us(u64 ticks)
{
	return (u32)((ticks * 1000000ull) / tb_hz);
}

/* Cuanto cuesta escribir en la superficie, medido y no supuesto.
 *
 * El log de hardware dice que el contenido tarda 13 ms en escribir 3,7 MB
 * (unos 280 MB/s) tanto con la superficie en VRAM como en memoria
 * principal. Eso es lentisimo para RAM cacheada, asi que o la memoria no
 * se comporta como cacheada, o el memcpy de newlib es malo. Estas tres
 * medidas lo separan:
 *
 *   ram  : cacheada -> cacheada        (referencia del memcpy)
 *   surf : cacheada -> superficie      (coste real del volcado)
 *   loop : bucle de palabras -> superficie
 */
typedef struct {
	u32 mb, ram, surf, loop, fast, fill, fillmc;
} benchSet;

/* Dos juegos de medidas de lo MISMO, tomadas en dos momentos distintos:
 * una al arrancar, con la consola practicamente parada, y otra en pleno
 * bucle de dibujo con el RSX trabajando.
 *
 * Existe porque los numeros del arranque y los del frame no cuadran: el
 * banco de pruebas dice que fila+memcpy mueve 3,5 MB en 1,27 ms, pero el
 * mismo codigo dentro del frame tardaba 10. Tengo hipotesis y ninguna me
 * convence, asi que en vez de elegir la que mas me guste, mido las dos
 * veces y que hable el log. */
static benchSet bench1, bench2;
static int bench2_done = 0;

/* Contador PROPIO. La primera version usaba `frames`, que ya era el
 * contador de fps y se pone a cero cada segundo: nunca llegaba a 300 y la
 * segunda medida no se tomo jamas. Reutilizar una variable que ya tenia
 * dueno es de las formas mas tontas de perder una prueba. */
static u32 bench_frames = 0;

static void bench_memory(gr33nSurface *s, benchSet *o)
{
	u32 bytes = s->pitch * s->height;
	u8 *src = (u8*)memalign(128, bytes);
	u8 *dst = (u8*)memalign(128, bytes);
	u64 t;

	if (!src || !dst) {
		/* Antes esto se rendia en silencio y el informe no salia, lo que
		 * parecia que el codigo no se habia ejecutado. Un fallo mudo es
		 * peor que un fallo. */
		linkLog("!! banco de pruebas: sin memoria para 2 x %u KB",
		        (unsigned)(bytes / 1024));
		free(src);
		free(dst);
		return;
	}

	memset(src, 0x5a, bytes);
	memset(dst, 0, bytes);

	t = __gettime();
	memcpy(dst, src, bytes);
	o->ram = tb_to_us(__gettime() - t);

	t = __gettime();
	memcpy(s->pixels, src, bytes);
	o->surf = tb_to_us(__gettime() - t);

	t = __gettime();
	{
		u32 i, n = bytes / 4;
		u32 *p = s->pixels;
		for (i = 0; i < n; i++) p[i] = 0;
	}
	o->loop = tb_to_us(__gettime() - t);

	/* La rutina nueva midiendo exactamente lo mismo que bench_surf. Estan
	 * las dos aqui a proposito: la comparacion tiene que salir del mismo
	 * log, no de mi palabra contra la de la sesion anterior. */
	t = __gettime();
	copyBytes(s->pixels, src, bytes);
	o->fast = tb_to_us(__gettime() - t);

	/* Y el relleno, que es lo que de verdad hace la interfaz. Primero
	 * como se hacia antes (montar fila + memcpy, dos escrituras por
	 * pixel) y luego como se hace ahora. */
	t = __gettime();
	{
		u32 y, w = s->width, stride = s->pitch / 4;
		for (y = 0; y < s->height; y++)
			memcpy(s->pixels + y * stride, src, (size_t)w * 4);
	}
	o->fillmc = tb_to_us(__gettime() - t);

	t = __gettime();
	{
		u32 y, w = s->width, stride = s->pitch / 4;
		for (y = 0; y < s->height; y++)
			fillWords(s->pixels + y * stride, 0xff101418, w);
	}
	o->fill = tb_to_us(__gettime() - t);

	o->mb = bytes;

	free(src);
	free(dst);
}

/* Un solo sitio que sepa dar formato a un juego de medidas, para que las
 * dos tomas sean comparables linea a linea. */
static void bench_report(const char *cuando, const benchSet *o)
{
	if (!o->mb) return;

	linkLog("=== [%s] copiar %u KB: memcpy ram->ram %u us (%u MB/s) | "
	        "memcpy ram->superf %u us (%u MB/s) | bucle->superf %u us "
	        "(%u MB/s) | copyBytes ram->superf %u us (%u MB/s) ===",
	        cuando, (unsigned)(o->mb / 1024),
	        (unsigned)o->ram,    (unsigned)(o->ram    ? o->mb / o->ram    : 0),
	        (unsigned)o->surf,   (unsigned)(o->surf   ? o->mb / o->surf   : 0),
	        (unsigned)o->loop,   (unsigned)(o->loop   ? o->mb / o->loop   : 0),
	        (unsigned)o->fast,   (unsigned)(o->fast   ? o->mb / o->fast   : 0));

	linkLog("=== [%s] rellenar %u KB: fila+memcpy %u us (%u MB/s) | "
	        "fillWords %u us (%u MB/s) ===",
	        cuando, (unsigned)(o->mb / 1024),
	        (unsigned)o->fillmc, (unsigned)(o->fillmc ? o->mb / o->fillmc : 0),
	        (unsigned)o->fill,   (unsigned)(o->fill   ? o->mb / o->fill   : 0));
}

static void phase_tick(phaseStats *p)
{
	if (++p->n < 30) return;

	p->ms_surf = tb_to_us(p->surf / p->n);
	p->ms_cont = tb_to_us(p->cont / p->n);
	p->ms_hud  = tb_to_us(p->hud  / p->n);
	p->ms_flip = tb_to_us(p->flip / p->n);

	p->surf = p->cont = p->hud = p->flip = 0;
	p->n = 0;
}

/* --------------------------------------------------------------------- */
/* Callbacks del sistema                                                 */
/* --------------------------------------------------------------------- */

static void sysutil_callback(u64 status, u64 param, void *usrdata)
{
	(void)param;
	(void)usrdata;

	if (status == SYSUTIL_EXIT_GAME)
		running = 0;
}

static void exit_callback(void)
{
	sysUtilUnregisterCallback(SYSUTIL_EVENT_SLOT0);
	videoShutdown();
}

/* --------------------------------------------------------------------- */
/* VIDEO_DEBUG: la carta de ajuste                                       */
/* --------------------------------------------------------------------- */

static const u32 bars[8] = {
	GR33N_RGB(255,255,255),
	GR33N_RGB(255,255,  0),
	GR33N_RGB(  0,255,255),
	GR33N_RGB(  0,255,  0),
	GR33N_RGB(255,  0,255),
	GR33N_RGB(255,  0,  0),
	GR33N_RGB(  0,  0,255),
	GR33N_RGB( 16, 16, 16)
};

static void draw_pattern(gr33nSurface *s, u32 tick)
{
	u32 stride = s->pitch / 4;
	u32 bars_h = (s->height * 2) / 3;
	u32 x, y;

	for (x = 0; x < s->width; x++)
		rowbuf[x] = bars[(x * 8) / s->width];
	for (y = 0; y < bars_h; y++)
		copyBytes(s->pixels + y * stride, rowbuf, s->pitch);

	for (x = 0; x < s->width; x++) {
		u32 v = ((x + tick * 2) * 256 / s->width) & 0xff;
		rowbuf[x] = GR33N_RGB(v, v >> 1, 255 - v);
	}
	for (y = bars_h; y < s->height; y++)
		copyBytes(s->pixels + y * stride, rowbuf, s->pitch);

	gfxFillRect(s, (int)((tick * 8) % s->width), 0, 6, (int)s->height,
	            GR33N_RGB(255,255,255));
	gfxRect(s, 0, 0, (int)s->width, (int)s->height, 3, GR33N_RGB(0,255,64));
}

/* --------------------------------------------------------------------- */
/* LOOP_TEST: el frame que llega del PC                                  */
/* --------------------------------------------------------------------- */

/* Escalado por vecino mas cercano de la rejilla a la superficie. Solo se
 * recalcula la fila cuando cambia la fila origen: 14 calculos de fila y
 * 720 memcpy, en vez de 921.600 lookups. */
static void draw_frame(gr33nSurface *s, const linkFrame *f)
{
	u32 stride = s->pitch / 4;
	u32 last_src = 0xffffffffu;
	u32 x, y;

	for (y = 0; y < s->height; y++) {
		u32 sy = (y * f->rows) / s->height;

		if (sy != last_src) {
			const u8 *src = f->rgb + (sy * f->cols) * 3;
			for (x = 0; x < s->width; x++) {
				const u8 *p = src + ((x * f->cols) / s->width) * 3;
				rowbuf[x] = GR33N_RGB(p[0], p[1], p[2]);
			}
			last_src = sy;
		}

		copyBytes(s->pixels + y * stride, rowbuf, s->pitch);
	}

	gfxRect(s, 0, 0, (int)s->width, (int)s->height, 3, GR33N_RGB(0,255,64));
}

static void draw_marker(gr33nSurface *s, int mx, int my, const gr33nPad *pad)
{
	u32 col = (pad->held & GR33N_BTN_CROSS) ? GR33N_RGB(0,255,64)
	                                        : GR33N_RGB(255,255,255);
	gfxFillRect(s, mx - 24, my - 2, 48, 4, col);
	gfxFillRect(s, mx - 2, my - 24, 4, 48, col);
	gfxRect(s, mx - 32, my - 32, 64, 64, 2, col);
}

/* --------------------------------------------------------------------- */
/* H264_DEBUG: estados sin imagen                                        */
/* --------------------------------------------------------------------- */

static void draw_dec_message(gr33nSurface *s, u32 color,
                             const char *title, const char *detail)
{
	int w = (int)s->width, h = (int)s->height;

	gfxFillRect(s, 0, 0, w, h, GR33N_RGB(8, 10, 9));
	gfxRect(s, 0, 0, w, h, 3, color);

	textDraw(s, (w - textWidth(4, title)) / 2, h / 2 - 60, 4, color, title);

	if (detail && detail[0])
		textWrap(s, w / 2 - 380, h / 2, 2, GR33N_RGB(200, 210, 205),
		         760, 28, 4, detail);
}

/* --------------------------------------------------------------------- */
/* TLS_DEBUG: la pantalla de la peticion                                 */
/* --------------------------------------------------------------------- */

static const char *tls_state_name(tlsState st)
{
	switch (st) {
	case TLS_RESOLVING:  return "Resolviendo el nombre";
	case TLS_CONNECTING: return "Abriendo el socket";
	case TLS_HANDSHAKE:  return "Negociando TLS";
	case TLS_REQUEST:    return "Pidiendo el documento";
	case TLS_DONE:       return "Listo";
	case TLS_FAILED:     return "Fallo";
	default:             return "En espera";
	}
}

/* Pie de la pantalla de cuenta. Aqui SIEMPRE se puede volver con Atras,
 * asi que se dice. Una pantalla de la que no sabes salir es una trampa. */
static void draw_auth_hints(gr33nSurface *s, const authInfo *a)
{
	int w = (int)s->width, h = (int)s->height;
	char hints[96];
	int tw;

	if (a->state == AUTH_WAITING)
		snprintf(hints, sizeof(hints), "%s  Cancelar y volver",
		         (uiBtnBack() == GR33N_BTN_CROSS) ? "X" : "O");
	else if (a->state == AUTH_IDLE || a->state == AUTH_FAILED)
		snprintf(hints, sizeof(hints), "%s  Empezar        %s  Volver",
		         (uiBtnOk()   == GR33N_BTN_CROSS) ? "X" : "O",
		         (uiBtnBack() == GR33N_BTN_CROSS) ? "X" : "O");
	else
		snprintf(hints, sizeof(hints), "%s  Volver",
		         (uiBtnBack() == GR33N_BTN_CROSS) ? "X" : "O");

	tw = textWidth(2, hints);

	gfxFillRect(s, 0, h - 56, w, 56, GR33N_RGB(6, 12, 10));
	gfxFillRect(s, 0, h - 56, w, 2, GR33N_RGB(40, 50, 45));
	textDraw(s, (w - tw) / 2, h - 56 + 18, 2, GR33N_RGB(150, 170, 165), hints);
}

static void draw_tls_screen(gr33nSurface *s)
{
	const tlsInfo *t = tlsStatus();
	int w = (int)s->width, h = (int)s->height;
	int x = 120, y = 120;
	u32 accent = (t->state == TLS_FAILED) ? GR33N_RGB(255, 80, 80)
	           : (t->state == TLS_DONE)   ? GR33N_RGB(0, 255, 64)
	                                      : GR33N_RGB(120, 160, 255);
	char line[160];

	gfxFillRect(s, 0, 0, w, h, GR33N_RGB(8, 10, 14));
	gfxRect(s, 0, 0, w, h, 3, accent);

	textDraw(s, x, y, 4, accent, tls_state_name(t->state));
	y += 64;

	snprintf(line, sizeof(line), "%s", t->host);
	textDraw(s, x, y, 2, GR33N_RGB(200, 210, 220), line);
	y += 40;

	if (t->state == TLS_FAILED) {
		textWrap(s, x, y, 2, GR33N_RGB(255, 160, 160), w - x * 2, 28, 4, t->err);
		return;
	}

	if (t->ip[0]) {
		snprintf(line, sizeof(line), "direccion   %s   (%u ms)",
		         t->ip, (unsigned)t->ms_resolve);
		textDraw(s, x, y, 2, GR33N_RGB(150, 165, 180), line); y += 30;
	}

	if (t->ms_connect) {
		snprintf(line, sizeof(line), "socket      %u ms", (unsigned)t->ms_connect);
		textDraw(s, x, y, 2, GR33N_RGB(150, 165, 180), line); y += 30;
	}

	if (t->version[0]) {
		snprintf(line, sizeof(line), "protocolo   %s   %s",
		         t->version, t->cipher);
		textDraw(s, x, y, 2, GR33N_RGB(200, 210, 220), line); y += 30;

		snprintf(line, sizeof(line), "apreton     %u ms",
		         (unsigned)t->ms_handshake);
		textDraw(s, x, y, 2, GR33N_RGB(150, 165, 180), line); y += 30;
	}

	if (t->peer[0]) {
		y += 10;
		textWrap(s, x, y, 2, GR33N_RGB(200, 210, 220),
		         w - x * 2, 28, 2, t->peer);
		y += 60;

		/* Lo mas importante de toda la pantalla: si la cadena valida o
		 * no. En verde o en rojo, sin medias tintas. */
		snprintf(line, sizeof(line), "certificado %s", t->verify_txt);
		textDraw(s, x, y, 3, t->verify_flags ? GR33N_RGB(255, 80, 80)
		                                     : GR33N_RGB(0, 255, 64), line);
		y += 46;
	}

	if (t->state == TLS_DONE) {
		snprintf(line, sizeof(line), "HTTP %d   %u bytes   %u ms",
		         t->http_status, (unsigned)t->body_len,
		         (unsigned)t->ms_request);
		textDraw(s, x, y, 3, accent, line);
		y += 46;

		{
			u32 n = 0;
			const char *b = tlsBody(&n);
			if (b) textWrap(s, x, y, 2, GR33N_RGB(130, 145, 160),
			                w - x * 2, 26, 6, b);
		}
	}
}

/* --------------------------------------------------------------------- */
/* INICIAR_SESION: el codigo, grande                                     */
/* --------------------------------------------------------------------- */

static void draw_auth_screen(gr33nSurface *s)
{
	const authInfo *a = authStatus();
	int w = (int)s->width, h = (int)s->height;
	u32 accent = (a->state == AUTH_FAILED) ? GR33N_RGB(255, 80, 80)
	           : (a->state == AUTH_OK)     ? GR33N_RGB(0, 255, 64)
	                                       : GR33N_RGB(0, 220, 160);
	char line[160];
	int y = 90;

	gfxFillRect(s, 0, 0, w, h, GR33N_RGB(8, 14, 12));
	gfxRect(s, 0, 0, w, h, 3, accent);

	switch (a->state) {
	case AUTH_IDLE: {
		/* SIN TILDES, Y NO ES DESCUIDO AL REVES.
		 *
		 * Aqui ponia "Iniciar sesion" CON tilde, y en la consola salia
		 * un hueco donde va la o: la fuente empotrada es ASCII 32..122 y
		 * no tiene ni una vocal acentuada. Llevaba asi desde que se
		 * escribio la pantalla; se ve al traducir porque hay que pasar
		 * por cada cadena una por una.
		 *
		 * En el resto del proyecto el castellano va sin tildes por esto
		 * mismo. Estas cuatro se habian colado. */
		const char *tit = TR("Iniciar sesion", "Sign in");

		textDraw(s, (w - textWidth(4, tit)) / 2, h / 2 - 90, 4, accent, tit);
		textWrap(s, w / 2 - 400, h / 2 - 20, 2, GR33N_RGB(190, 205, 200),
		         800, 30, 4,
		         authHaveToken()
		             ? TR("Ya hay una sesion guardada en esta consola. "
		                  "Pulsa X para volver a entrar con otra cuenta.",
		                  "There is already a session saved on this "
		                  "console. Press X to sign in with a different "
		                  "account.")
		             : TR("Pulsa X y la consola te dara un codigo corto "
		                  "para escribir en el movil o en el PC.",
		                  "Press X and the console will give you a short "
		                  "code to type on your phone or PC."));
		{
			const char *b = TR("X  Empezar", "X  Start");
			textDraw(s, (w - textWidth(3, b)) / 2, h / 2 + 130, 3, accent, b);
		}
		break;
	}

	case AUTH_REQUESTING: {
		const char *t = TR("Pidiendo codigo...", "Requesting code...");
		textDraw(s, (w - textWidth(4, t)) / 2, h / 2 - 20, 4, accent, t);
		break;
	}

	case AUTH_WAITING: {
		int cw;

		textDraw(s, 100, y, 3, GR33N_RGB(190, 205, 200),
		         TR("1.  Entra en esta direccion:",
		            "1.  Go to this address:"));
		y += 54;
		textDraw(s, 140, y, 4, GR33N_RGB(255, 255, 255), a->verify_uri);
		y += 80;

		textDraw(s, 100, y, 3, GR33N_RGB(190, 205, 200),
		         TR("2.  Escribe este codigo:",
		            "2.  Type this code:"));
		y += 60;

		/* El codigo, lo mas grande que quepa. Es lo unico que el usuario
		 * tiene que copiar a mano, posiblemente desde el sofa y a tres
		 * metros de la tele. */
		cw = textWidth(9, a->user_code);
		gfxFillRect(s, (w - cw) / 2 - 40, y - 20, cw + 80, 110,
		            GR33N_RGB(14, 24, 20));
		gfxRect(s, (w - cw) / 2 - 40, y - 20, cw + 80, 110, 3, accent);
		textDraw(s, (w - cw) / 2, y + 6, 9, accent, a->user_code);
		y += 140;

		snprintf(line, sizeof(line),
		         TR("Esperando...   %u s de %u   %u consultas",
		            "Waiting...   %u s of %u   %u polls"),
		         (unsigned)a->elapsed_s, (unsigned)a->expires_s,
		         (unsigned)a->polls);
		textDraw(s, 100, y, 2, GR33N_RGB(140, 160, 155), line);
		y += 36;

		break;
	}

	case AUTH_OK: {
		const char *t = TR("Sesion iniciada", "Signed in");

		textDraw(s, (w - textWidth(5, t)) / 2, h / 2 - 60, 5, accent, t);
		textWrap(s, w / 2 - 400, h / 2 + 30, 2, GR33N_RGB(190, 205, 200),
		         800, 30, 3,
		         TR("El token queda guardado en la consola. En el proximo "
		            "arranque no hara falta repetir esto.",
		            "The token is saved on the console. Next time you "
		            "start up you will not have to do this again."));
		break;
	}

	case AUTH_FAILED: {
		const char *t = TR("No se pudo entrar", "Sign-in failed");
		const char *b = TR("X  Reintentar", "X  Try again");

		textDraw(s, (w - textWidth(4, t)) / 2, h / 2 - 80, 4, accent, t);
		textWrap(s, w / 2 - 420, h / 2 - 10, 2, GR33N_RGB(255, 170, 170),
		         840, 30, 4, a->err);
		textDraw(s, (w - textWidth(3, b)) / 2, h / 2 + 150, 3, accent, b);
		break;
	}
	}

	draw_auth_hints(s, a);
}

/* --------------------------------------------------------------------- */
/* Panel de estadisticas (ajuste Debug)                                  */
/* --------------------------------------------------------------------- */

#define HUD_LINES  18
#define HUD_COLS   96

static const char *link_state_name(linkState st)
{
	switch (st) {
	case LINK_UP:        return "UP";
	case LINK_SEARCHING: return "buscando";
	default:             return "DOWN";
	}
}

static const char *dec_state_name(decState st)
{
	switch (st) {
	case DEC_OPENING: return "abriendo";
	case DEC_PLAYING: return "ok";
	case DEC_FAILED:  return "ERROR";
	default:          return "-";
	}
}

/* Microsegundos a "M.CC" milisegundos. En LAN el RTT vive en cientos de
 * microsegundos, asi que redondear a milisegundos enteros no sirve. */
static void fmt_ms(char *buf, size_t n, u32 us)
{
	snprintf(buf, n, "%u.%02u", (unsigned)(us / 1000u),
	         (unsigned)((us % 1000u) / 10u));
}

typedef struct {
	char line[HUD_LINES][HUD_COLS];
	u32  col[HUD_LINES];
	u32  n;   /* sin signo a proposito: la macro HUD_ADD propaga los
	           * incrementos y genera comparaciones "n + k < 18", el
	           * patron que dispara -Wstrict-overflow en ppu-gcc */
} hudLines;

/* El HUD se construye una vez por frame y sirve para dos cosas: pintarlo
 * en la tele y mandarlo al log del PC. De ahi que este separado del
 * dibujo: asi lo que ves en pantalla y lo que leo yo en el log no se
 * pueden separar nunca, por definicion. */
static void hud_build(hudLines *H, gr33nSurface *s, const gr33nPad *pad,
                      f32 fps, f32 ms, const e2eStats *e2e, int live,
                      const phaseStats *ph)
{
	const u32 fg   = GR33N_RGB(0, 255, 64);
	const u32 fg2  = GR33N_RGB(220, 220, 220);
	const u32 hot  = GR33N_RGB(0, 220, 255);
	const u32 warn = GR33N_RGB(255, 180, 0);

	const linkInfo *lk = linkStatus();

	char a[16], b[16], c[16];
	char btns[96];
	int i;
	size_t bn = 0;   /* indice sin signo: en aritmetica sin signo el
	                  * desbordamiento esta definido, asi que este
	                  * chequeo de limites no puede disparar
	                  * -Wstrict-overflow como el de ui.c */

	H->n = 0;

	#define HUD_ADD(colour, ...) do { \
		if (H->n < HUD_LINES) { \
			snprintf(H->line[H->n], HUD_COLS, __VA_ARGS__); \
			H->col[H->n] = (colour); \
			H->n++; \
		} \
	} while (0)

	HUD_ADD(fg, "%s %s", GR33N_NAME, GR33N_VERSION);

	HUD_ADD(fg2, "fps %d.%d  frame %d.%d ms",
	        (int)fps, ((int)(fps * 10)) % 10,
	        (int)ms, ((int)(ms * 10)) % 10);

	HUD_ADD(fg2, "display %dx%d  surface %dx%d %s",
	        (int)videoDisplayWidth(), (int)videoDisplayHeight(),
	        (int)s->width, (int)s->height,
	        videoSurfaceInMain() ? "[main]" : "[vram]");

	{
		char d[4][16];
		fmt_ms(d[0], sizeof(d[0]), ph->ms_surf);
		fmt_ms(d[1], sizeof(d[1]), ph->ms_cont);
		fmt_ms(d[2], sizeof(d[2]), ph->ms_hud);
		fmt_ms(d[3], sizeof(d[3]), ph->ms_flip);
		HUD_ADD(hot, "fase surf %s cont %s hud %s flip %s ms",
		        d[0], d[1], d[2], d[3]);
	}

	HUD_ADD(lk->state == LINK_UP ? fg2 : warn,
	        "net %s  ip %s", link_state_name(lk->state), lk->local_ip);

	if (lk->state == LINK_UP) {
		HUD_ADD(fg2, "srv %s %s",
		        lk->server_name[0] ? lk->server_name : "-", lk->server_ip);

		fmt_ms(a, sizeof(a), lk->rtt_last);
		fmt_ms(b, sizeof(b), lk->rtt_avg);
		fmt_ms(c, sizeof(c), lk->jitter);
		HUD_ADD(fg2, "rtt %s avg %s jit %s ms", a, b, c);

		fmt_ms(a, sizeof(a), lk->rtt_min);
		fmt_ms(b, sizeof(b), lk->rtt_max);
		HUD_ADD(fg2, "min %s max %s  loss %u%%  %u pps",
		        a, b, (unsigned)lk->loss_pct, (unsigned)lk->pps);

		HUD_ADD(live ? hot : fg2,
		        "video %s  %u fps  huecos %u",
		        live ? "live" : "-",
		        (unsigned)lk->fps_rx, (unsigned)lk->frame_gaps);

		if (live) {
			fmt_ms(a, sizeof(a), lk->wait_us);
			HUD_ADD(hot, "lag %u inputs  espera %s ms",
			        (unsigned)lk->frame_lag, a);
		}

		if (e2e->valid) {
			fmt_ms(a, sizeof(a), e2e->last);
			fmt_ms(b, sizeof(b), e2e->avg);
			HUD_ADD(hot, "e2e %s avg %s ms", a, b);

			fmt_ms(a, sizeof(a), e2e->min);
			fmt_ms(b, sizeof(b), e2e->max);
			HUD_ADD(hot, "e2e min %s max %s ms", a, b);
		}
	}

	{
		const decInfo *d = decStatus();

		if (d->state == DEC_FAILED) {
			HUD_ADD(warn, "vdec ERROR %.76s", d->err);
		} else if (d->state != DEC_IDLE) {
			HUD_ADD(hot, "vdec %s %ux%u  %u SPU  au %u/%u  dec %u  err %u",
			        dec_state_name(d->state),
			        (unsigned)d->width, (unsigned)d->height,
			        (unsigned)d->spus,
			        (unsigned)d->au_index, (unsigned)d->au_count,
			        (unsigned)d->frames_decoded, (unsigned)d->errors);

			if (d->state == DEC_PLAYING) {
				u32 pace = d->pace_us_avg ? d->pace_us_avg : d->pace_us_last;

				fmt_ms(a, sizeof(a), d->decode_us_last);
				fmt_ms(b, sizeof(b), d->decode_us_avg);
				HUD_ADD(hot, "latencia %s avg %s ms  (min %u max %u ms)",
				        a, b,
				        (unsigned)(d->decode_us_min / 1000),
				        (unsigned)(d->decode_us_max / 1000));

				/* EL NUMERO QUE DECIDE 720p60. La latencia dice cuanto
				 * tarda en aparecer el primer frame; el ritmo dice cuantos
				 * caben por segundo. Con la tuberia llena, el primero sube
				 * y el segundo baja. */
				fmt_ms(a, sizeof(a), pace);
				fmt_ms(b, sizeof(b), d->pace_us_min);
				HUD_ADD(hot, "ritmo %s ms = %u fps  (mejor %s ms)",
				        a, (unsigned)(pace ? 1000000u / pace : 0), b);

				HUD_ADD(fg2, "tuberia %u/%u en vuelo  descartes %u",
				        (unsigned)d->inflight, (unsigned)d->inflight_max,
				        (unsigned)d->drops);
			}
		}
	}

	{
		const xblProfile *pr = authProfile();

		if (pr->state == PROF_OK)
			HUD_ADD(fg2, "perfil %s  %u G  cadena %u ms",
			        pr->gamertag, (unsigned)pr->gamerscore,
			        (unsigned)pr->ms_total);
		else if (pr->state == PROF_FAILED)
			HUD_ADD(warn, "perfil ERROR %.70s", pr->err);
		else if (pr->state == PROF_WORKING)
			HUD_ADD(hot, "perfil cargando...");
	}

	{
		const authInfo *a = authStatus();

		if (a->state == AUTH_FAILED)
			HUD_ADD(warn, "auth ERROR %.76s", a->err);
		else if (a->state == AUTH_WAITING)
			HUD_ADD(hot, "auth codigo %s  %u/%u s  %u consultas",
			        a->user_code, (unsigned)a->elapsed_s,
			        (unsigned)a->expires_s, (unsigned)a->polls);
		else if (a->state == AUTH_OK)
			HUD_ADD(hot, "auth sesion iniciada");
	}

	{
		const tlsInfo *t = tlsStatus();

		if (t->state == TLS_FAILED) {
			HUD_ADD(warn, "tls ERROR %.78s", t->err);
		} else if (t->state != TLS_IDLE) {
			HUD_ADD(hot, "tls %s %s  %s",
			        t->version[0] ? t->version : "-",
			        t->cipher[0] ? t->cipher : "-",
			        t->verify_flags ? "CERT INVALIDO" : "cert ok");
			HUD_ADD(fg2, "tls dns %u  con %u  tls %u  http %u ms  -> %d",
			        (unsigned)t->ms_resolve, (unsigned)t->ms_connect,
			        (unsigned)t->ms_handshake, (unsigned)t->ms_request,
			        t->http_status);
		}
	}

	/* WebRTC. Va en el HUD y no en una pantalla propia por lo mismo que
	 * TLS: es una prueba de diagnostico, no una funcion del programa.
	 *
	 * Lo que se ensena mientras corre es EN QUE PASO VA, porque uno de
	 * ellos -la clave RSA- puede tardar varios segundos y sin esto la
	 * consola pareceria colgada. Ya nos paso con videoInit. */
	{
		const wrtcInfo *w = wrtcStatus();

		switch (w->state) {
		case WRTC_IDLE:
			break;
		case WRTC_MIDIENDO:
			HUD_ADD(hot, "webrtc midiendo esperas...");
			break;
		case WRTC_INIT:
			HUD_ADD(hot, "webrtc arrancando libpeer...");
			break;
		case WRTC_CREANDO:
			HUD_ADD(hot, "webrtc generando la clave RSA... (puede tardar)");
			break;
		case WRTC_OFERTA:
			HUD_ADD(hot, "webrtc armando la oferta SDP...");
			break;
		case WRTC_CERRANDO:
			HUD_ADD(hot, "webrtc cerrando...");
			break;
		case WRTC_FALLO:
			HUD_ADD(warn, "webrtc ERROR %.72s", w->err);
			break;
		case WRTC_LISTO:
			HUD_ADD(hot, "webrtc init %u  clave %u  oferta %u  cierre %u ms",
			        (unsigned)w->ms_init, (unsigned)w->ms_create,
			        (unsigned)w->ms_offer, (unsigned)w->ms_close);
			HUD_ADD(fg2, "webrtc usleep 1ms->%u us  netPoll 1ms->%u us",
			        (unsigned)w->us_usleep_1000,
			        (unsigned)w->us_poll_1000);
			HUD_ADD(fg2, "webrtc sdp %u B  102:%d fmtp:%d setup:%d fp:%d remb:%d",
			        (unsigned)w->sdp_len, w->sdp_pt102, w->sdp_fmtp,
			        w->sdp_setup_active, w->sdp_fingerprint, w->sdp_remb);
			HUD_ADD(fg2, "webrtc candidatos  host:%d  srflx:%d",
			        w->sdp_host, w->sdp_srflx);
			break;
		}
	}

	/* El servidor de depuracion vive aqui ahora, no en la barra
	 * superior: es informacion nuestra, no del usuario. */
	{
		const linkInfo *lk = linkStatus();

		if (lk->state == LINK_UP)
			HUD_ADD(fg2, "debug %s %s",
			        lk->server_name[0] ? lk->server_name : "servidor",
			        lk->server_ip);
	}

	HUD_ADD(fg2, "pad %s  port %d", pad->connected ? "ok" : "no", pad->port);

	HUD_ADD(fg2, "lx %d ly %d  rx %d ry %d",
	        (int)pad->lx, (int)pad->ly, (int)pad->rx, (int)pad->ry);

	btns[0] = '\0';
	for (i = 0; i < GR33N_BTN_COUNT; i++) {
		if (pad->held & (1u << i)) {
			const char *nm = inputButtonName(i);
			size_t len = strlen(nm);
			if (bn + len + 2 >= sizeof(btns)) break;
			if (bn) btns[bn++] = ' ';
			memcpy(btns + bn, nm, len);
			bn += len;
			btns[bn] = '\0';
		}
	}
	HUD_ADD(fg2, "btn %s", bn ? btns : "-");

	#undef HUD_ADD
}

/* El panel, con la escala y la esquina que diga el usuario.
 *
 * La escala no multiplica solo la letra: multiplica el alto de linea y
 * los margenes tambien. Escalar solo el texto es como se consiguen esos
 * paneles con las lineas pisandose unas a otras.
 *
 * Ya no hay "anclaje" fijo por modo. Antes el menu lo ponia arriba a la
 * izquierda y el stream abajo a la derecha, decidido por mi. Ahora lo
 * decide quien mira la pantalla, que para eso es su tele. */
static void hud_draw(gr33nSurface *s, const hudLines *H)
{
	const u32 fg   = GR33N_RGB(0, 255, 64);
	const u32 back = GR33N_RGB(0, 0, 0);

	int sc = uiHudScale();
	int pos = uiHudPos();
	int lh, pad, longest = 0, x, y0, bw, bh;
	u32 i;

	if (sc < 1) sc = 1;
	if (sc > 3) sc = 3;

	lh  = (TEXT_GLYPH_H + 4) * sc;
	pad = 6 * sc;

	for (i = 0; i < H->n; i++) {
		int len = textWidth(sc, H->line[i]);
		if (len > longest) longest = len;
	}

	bw = longest + pad * 2;
	bh = (int)H->n * lh + pad * 2;

	/* 72 abajo y no 24: la barra inferior del menu ocupa 48 y taparia
	 * las ultimas lineas. */
	switch (pos) {
	case 1:  x = (int)s->width - 24 - bw;  y0 = 24; break;
	case 2:  x = 24;                       y0 = (int)s->height - 72 - bh; break;
	case 3:  x = (int)s->width - 24 - bw;  y0 = (int)s->height - 72 - bh; break;
	default: x = 24;                       y0 = 24; break;
	}

	if (x < 8)  x = 8;
	if (y0 < 8) y0 = 8;

	gfxFillRect(s, x, y0, bw, bh, back);
	gfxRect(s, x, y0, bw, bh, 2, fg);

	for (i = 0; i < H->n; i++)
		textDraw(s, x + pad, y0 + pad + lh * (int)i, sc,
		         H->col[i], H->line[i]);
}

/* Manda el HUD entero al PC en UN solo datagrama, con saltos de linea
 * dentro. El servidor lo desglosa al escribirlo en el fichero. Una
 * llamada en vez de diecisiete: menos trafico y sin riesgo de que las
 * lineas de una misma captura lleguen entremezcladas. */
static void hud_report(const hudLines *H)
{
	char buf[1600];
	size_t n = 0;
	u32 i;

	for (i = 0; i < H->n; i++) {
		int r = snprintf(buf + n, sizeof(buf) - n, "%s%s",
		                 i ? "\n" : "", H->line[i]);
		if (r < 0 || (size_t)r >= sizeof(buf) - n) break;
		n += (size_t)r;
	}

	if (n) linkLog("%s", buf);
}

/* --------------------------------------------------------------------- */

static int clampi(int v, int lo, int hi)
{
	if (v < lo) return lo;
	if (v > hi) return hi;
	return v;
}

int main(int argc, const char *argv[])
{
	u64 sec = 0, nsec = 0, t0_sec = 0, t0_nsec = 0;
	u32 frames = 0, tick = 0;
	int profile_shown = 0;
	int avatar_shown = 0, avatar_warned = 0;
	int cat_asked = 0, cat_shown = 0;
	f32 fps = 0.0f, ms = 0.0f;
	int mx, my;
	/* La combinacion de salida la decide Ajustes, y se lee CADA vuelta:
	 * si fuera una constante local, apagarla no haria efecto hasta el
	 * siguiente arranque -- y el usuario la apaga justo cuando le esta
	 * estorbando. */

	linkFrame frame;
	int  live = 0;
	u64  last_frame_us = 0;

	const uiTitle *prev_title = NULL;
	u64 last_log_us = 0;
	u64 opening_since = 0;
	int opening_warned = 0;
	int session_sent = 0;
	s32 spu_ret = 0;
	hudLines hud;
	phaseStats ph;
	u64 tb0;

	e2eStats e2e;
	u64 e2e_sum = 0;
	u32 e2e_n = 0, e2e_acc_min = 0xffffffffu, e2e_acc_max = 0;



	memset(&frame, 0, sizeof(frame));
	memset(&e2e, 0, sizeof(e2e));
	memset(&ph, 0, sizeof(ph));

	tb_hz = sysGetTimebaseFrequency();
	if (tb_hz == 0) tb_hz = 79800000ull;

	printf("%s %s arrancando\n", GR33N_NAME, GR33N_VERSION);

	/* LO PRIMERO DE TODO, como en todos los samples de PSL1GHT que tocan
	 * SPUs. cellVdec lanza hilos de SPU por su cuenta y sin esto no hay
	 * de donde sacarlos. Se guarda el codigo para reportarlo: si sale
	 * distinto de cero, es el primer sospechoso de que vdecOpen no
	 * vuelva. */
	spu_ret = sysSpuInitialize(6, 0);
	printf("[main] sysSpuInitialize(6,0) -> 0x%08x\n", (unsigned)spu_ret);

	/* LA RED VA ANTES QUE EL VIDEO, y no es un capricho de orden.
	 *
	 * Estaba al reves y costo un ciclo entero: abrir GR33N con la consola
	 * puesta en 1080p salia al XMB sin una sola linea de log. Claro que no
	 * habia log - videoInit fallaba y devolvia 1 ANTES de que existiera
	 * el unico instrumento que tenemos para saber por que.
	 *
	 * El comentario de storeInit, veinte lineas mas abajo, ya decia esto
	 * mismo con otras palabras: "detras de linkInit porque storeInit
	 * escribe en el log lo que encuentra, y ese es justo el mensaje que
	 * hace falta cuando esto se tuerce". Y el video, que es lo unico aqui
	 * capaz de colgar la consola, iba delante.
	 *
	 * linkLog encola lo que se diga antes de que aparezca el servidor, asi
	 * que arrancar la red primero no pierde nada: lo suelta todo en cuanto
	 * linkUpdate encuentra al PC. */
	if (linkInit() != 0)
		printf("[main] sin red, seguimos igual\n");

	/* CUANDO SE COMPILO ESTO, LO PRIMERO DE TODO.
	 *
	 * El 2026-09-03 se perdio una ronda entera analizando un log que
	 * contradecia al codigo: el fuente, el .o y el .elf del disco eran
	 * los nuevos, y la consola estaba ejecutando un EBOOT viejo que no
	 * se habia reinstalado. Ni el log ni la version --que solo cambia
	 * cuando alguien la sube a mano-- decian nada.
	 *
	 * Con la fecha de compilacion delante, esa pregunta se contesta en
	 * la primera linea en vez de con arqueologia sobre las cadenas del
	 * binario. Es la version que de verdad manda: GR33N_VERSION dice lo
	 * que alguien escribio, esto dice lo que se ejecuta.
	 *
	 * CON UNA TRAMPA QUE CONVIENE SABER: __DATE__ se congela cuando se
	 * compila ESTE fichero. Si se toca solo webrtc.c y se hace un make
	 * incremental, main.o no se recompila y la fecha se queda vieja. Con
	 * "make clean && make pkg" --que es como se construye aqui-- no pasa,
	 * pero un sello que puede mentir hay que saber cuando miente. */
	linkLog("=== %s %s  compilado " __DATE__ " " __TIME__ " ===",
	        GR33N_NAME, GR33N_VERSION);
	printf("[main] compilado " __DATE__ " " __TIME__ "\n");

	if (inputInit() != 0)
		printf("[main] sin mando, seguimos igual\n");

	if (videoInit() != 0) {
		printf("[main] videoInit fallo, abortando\n");
		linkLog("!! videoInit fallo: GR33N se sale al XMB");

		/* Y ANTES DE IRSE, dar tiempo a que el log llegue.
		 *
		 * Sin esto, todo lo que ha dicho video.c se queda en la cola de
		 * linkLog y muere con el proceso. Tres segundos dan de sobra para
		 * que el descubrimiento encuentre al PC y suelte la cola: es lo
		 * unico que separa "se sale al XMB y no se sabe por que" de
		 * tener el motivo escrito. */
		{
			int i;
			for (i = 0; i < 150; i++) {
				linkUpdate();
				usleep(20000);
			}
		}

		linkShutdown();
		return 1;
	}

	/* EL ALMACEN VA AQUI, ANTES QUE authInit.
	 *
	 * Estaba veinte lineas mas abajo y costo una sesion entera entenderlo:
	 * authInit pregunta si hay un token guardado para arrancar la cadena
	 * del perfil, y si el almacen todavia no sabe donde escribe contesta
	 * que no. No falla, no avisa: contesta que no. Resultado, la barra
	 * superior se quedaba en "Cargando perfil..." para siempre porque
	 * nadie habia pedido nunca el perfil.
	 *
	 * Detras de linkInit porque storeInit escribe en el log lo que
	 * encuentra, y ese es justo el mensaje que hace falta cuando esto se
	 * tuerce. */
	storeInit(argc > 0 && argv ? argv[0] : NULL);

	if (decInit() != 0)
		printf("[main] sin decodificador, seguimos igual\n");
	if (tlsInit() != 0)
		printf("[main] sin TLS, seguimos igual\n");
	if (imgInit() != 0)
		printf("[main] sin decodificador de PNG, seguimos igual\n");
	if (authInit() != 0)
		printf("[main] sin inicio de sesion, seguimos igual\n");
	if (catInit() != 0)
		printf("[main] sin catalogo, seguimos igual\n");
	if (sesInit() != 0)
		printf("[main] sin sesiones de juego, seguimos igual\n");
	if (wrtcInit() != 0)
		printf("[main] sin WebRTC, seguimos igual\n");
	if (pingInit() != 0)
		printf("[main] sin medidor de latencia, seguimos igual\n");
	if (audInit() != 0)
		printf("[main] sin audio, seguimos igual\n");

	/* dcbz solo es valido sobre memoria cacheada. Si la superficie acabo
	 * en memoria local del RSX, se queda desactivado y las rutinas siguen
	 * siendo correctas, solo que sin ese ahorro. Esto va ANTES del
	 * benchmark para que el benchmark mida la configuracion real. */
	copyInit(videoSurfaceInMain());

	/* Una vez, al arrancar. Cuesta unos milisegundos y responde a la
	 * pregunta que lleva dos hipotesis fallidas sin contestar. */
	bench_memory(videoSurface(), &bench1);

	uiInit();
	uiLoadSettings();   /* storeInit ya se hizo arriba, antes de authInit */

	/* Primer arranque sin sesion: se abre la cuenta directamente. No se
	 * empieza el flujo solo — se enseña la pantalla con X para seguir y
	 * Atras para saltarla. Arrancar peticiones a Microsoft sin que nadie
	 * las haya pedido seria pasarse. */
	if (uiFirstRun() && !authHaveToken()) {
		uiOpenAuth();
		linkLog("[ui] primer arranque: abriendo la pantalla de cuenta");
	}

	atexit(exit_callback);
	sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, sysutil_callback, NULL);

	mx = GR33N_SURFACE_W / 2;
	my = GR33N_SURFACE_H / 2;

	sysGetCurrentTime(&t0_sec, &t0_nsec);

	while (running) {
		gr33nSurface *s;
		const gr33nPad *pad;
		const uiTitle *title;
		u64 t;
		int fresh = 0, remote = 0, h264 = 0, tls = 0, auth = 0;

		sysUtilCheckCallback();

		inputPoll();
		pad = inputPad();

		linkUpdate();

		{
			u32 quit_mask = uiQuitMask();

			if (quit_mask && (pad->held & quit_mask) == quit_mask)
				running = 0;
		}

		/* MEDIR LAS REGIONES EN CUANTO SE SEPA CUALES SON.
		 *
		 * Va aqui y no dentro de auth.c porque el login no tiene por que
		 * saber que existe un medidor de latencia; lo unico que hace
		 * falta es ver el flanco de XC_OK. Se rearma si la sesion se
		 * cae, para que volver a entrar vuelva a medir: la red pudo
		 * cambiar mientras tanto. */
		{
			static int medido = 0;
			const xcloudInfo *xcp = authXCloud();

			if (xcp && xcp->state == XC_OK && xcp->regions_n > 0) {
				if (!medido) { medido = 1; pingMedir(); }
			} else {
				medido = 0;
			}
		}

		/* Que la interfaz sepa que hay juego ANTES de darle el mando.
		 *
		 * Si se pusiera despues de uiUpdate, esta vuelta todavia
		 * trataria el juego como menu -- y una sola vuelta basta para
		 * que Atras cierre la ficha y suelte la sesion. */
		{
			int hay_juego = (sesStatus()->state == SES_READY &&
			                 decStatus()->frames_decoded > 0);

			uiSetLive(hay_juego);

			/* Y el mando al juego. Va aqui, con el pad recien leido y
			 * ANTES de uiUpdate, para no meterle un fotograma de
			 * retraso por gusto: en un stream la latencia del mando es
			 * lo primero que se nota.
			 *
			 * Solo copia; el envio de verdad lo hace el hilo del
			 * bombeo, que es el unico que puede hablar con libpeer. */
			if (hay_juego) wrtcSesionPad(pad);
		}

		uiUpdate(pad);

		/* Y si ha hecho la combinacion, se suelta la maquina. La sesion
		 * se para AQUI y no dentro de la interfaz: ui.c no sabe de
		 * sesiones y no tiene por que. */
		if (uiLiveWantsExit()) {
			linkLog(">>> salir del juego");
			sesStop();
		}

		/* El perfil llega por su cuenta desde el hilo de sesion. Aqui
		 * solo se mira si ha cambiado y se le pasa a la interfaz. */
		{
			const xblProfile *pr = authProfile();

			if (pr->state == PROF_OK && !profile_shown) {
				profile_shown = 1;
				uiSetProfile(pr->gamertag, pr->gamerscore);
				linkLog("[perfil] %s  %u G  (cadena en %u ms)",
				        pr->gamertag, (unsigned)pr->gamerscore,
				        (unsigned)pr->ms_total);
			} else if (pr->state == PROF_FAILED && !profile_shown) {
				profile_shown = 1;
				linkLog("!! [perfil] %s", pr->err);
			}

			/* En cuanto xCloud da el token, se pide el catalogo. Un
			 * pestillo mas: cada cosa llega cuando llega. */
			{
				const xcloudInfo *xcp = authXCloud();

				if (xcp->state == XC_OK && !cat_asked) {
					cat_asked = 1;
					catFetch();
				}
			}

			if (!cat_shown) {
				const catInfo *ci = catStatus();

				if (ci->state == CAT_OK) {
					cat_shown = 1;
					linkLog("[catalogo] %u titulos, %u jugables",
					        (unsigned)ci->n, (unsigned)ci->entitled_n);
				} else if (ci->state == CAT_FAILED) {
					cat_shown = 1;
					linkLog("!! [catalogo] %s", ci->err);
				}
			}

			/* La foto llega despues que el resto del perfil, asi que
			 * lleva su propio pestillo. */
			if (pr->pic_ready && !avatar_shown) {
				avatar_shown = 1;
				uiSetProfilePic(authAvatar(), AVATAR_PX, AVATAR_PX);
				linkLog("[perfil] avatar listo");
			} else if (!avatar_shown && pr->pic_err[0] && !avatar_warned) {
				avatar_warned = 1;
				linkLog("!! [perfil] avatar: %s", pr->pic_err);
			}
		}

		title  = uiActiveTitle();
		remote = (uiGetMode() == UI_STREAM && title &&
		          title->kind == TITLE_REMOTE);
		h264   = (uiGetMode() == UI_STREAM && title &&
		          title->kind == TITLE_H264);
		tls    = (uiGetMode() == UI_STREAM && title &&
		          title->kind == TITLE_TLS);
		auth   = (uiGetMode() == UI_STREAM && title &&
		          title->kind == TITLE_AUTH);

		/* Entrar y salir de un titulo: el decodificador se arranca y se
		 * para aqui, no dentro de la UI. */
		if (title != prev_title) {
			if (prev_title && prev_title->kind == TITLE_H264)
				decStop();
			if (prev_title && prev_title->kind == TITLE_AUTH)
				authCancel();
			if (title && title->kind == TITLE_TLS) {
				/* El documento de descubrimiento de OpenID. No es una
				 * URL de relleno: dentro viene la direccion del endpoint
				 * de codigo de dispositivo, que es el siguiente paso. */
				tlsStartGet("login.microsoftonline.com",
				            "/consumers/v2.0/.well-known/openid-configuration");
			}
			if (title && title->kind == TITLE_WEBRTC) {
				/* No bloquea: se lo lleva su propio hilo. Y hace falta
				 * que sea asi, porque ahi dentro puede haber una
				 * generacion de clave RSA de varios segundos y el hilo
				 * de dibujo no se puede parar a esperarla. Lo que se ve
				 * en pantalla mientras tanto sale de wrtcStatus(). */
				wrtcProbe();
			}
			if (title && title->kind == TITLE_H264) {
				decStart(clip_h264, (u32)(clip_h264_end - clip_h264));
				opening_since = now_us();
				opening_warned = 0;
			}
			prev_title = title;
			linkLog(">>> %s", title ? title->id : "menu");
		}

		/* Vigilante: si cellVdec lleva demasiado sin volver, decirlo. Un
		 * "abriendo..." eterno no es un diagnostico. */
		if (!opening_warned && decStatus()->state == DEC_OPENING &&
		    opening_since && now_us() - opening_since > 8000000ull) {
			opening_warned = 1;
			linkLog("!! cellVdec lleva 8 s sin abrir. El ultimo [dec] de "
			        "arriba dice en que paso se quedo.");
		}

		if (remote) {
			/* El input sale LO ANTES POSIBLE en el frame, para darle a la
			 * respuesta el maximo de tiempo para volver antes de dibujar. */
			linkSendInput(pad->held, pad->lx, pad->ly, pad->rx, pad->ry);

			/* Mira local: misma integracion que hace el servidor, para
			 * poder comparar. Zona muerta generosa, que los nubs de un
			 * DS3 con anos encima no vuelven a 128 ni de broma. */
			{
				int dz = uiDeadzone();
				if (pad->lx > dz || pad->lx < -dz) mx += pad->lx / 10;
				if (pad->ly > dz || pad->ly < -dz) my += pad->ly / 10;
			}
			mx = clampi(mx, 40, GR33N_SURFACE_W - 40);
			my = clampi(my, 40, GR33N_SURFACE_H - 40);
		} else {
			live = 0;
		}

		tb0 = __gettime();
		s = videoSurface();          /* espera a que el RSX suelte la textura */
		ph.surf += __gettime() - tb0;

		/* La segunda toma del banco de pruebas, esta vez con la consola en
		 * marcha: mismo codigo, mismo tamano, mismo destino, pero en medio
		 * del bucle de dibujo en vez de en un arranque tranquilo.
		 *
		 * Cuesta un frame con la pantalla hecha un cuadro. Es un precio
		 * ridiculo por dejar de discutir a base de hipotesis. */
		if (!bench2_done && ++bench_frames >= 300) {
			bench2_done = 1;
			/* Anunciarse ANTES de medir. La toma anterior no aparecio en el
			 * log y no habia forma de saber si es que no se ejecuto o es que
			 * fallo por dentro. Una prueba que no dice "he empezado" solo
			 * puede fallar de una forma: en silencio. */
			linkLog("=== segunda toma del banco de pruebas, frame %u ===",
			        (unsigned)bench_frames);
			bench_memory(s, &bench2);
			bench_report("en marcha", &bench2);
		}

		if (remote) {
			/* Y el frame se recoge LO MAS TARDE POSIBLE, ya con la
			 * superficie en la mano y justo antes de pintar. Con espera
			 * acotada: gastar medio milisegundo aqui sale mucho mas
			 * barato que pintar un frame de 16,6 ms de antiguedad, y el
			 * tiempo no se pierde porque ibamos a bloquearnos en el vsync
			 * de todas formas.
			 *
			 * Sin enlace vivo no se espera: un servidor muerto no puede
			 * costar 4 ms por frame. */
			t = now_us();
			fresh = live ? linkWaitFrame(&frame, FRAME_WAIT_US)
			             : linkTakeFrame(&frame);
			if (fresh) {
				live = 1;
				last_frame_us = t;
			} else if (live && t - last_frame_us > FRAME_STALE_US) {
				live = 0;
			}
		}

		/* Se construye SIEMPRE, aunque Debug este apagado: el log del PC
		 * no deberia depender de un ajuste de la interfaz. */
		tb0 = __gettime();
		hud_build(&hud, s, pad, fps, ms, &e2e, live, &ph);
		ph.hud += __gettime() - tb0;

		tb0 = __gettime();

		/* JUEGO EN VIVO: lo primero de todo, y fuera del reparto por
		 * titulo.
		 *
		 * Una sesion de xCloud no se abre desde la biblioteca como los
		 * titulos de prueba: se pide desde la ficha del juego y la
		 * interfaz sigue en el menu enseñando el estado. Cuando empieza
		 * a haber imagen decodificada, esa imagen manda sobre todo lo
		 * demas.
		 *
		 * La condicion es "hay sesion Y hay imagen", no una de las dos:
		 * SES_READY llega antes de que exista video, y el decodificador
		 * tambien lo usa el clip empotrado. Las dos juntas solo se dan
		 * aqui.
		 *
		 * Y la segunda mitad es frames_DECODED, no frames_SHOWN. La
		 * primera version ponia shown, y eso no puede cumplirse nunca:
		 * shown lo incrementa decTakePicture() al pintar, o sea que la
		 * condicion pedia haber pintado ya para dejar pintar. Una guarda
		 * que depende de lo que guarda no es una guarda: es un candado
		 * con la llave dentro.
		 *
		 * decoded lo pone el hilo del decodificador cuando cellVdec
		 * suelta una imagen, sin depender de que nadie la mire, que es
		 * justo la senal que hacia falta. */
		if (sesStatus()->state == SES_READY &&
		    decStatus()->frames_decoded > 0) {
			decTakePicture(s);
			uiDrawStreamHint(s);
		} else if (uiGetMode() == UI_STREAM) {
			if (remote) {
				if (live) draw_frame(s, &frame);
				else      draw_pattern(s, tick);
				draw_marker(s, mx, my, pad);
			} else if (auth) {
				const authInfo *a = authStatus();

				/* X arranca o reintenta; O cancela mientras espera. La
				 * UI no toca el flujo por dentro: solo pide y cancela. */
				if ((pad->pressed & uiBtnOk()) &&
				    (a->state == AUTH_IDLE || a->state == AUTH_FAILED))
					authStart();
				if ((pad->pressed & uiBtnBack()) && a->state == AUTH_WAITING)
					authCancel();

				draw_auth_screen(s);
			} else if (tls) {
				draw_tls_screen(s);
			} else if (h264) {
				const decInfo *d = decStatus();

				if (d->state == DEC_FAILED) {
					draw_dec_message(s, GR33N_RGB(255, 80, 80),
					                 "cellVdec fallo", d->err);
				} else if (!decTakePicture(s) && d->frames_shown == 0) {
					/* Sin imagen todavia. Si ya hubo alguna, se deja la
					 * anterior en pantalla y punto. */
					draw_dec_message(s, GR33N_RGB(255, 140, 0),
					                 d->state == DEC_OPENING
					                     ? "Abriendo cellVdec..."
					                     : "Decodificando...",
					                 "Inicializando SPUs, troceando el "
					                 "stream y arrancando la secuencia. "
					                 "Todo esto va en su propio hilo: si "
					                 "se atasca, la interfaz sigue viva.");
				}
			} else {
				draw_pattern(s, tick);
			}
			uiDrawStreamHint(s);
			ph.cont += __gettime() - tb0;

			tb0 = __gettime();
			if (uiDebug()) hud_draw(s, &hud);
			ph.hud += __gettime() - tb0;
		} else {
			uiDraw(s);
			ph.cont += __gettime() - tb0;

			/* En el menu el panel se va abajo a la derecha, que arriba a
			 * la izquierda tapa las pestanas y la lista. */
			tb0 = __gettime();
			if (uiDebug()) hud_draw(s, &hud);
			ph.hud += __gettime() - tb0;
		}

		tb0 = __gettime();
		videoPresent();
		ph.flip += __gettime() - tb0;
		phase_tick(&ph);

		/* Latencia extremo a extremo: del instante en que mandamos el
		 * input al instante en que el frame que lo refleja queda
		 * entregado al RSX. NO incluye el frame de vsync que falta para
		 * que salga por HDMI ni el retardo del panel: eso es un frame mas
		 * (16,6 ms) y lo que anada la tele. */
		if (fresh && frame.t_input_us) {
			u64 nowv = now_us();
			if (nowv > frame.t_input_us) {
				u64 d = nowv - frame.t_input_us;
				if (d < 1000000ull) {
					e2e.last = (u32)d;
					e2e.valid = 1;
					e2e_sum += d;
					e2e_n++;
					if ((u32)d < e2e_acc_min) e2e_acc_min = (u32)d;
					if ((u32)d > e2e_acc_max) e2e_acc_max = (u32)d;

					if (e2e_n >= E2E_WINDOW) {
						e2e.avg = (u32)(e2e_sum / e2e_n);
						e2e.min = e2e_acc_min;
						e2e.max = e2e_acc_max;
						e2e_sum = 0;
						e2e_n = 0;
						e2e_acc_min = 0xffffffffu;
						e2e_acc_max = 0;
					}
				}
			}
		}

		tick++;
		frames++;

		/* Latido al log remoto cada 2 s. Por TIEMPO, no por frames: con
		 * el fps variable, contar frames da un intervalo que cambia solo y
		 * las medidas dejan de ser comparables entre sesiones.
		 *
		 * Se manda el HUD ENTERO, tal cual. Sesion de hardware completa
		 * legible desde el PC sin fotografiar un televisor. */
		{
			u64 nowv = now_us();
			if (nowv - last_log_us > 2000000ull) {
				const decInfo *d = decStatus();

				last_log_us = nowv;

				/* Cabecera de sesion, una vez, cuando hay enlace. Deja
				 * el entorno escrito al principio del fichero de log. */
				if (!session_sent && linkStatus()->state == LINK_UP) {
					session_sent = 1;
					linkLog("=== %s %s | display %ux%u | superficie %s | "
					        "copia %s | almacen %s | "
					        "sysSpuInitialize 0x%08x ===",
					        GR33N_NAME, GR33N_VERSION,
					        (unsigned)videoDisplayWidth(),
					        (unsigned)videoDisplayHeight(),
					        videoSurfaceInMain() ? "MEMORIA PRINCIPAL"
					                             : "memoria local RSX",
					        copyMode(),
					        storePath()[0] ? storePath() : "NINGUNO",
					        (unsigned)spu_ret);

					bench_report("arranque", &bench1);
				}

				linkLog("--- %s ---", title ? title->id : "menu");
				hud_report(&hud);

				if (d->state == DEC_FAILED)
					linkLog("!! vdec ERROR: %s", d->err);
			}
		}

		sysGetCurrentTime(&sec, &nsec);
		{
			f64 dt = (f64)(sec - t0_sec) +
			         ((f64)nsec - (f64)t0_nsec) / 1000000000.0;
			if (dt >= 0.5) {
				fps = (f32)((f64)frames / dt);
				ms  = (fps > 0.0f) ? (1000.0f / fps) : 0.0f;
				frames = 0;
				t0_sec = sec;
				t0_nsec = nsec;
			}
		}
	}

	printf("[main] saliendo\n");
	uiSaveSettings();
	/* authShutdown primero: el hilo de sesion puede estar decodificando un
	 * avatar justo ahora. Cerrar el decodificador debajo de el seria la
	 * misma carrera que ya nos costo un 0x80610102 con cellVdec. */
	/* sesShutdown LO PRIMERO de los tres, y no por orden alfabetico: si
	 * hay una sesion abierta, hay una maquina reservada en un centro de
	 * datos de Microsoft contra la cuota de esta cuenta. Soltarla es lo
	 * unico de todo el apagado que le importa a alguien de fuera. */
	/* wrtcShutdown antes que sesShutdown: el hilo de WebRTC no toca la
	 * sesion, pero puede estar dentro de una generacion de clave RSA de
	 * varios segundos, y es mejor esperarlo mientras todo lo demas sigue
	 * en pie que dejarlo corriendo debajo de un apagado a medias. */
	audShutdown();
	pingShutdown();
	wrtcShutdown();
	sesShutdown();
	catShutdown();
	authShutdown();
	imgShutdown();
	tlsShutdown();
	linkLog("GR33N saliendo");
	decShutdown();
	linkShutdown();
	inputShutdown();

	return 0;
}
