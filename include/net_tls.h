/* GR33N - TLS sobre PSL1GHT
 *
 * La cola entre mbedTLS y la consola. Tres cosas que mbedTLS espera del
 * sistema operativo y que la PS3 da de otra forma:
 *
 *   entropia   sysGetRandomNumber (lv2), no /dev/urandom
 *   relojes    el registro de timebase y sysGetCurrentTime, no POSIX
 *   sockets    netSocket/netSend/netRecv, que se parecen a los BSD pero
 *              no lo son
 *
 * TODO LO QUE BLOQUEA VIVE EN UN HILO. Resolver un nombre, abrir un
 * socket y negociar TLS son tres esperas de cientos de milisegundos cada
 * una; hacerlas desde el bucle de dibujo congela la consola. Ya nos paso
 * con vdecOpen y no se repite: tlsStartGet() deja la peticion y vuelve.
 */

#ifndef GR33N_NET_TLS_H
#define GR33N_NET_TLS_H

#include <ppu-types.h>

#define TLS_BODY_MAX  32768

typedef enum {
	TLS_IDLE = 0,
	TLS_RESOLVING,
	TLS_CONNECTING,
	TLS_HANDSHAKE,
	TLS_REQUEST,
	TLS_DONE,
	TLS_FAILED
} tlsState;

typedef struct {
	tlsState state;

	char host[96];
	char ip[16];
	char version[16];    /* TLSv1.2 / TLSv1.3            */
	char cipher[48];     /* suite negociada              */
	char peer[96];       /* sujeto del certificado       */

	u32  verify_flags;   /* 0 = cadena valida            */
	char verify_txt[96];

	int  http_status;
	u32  body_len;

	/* Cuanto cuesta cada tramo. La negociacion TLS es la parte cara y
	 * conviene saber cuanto, porque se paga en cada arranque. */
	u32  ms_resolve, ms_connect, ms_handshake, ms_request;

	char err[128];
} tlsInfo;

int  tlsInit(void);
void tlsShutdown(void);

/* No bloquea: deja la peticion puesta y el hilo se encarga. */
int  tlsStartGet(const char *host, const char *path);

/* ¿Hay una peticion en marcha? Para no montar los preparativos -el de la
 * tienda reserva dos megabytes- cuando ya se sabe que tlsRequestTo va a
 * devolver -2. Es una foto: entre preguntar y pedir, el otro hilo puede
 * coger el turno igual, asi que hay que seguir tratando el -2. */
int  tlsBusy(void);

/* PRIORIDAD. Mientras alguien la tenga cogida, el trabajo de FONDO tiene
 * que apartarse: consultar tlsHeld() antes de montar una peticion y volver
 * luego.
 *
 * Hace falta porque turnarse no basta cuando uno de los dos vuelve a la
 * cola al instante. El barrido del catalogo encadena lotes de dos segundos
 * y reclama el turno en cuanto suelta el anterior: el hilo de la sesion de
 * juego pedia cada 200 ms y no lo conseguia NUNCA.
 *
 * No afecta a quien pide el turno de verdad: tlsRequestTo sigue igual para
 * todos. Esto es una cortesia que los de fondo aceptan por su cuenta. */
void tlsHold(int on);
int  tlsHeld(void);

/* Corta la peticion en curso. Se nota como mucho 200 ms despues, que es
 * lo que dura una vuelta de espera. */
void tlsAbort(void);

const tlsInfo *tlsStatus(void);

/* SOLO el cuerpo, y SOLO si cayo en el buffer de serie del modulo.
 *
 * Las cabeceras HTTP ya estan quitadas y el troceado de Transfer-Encoding
 * deshecho. Lleva un nulo detras por comodidad, pero si el cuerpo es
 * binario (un PNG) hay que ir por *len, no por strlen.
 *
 * Devuelve NULL cuando la ultima peticion traia su propio buffer: ese
 * cuerpo vive en memoria de otro y puede estar ya liberada. Quien pide con
 * buffer propio recoge el cuerpo del tlsResult, que se copia con el turno
 * cogido. Esto se queda para el panel de pruebas, que lee desde el hilo de
 * dibujo y no puede permitirse un puntero prestado. */
const char *tlsBody(u32 *len);

/* El resultado de UNA peticion, copiado mientras el modulo todavia es
 * tuyo.
 *
 * Existe por el hermano pequeno del fallo del sink. tlsBody() y tlsStatus()
 * leen estado GLOBAL, y el turno se suelta al volver de la peticion: entre
 * ese momento y el instante en que quien llamo mira el cuerpo, el otro hilo
 * puede haber cogido el turno y haber empezado otra peticion encima. El
 * cuerpo que lees ya no es el tuyo.
 *
 * Con esto, lo que importa -codigo, cuerpo, longitud y motivo del fallo- se
 * captura DENTRO del turno y viaja en una estructura tuya.
 *
 * `body` apunta dentro del sink que TU diste, y por eso conviene darlo
 * siempre: pasando NULL cae en el buffer estatico del modulo, que es
 * compartido, y entonces el puntero vale solo hasta la siguiente peticion
 * de quien sea. La garantia es del buffer, no de la estructura.
 *
 * tlsStatus() se queda para el panel de depuracion, que enseña tiempos y
 * cifrado y a quien no le importa ir un fotograma desfasado. */
typedef struct {
	int         http_status;
	const char *body;      /* dentro de TU sink, o NULL */
	u32         len;
	int         ok;        /* 1 si la peticion llego a completarse */
	char        err[160];
} tlsResult;

/* Lo mismo, pero diciendo DONDE cae la respuesta. El buffer de serie son
 * TLS_BODY_MAX bytes, de sobra para JSON y ridiculo para una imagen o un
 * catalogo de un mega.
 *
 * Va como parametro y no como un tlsSetSink() aparte por un motivo que
 * costo un crasheo: el buffer solo se puede tocar CON EL TURNO COGIDO. Si
 * se apunta antes y el turno no llega -porque el otro hilo esta en mitad
 * de una peticion- quien llama libera su buffer y el modulo se queda con
 * el puntero. La siguiente peticion del otro hilo escribe ahi.
 *
 * buf tiene que seguir vivo hasta terminar de leer el cuerpo. Devuelve 0,
 * -2 si el modulo esta ocupado (reintenta mas tarde), negativo con otro
 * motivo si no. */
int tlsRequestTo(const char *host, const char *path, const char *method,
                 const char *ctype, const char *reqbody, const char *headers,
                 void *sink, u32 sink_cap, tlsResult *out);

/* Parte una URL en servidor y ruta. Acepta https://, http:// y las que
 * empiezan por // sin esquema, que es como manda las imagenes la tienda de
 * Microsoft. Devuelve 0, o negativo con el motivo. */
int tlsSplitUrl(const char *url, char *host, u32 hmax, char *path, u32 pmax);

#endif /* GR33N_NET_TLS_H */
