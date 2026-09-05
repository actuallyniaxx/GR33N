/* GR33N - decodificador H.264 por hardware (cellVdec)
 *
 * Envuelve cellVdec en la misma forma que el resto: un hilo hace el
 * trabajo, el bucle de dibujo pide la imagen cuando la necesita.
 *
 *     decStart(stream, size);
 *     ... por frame ...
 *     decTakePicture(surface);
 *
 * AVISOS QUE CUESTAN UNA CONSOLA:
 *
 * 1. NADIE en PSL1GHT usa este binding. Ni un sample, ni un wrapper, ni
 *    un test. La cabecera y los exports estan, sin ejercitar. Somos
 *    probablemente los primeros.
 *
 * 2. libvdec no tiene wrapper, a diferencia de libsysutil o libgcm_sys.
 *    En PPC64 un puntero a funcion es un DESCRIPTOR, no una direccion.
 *    Las demas librerias lo convierten con __get_opd32() por dentro;
 *    esta no. Hay que construir vdecClosure.fn a mano.
 *
 * 3. El tamano de la imagen se lee del SPS del bitstream, nunca del
 *    declarado. Pasarse cuelga la consola en duro; quedarse corto da
 *    pantalla negra en silencio. Por eso decoder.c COMPRUEBA el tamano
 *    contra el buffer antes de llamar a vdecGetPicture y se niega a
 *    seguir si no cabe: un mensaje de error es infinitamente mejor que
 *    un cuelgue.
 */

#ifndef GR33N_DECODER_H
#define GR33N_DECODER_H

#include <ppu-types.h>
#include "video.h"

/* Techo del buffer de imagen. 1088 y no 1080 porque H.264 codifica en
 * macrobloques de 16 y 1080 no es multiplo. */
#define DEC_MAX_W   1920
#define DEC_MAX_H   1088

typedef enum {
	DEC_IDLE = 0,
	DEC_OPENING,   /* abriendo cellVdec — puede tardar, va en su hilo */
	DEC_PLAYING,
	DEC_FAILED
} decState;

typedef struct {
	decState state;

	u32 width;          /* del SPS, no del declarado */
	u32 height;
	u32 au_count;       /* unidades de acceso encontradas en el stream */
	u32 au_index;

	u32 frames_decoded;
	u32 frames_shown;
	u32 errors;
	u32 drops;          /* imagenes pisadas por no haber hueco */

	/* LATENCIA: microsegundos de vdecDecodeAu a imagen lista, para UNA
	 * unidad de acceso concreta. Con la tuberia llena esto SUBE, porque
	 * cada imagen espera detras de las que van delante. Es lo que tarda
	 * en aparecer el primer frame. */
	u32 decode_us_last;
	u32 decode_us_avg;
	u32 decode_us_min;
	u32 decode_us_max;

	/* RITMO: microsegundos entre imagenes CONSECUTIVAS. Este es el numero
	 * que decide si 720p60 es posible, y no tiene nada que ver con el de
	 * arriba. En serie los dos coinciden; encauzando, este baja y aquel
	 * sube. Confundirlos es el error clasico de medir un decodificador. */
	u32 pace_us_last;
	u32 pace_us_avg;
	u32 pace_us_min;

	u32 inflight;       /* unidades de acceso enviadas sin imagen todavia */
	u32 inflight_max;   /* techo, sacado de vdecAttr.cmd_depth */
	u32 spus;           /* SPUs que tiene cellVdec en esta ronda */

	char err[64];       /* motivo si state == DEC_FAILED */
} decInfo;

int  decInit(void);
void decShutdown(void);

/* Trocea el elementary stream en unidades de acceso y arranca. */
int  decStart(const u8 *stream, u32 size);

/* Arranca EN VIVO: sin clip, con las unidades llegando de la red.
 *
 * El modo de siempre da vueltas a un clip empotrado; este espera a que
 * alguien llame a decFeedAu(). Lo usa el camino de xCloud: vjitter.c monta
 * las unidades a partir del RTP y las va empujando aqui. */
int  decStartLive(void);

/* Una unidad de acceso completa, en Annex-B (con sus 00 00 00 01).
 *
 * Devuelve 0 si se ha encolado y -1 si no habia sitio, no cabia, o no
 * estamos en vivo. NUNCA recorta: una unidad a medias tiene forma de
 * unidad y el decodificador la intentaria.
 *
 * Se puede llamar desde otro hilo. */
int  decFeedAu(const u8 *au, u32 n);

/* Unidades tiradas por no haber ranura libre o no caber. Si esto sube, el
 * decodificador no sigue el ritmo de la red. */
u32  decLiveDrops(void);
void decStop(void);

/* 1 UNA SOLA VEZ si el descodificador acaba de reabrirse tras un atasco y
 * necesita un fotograma clave.
 *
 * Lo pregunta el bombeo de WebRTC, que es el unico que puede mandar un
 * PLI. Se pregunta y se limpia: dos lecturas no piden dos IDR.
 *
 * Sin esto, reabrir no sirve de nada: un descodificador recien abierto no
 * puede empezar por la mitad de un GOP, asi que se quedaria tragando
 * unidades sin sacar una sola imagen -- exactamente la misma pantalla
 * congelada, pero con mas trabajo detras. */
int  decQuiereClave(void);

/* Si hay imagen nueva, la escala a la superficie. Devuelve 1 si pinto. */
int  decTakePicture(gr33nSurface *s);

const decInfo *decStatus(void);

#endif /* GR33N_DECODER_H */
