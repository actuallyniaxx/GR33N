/* GR33N - enlace UDP con el servidor
 *
 * Descubrimiento, ping/RTT, envio de input, recepcion de frames y log
 * remoto. Es el transporte completo: lo unico que le falta al streaming
 * de verdad es que el payload de FRAME sea H.264 en vez de una rejilla
 * de colores.
 *
 * HILOS. La recepcion vive en un hilo PPU propio, no en el bucle de
 * dibujo. No es por elegancia: si drenas el socket una vez por frame, el
 * paquete que llega justo despues del drenaje espera hasta el frame
 * siguiente y cualquier medida de latencia sale contaminada con el
 * periodo de frame entero. Se midio: RTT "de red" de 16,65 ms en
 * localhost, que es exactamente 1/60 s. Con el hilo, 0,48 ms. El hilo
 * marca la hora de llegada en el instante en que netRecvFrom devuelve.
 *
 * Los envios (hello, ping, input, log) salen del hilo que llama. El
 * estado compartido va bajo mutex.
 *
 * ENDIANNESS: la PS3 es big-endian, o sea que el orden de red y el del
 * host coinciden y htonl/htons son no-ops. Aun asi TODO el formato de
 * cable se arma byte a byte con put_u32/get_u32: asi el servidor en
 * Python (x86) y cualquier par futuro se entienden sin sorpresas, y el
 * padding de structs no puede morder.
 */

#ifndef GR33N_LINK_H
#define GR33N_LINK_H

#include <ppu-types.h>

#define LINK_SERVER_PORT   9330
#define LINK_CLIENT_PORT   9331

#define LINK_MAGIC         0x47523333u   /* "GR33" */
#define LINK_VERSION       1

#define LINK_MSG_HELLO     1   /* cliente -> broadcast : estoy aqui, quien sirve? */
#define LINK_MSG_HERE      2   /* servidor -> cliente  : yo sirvo */
#define LINK_MSG_PING      3   /* cliente -> servidor  : seq + t_cliente */
#define LINK_MSG_PONG      4   /* servidor -> cliente  : eco + t_servidor */
#define LINK_MSG_LOG       5   /* cliente -> servidor  : texto */
#define LINK_MSG_INPUT     6   /* cliente -> servidor  : estado del mando */
#define LINK_MSG_FRAME     7   /* servidor -> cliente  : rejilla RGB */

#define LINK_NAME_LEN      32

/* Rejilla del "video" de mentira. 24x14 celdas en RGB son 1008 bytes de
 * payload: cabe en un datagrama sin fragmentar IP, que es lo que importa.
 * La fragmentacion multiplica la perdida en redes reales. */
#define LINK_FRAME_COLS    24
#define LINK_FRAME_ROWS    14
#define LINK_FRAME_BYTES   (LINK_FRAME_COLS * LINK_FRAME_ROWS * 3)

typedef enum {
	LINK_DOWN = 0,     /* sin pila de red, o netInitialize fallo */
	LINK_SEARCHING,    /* buscando servidor */
	LINK_UP            /* servidor encontrado y respondiendo */
} linkState;

typedef struct {
	u32 seq;
	u32 input_seq;   /* input que el servidor tenia al generarlo */
	u32 cols;
	u32 rows;
	u8  rgb[LINK_FRAME_BYTES];

	/* Marca de tiempo (reloj del CLIENTE) del input que el servidor tenia
	 * en la mano cuando genero este frame. El servidor solo la copia, no
	 * la interpreta: por eso no hace falta sincronizar relojes para medir
	 * la latencia extremo a extremo. 0 = sin input asociado. */
	u64 t_input_us;
} linkFrame;

typedef struct {
	linkState state;

	char local_ip[16];
	char server_ip[16];
	char server_name[LINK_NAME_LEN];

	/* RTT en microsegundos, ventana movil de 128 muestras */
	u32 rtt_last;
	u32 rtt_min;
	u32 rtt_max;
	u32 rtt_avg;
	u32 jitter;

	u32 loss_pct;      /* perdida de pings en la ultima ventana, 0..100 */
	u32 pings_sent;
	u32 pongs_recv;
	u32 pps;           /* pongs por segundo */

	u32 frames_recv;   /* frames totales recibidos */
	u32 frame_gaps;    /* huecos en la secuencia de frames */
	u32 fps_rx;        /* frames por segundo recibidos */

	/* Cuantos inputs de antiguedad trae el frame que se acaba de pintar.
	 * 0 = responde al input de este mismo frame. 1 = un frame tarde. Es
	 * el diagnostico directo de si la tuberia esta comiendo latencia. */
	u32 frame_lag;

	/* Microsegundos que se espero al frame antes de dibujar. */
	u32 wait_us;
} linkInfo;

int  linkInit(void);
void linkShutdown(void);

/* Llamar una vez por frame desde el bucle principal. Manda hello/ping y
 * mantiene las ventanas de estadisticas. No bloquea nunca. */
void linkUpdate(void);

/* Manda el estado del mando. El servidor responde con un FRAME, asi que
 * esto es lo que marca el ritmo del "video". */
void linkSendInput(u32 buttons, s32 lx, s32 ly, s32 rx, s32 ry);

/* Copia el ultimo frame recibido. Devuelve 1 si habia uno nuevo desde la
 * llamada anterior, 0 si no. */
int  linkTakeFrame(linkFrame *out);

/* Igual, pero espera hasta timeout_us a que llegue uno.
 *
 * Esperar suena a herejia en un bucle de dibujo, pero la alternativa es
 * peor: si pides el frame y lo recoges 50 us despues, cuando la respuesta
 * tarda 400, pierdes la carrera SIEMPRE y pintas el frame anterior. Eso
 * son 16,6 ms de latencia regalados. Y el tiempo que gastas esperando no
 * se pierde: ibas a bloquearte en el vsync de todas formas. */
int  linkWaitFrame(linkFrame *out, u32 timeout_us);

/* Devuelve una copia coherente del estado, tomada bajo el mutex.
 * El puntero es valido hasta la siguiente llamada desde el mismo hilo. */
const linkInfo *linkStatus(void);

/* Log por UDP al servidor. Si no hay enlace, se descarta en silencio.
 * En PS3 printf solo se ve por el TTY de ps3load; esto funciona siempre. */
void linkLog(const char *fmt, ...);

/* Copia `src` a `out` sustituyendo el valor de las claves JSON indicadas
 * por su longitud: {"userToken":"M.C517..."} -> {"userToken":"<564 bytes>"}
 *
 * Las claves se pasan CON la comilla de delante ("\"userToken") para que no
 * casen dentro de otra palabra. Es para no meter credenciales en un fichero
 * que luego se manda por ahi; ver el comentario largo en link.c.
 *
 * Devuelve `out`, siempre terminado en nulo. */
const char *linkRedact(char *out, u32 max, const char *src,
                       const char *const *claves, u32 n_claves);

#endif /* GR33N_LINK_H */
