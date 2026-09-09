/* GR33N - de paquetes RTP sueltos a unidades de acceso H.264.
 *
 * libpeer entrega el paquete RTP ENTERO, cabecera incluida
 * (rtp.c:rtp_decode_h264). No despaqueta nada, y no es un descuido:
 * green-nx borro su despaquetizador porque reensamblaba en orden de
 * llegada y ante cualquier perdida o reordenacion sacaba fotogramas
 * corruptos -- la basura verde y rosa de siempre.
 *
 * Asi que el trabajo es este fichero. Y no es solo "juntar trozos".
 *
 * POR QUE HACE FALTA RETRASO, que es lo que no se ve venir.
 *
 * Un buffer ingenuo --montar el fotograma actual y soltarlo en cuanto
 * llega una marca de tiempo nueva-- tira cualquier fotograma al que le
 * falte un paquete ANTES de que pueda llegar la retransmision, que tarda
 * un viaje de ida y vuelta. Y luego sigue dandole al decodificador
 * fotogramas P que se refieren al que acaba de tirar. El decodificador no
 * falla: rellena con lo que tenga, y eso se sostiene solo -- verde,
 * rosa, bloques-- hasta el siguiente fotograma clave.
 *
 * Este buffer hace lo contrario:
 *
 *   - guarda los fotogramas un rato (VJ_ESPERA_MS) para que lleguen las
 *     retransmisiones y los reordenados,
 *   - los suelta en orden ESTRICTO de marca de tiempo,
 *   - y ante una perdida irrecuperable tira, y no vuelve a soltar nada
 *     hasta que llega un IDR nuevo. Un fotograma P roto no llega jamas
 *     al decodificador.
 *
 * El algoritmo es el de green-nx (src/switch/stream/video_jitter.cpp),
 * portado a C con memoria fija: nada de reservar en un camino que se
 * recorre trescientas veces por segundo.
 *
 * Todo lo de aqui es aritmetica sobre bytes, asi que se prueba en el PC
 * (t/vjitter.c), como teredo.c y xcmsg.c.
 */

#ifndef GR33N_VJITTER_H
#define GR33N_VJITTER_H

#include <stddef.h>
#include <ppu-types.h>

/* Cuanto se espera a que se complete un fotograma al que le falta algo.
 *
 * green-nx lo subio de 90 a 200 ms y dejo escrito por que: con 90, un
 * solo paquete que llegara justo despues del plazo se cargaba la cadena
 * predictiva entera y congelaba la imagen hasta el siguiente IDR. El
 * retraso solo se paga cuando el fotograma YA esta incompleto; los sanos
 * salen en el acto. */
#define VJ_ESPERA_MS   200

#define VJ_MAX_FRAMES   16      /* fotogramas a la vez */
#define VJ_MAX_PKTS    512      /* deposito comun de paquetes */
#define VJ_PKT_MAX    1500      /* lo mas grande que cabe en un datagrama */
#define VJ_AU_MAX  (512 * 1024) /* una unidad de acceso: un IDR de 720p */

typedef struct {
	u16 seq;
	u8  marca;          /* el bit M: ultimo paquete del fotograma */
	u16 len;
	s16 sig;            /* siguiente del mismo fotograma, o -1 */
	u8  datos[VJ_PKT_MAX];
} vjPkt;

typedef struct {
	u32 ts;
	u64 visto_ms;
	s16 cabeza;         /* lista de paquetes, o -1 */
	int usado;
} vjFrame;

typedef struct {
	u32 paquetes;       /* RTP recibidos */
	u32 fotogramas;     /* unidades de acceso soltadas */
	u32 tirados;        /* fotogramas perdidos por plazo vencido */
	u32 nacks;
	u32 resyncs;        /* veces que se ha vuelto a esperar un IDR */
	u32 sin_sitio;      /* veces que el deposito se quedo sin paquetes */
	u32 ultima_au;      /* bytes de la ultima unidad de acceso */
} vjStats;

typedef struct {
	vjPkt   pool[VJ_MAX_PKTS];
	s16     libre;                  /* cabeza de la lista de libres */
	vjFrame frames[VJ_MAX_FRAMES];  /* ordenados por ts ascendente */
	int     n_frames;

	u32     ultimo_ts;
	int     hay_ultimo;
	int     esperando_clave;

	/* Seguimiento global de secuencia: NACK y estadisticas. */
	int     hay_seq;
	u16     max_seq;
	u32     ciclos, base_seq, recibidos;
	u32     recibidos_antes, esperados_antes;

	u8      au[VJ_AU_MAX];          /* donde se monta la que sale */
	vjStats st;
} vjBuf;

/* Deja el buffer como recien nacido: sin fotogramas y esperando un IDR. */
void vjReset(vjBuf *v);

/* Un paquete RTP ya descifrado.
 *
 * `emitir` se llama, quizas varias veces, con cada unidad de acceso
 * completa en orden de decodificacion. El buffer que recibe es interno y
 * VALE SOLO DURANTE LA LLAMADA: quien lo quiera guardar, que lo copie.
 *
 * `nack` se llama por cada hueco de secuencia, con el formato de RTCP
 * (un identificador y una mascara de los 16 siguientes).
 *
 * `pedir_clave` se pone a 1 si hace falta pedirle al servidor un
 * fotograma clave, que es lo unico que saca de un corte. */
void vjRecibe(vjBuf *v, const u8 *rtp, size_t len, u64 ahora_ms,
              void (*emitir)(const u8 *au, size_t n, void *ud),
              void (*nack)(u16 pid, u16 blp, void *ud),
              void *ud, int *pedir_clave);

/* Para el informe de recepcion de RTCP. Devuelve 0 si todavia no hay
 * nada que contar.
 *
 * OJO: A DIA DE HOY NADIE LA LLAMA.
 *
 * Se escribio en agosto para mandar informes de recepcion, y se quedo sin
 * conectar. Eso significa que GR33N calcula cuantos paquetes pierde y se
 * lo guarda para si: xCloud no se entera nunca de como va la cosa por
 * aqui, y por tanto no tiene con que decidir bajar la calidad.
 *
 * Se deja porque el codigo es correcto y hara falta, pero se deja DICHO,
 * que es lo que faltaba: una funcion muda con pinta de estar en uso es
 * peor que no tenerla. Costo un mes darse cuenta.
 *
 * Le falta el jitter de llegada (RFC 3550 6.4.1) para poder montar un RR
 * completo. Informar jitter cero seria decirle al emisor que el enlace va
 * fino justo cuando no lo va. */
int vjStatsRtcp(vjBuf *v, u8 *fraccion_perdida, u32 *perdidos,
                u32 *mayor_seq);

const vjStats *vjEstado(const vjBuf *v);

/* 1 mientras esta tirando todo a la espera de un IDR. */
int vjEsperandoClave(const vjBuf *v);

#endif /* GR33N_VJITTER_H */
