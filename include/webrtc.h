/* GR33N - la primera prueba de que libpeer arranca en la consola.
 *
 * NO ES UNA CONEXION. No habla con xCloud, no manda un paquete, no negocia
 * nada. Lo que hace es despertar la pila entera de WebRTC en la PS3 y
 * cronometrar cada paso, que es informacion que hace falta antes de
 * escribir la capa de verdad.
 *
 * TRES COSAS QUE SE QUIEREN SABER, Y LAS TRES DUELEN SI SE DESCUBREN TARDE:
 *
 *   1. Cuanto tarda peer_connection_create(). Ahi dentro se genera un
 *      certificado autofirmado con mbedtls_rsa_gen_key, y una clave RSA de
 *      2048 bits en un PPU de 2006 puede ser un rato muy largo. Si son
 *      cuarenta segundos, hay que cambiar a ECDSA -- y mejor saberlo ahora
 *      que cuando este todo montado encima.
 *
 *   2. Cuanto dura de verdad un usleep y un select de 1 ms. Todos los
 *      contadores de green-nx cuentan VUELTAS: AGENT_CONNCHECK_MAX son 1500
 *      iteraciones dando por hecho que cada select de 1 ms tarda uno o dos.
 *      Si aqui redondea al tick del sistema, ese presupuesto se multiplica
 *      por cinco o por diez y la negociacion ICE se come el tiempo de
 *      espera de la aplicacion. Es lo mismo que ya paso con FRAME_WAIT_US,
 *      calibrado contra localhost.
 *
 *   3. Que la oferta SDP que sale sea la que xCloud espera. El parche lleva
 *      la plantilla de green-nx, pero compilar un literal y que ese literal
 *      salga en la cadena son dos cosas distintas. Se comprueba sobre el
 *      texto generado, no sobre el codigo fuente.
 */

#ifndef GR33N_WEBRTC_H
#define GR33N_WEBRTC_H

#include <ppu-types.h>
#include "input.h"

typedef enum {
	WRTC_IDLE = 0,
	WRTC_MIDIENDO,      /* usleep y select                */
	WRTC_INIT,          /* peer_init                      */
	WRTC_CREANDO,       /* peer_connection_create: la clave RSA */
	WRTC_OFERTA,        /* peer_connection_create_offer   */
	WRTC_CERRANDO,
	WRTC_LISTO,
	WRTC_FALLO
} wrtcState;

typedef struct {
	wrtcState state;

	/* Lo medido, en microsegundos. */
	u32 us_usleep_1000;   /* lo que dura de verdad un usleep(1000)  */
	u32 us_usleep_20000;  /* y un usleep(20000)                     */
	u32 us_select_1000;   /* select con 1 ms: en la PS3 sale 0, no  */
	                      /* se puede llamar (ver webrtc.c)         */
	u32 us_poll_1000;     /* netPoll con 1 ms: ESTE es el bueno, y  */
	                      /* el que decide los contadores de ICE    */

	u32 ms_init;
	u32 ms_create;        /* EL NUMERO QUE IMPORTA: la clave RSA    */
	u32 ms_offer;
	u32 ms_close;

	/* La oferta: cuanto mide y si lleva lo que tiene que llevar. */
	u32 sdp_len;
	int sdp_pt102;        /* m=video ... SAVPF 102                  */
	int sdp_fmtp;         /* la plantilla exacta de xCloud          */
	int sdp_setup_active; /* a=setup:active: somos cliente DTLS     */
	int sdp_fingerprint;  /* a=fingerprint:sha-256                  */
	int sdp_recvonly;
	int sdp_remb;         /* a=rtcp-fb:102 goog-remb                */
	int sdp_host;         /* a=candidate ... typ host: la LAN       */
	int sdp_srflx;        /* a=candidate ... typ srflx: la publica   */

	char err[224];
} wrtcInfo;

int  wrtcInit(void);
void wrtcShutdown(void);

/* Lanza la prueba. No bloquea: el trabajo va en su propio hilo, porque la
 * generacion de la clave puede tardar y el hilo de dibujo no puede pararse
 * a esperarla. */
void wrtcProbe(void);

const wrtcInfo *wrtcStatus(void);

/* --------------------------------------------------------------------- */
/* La sesion de verdad                                                   */
/* --------------------------------------------------------------------- */

/* Lo de arriba es la prueba. Esto es la conexion que se queda viva
 * mientras session.c negocia con xCloud y luego se bombea hasta que ICE
 * conecta.
 *
 * EL ORDEN NO ES DECORATIVO, y esta copiado de green-nx porque alli costo
 * descubrirlo:
 *
 *   1. wrtcSesionStun()       antes de crear, si /configuration los trae
 *   2. wrtcSesionCrear()
 *   3. wrtcSesionOferta()     el SDP que hay que mandar por /sdp
 *   4. wrtcSesionCandidato()  TODOS los remotos, uno a uno
 *   5. wrtcSesionRespuesta()  y SOLO ENTONCES la respuesta
 *   6. wrtcSesionBombear(1)
 *
 * El 4 va antes que el 5 porque libpeer arma los pares de candidatos UNA
 * sola vez, dentro de set_remote_description. Un candidato anadido
 * despues no entra en ningun par y no se usa nunca, sin un solo aviso. */

/* Servidor STUN/TURN a usar. Se copia: el puntero que guarda libpeer
 * tiene que sobrevivir a quien llama. Con NULL o "" se limpia. */
void wrtcSesionStun(const char *url);

int         wrtcSesionCrear(void);
const char *wrtcSesionOferta(void);
int         wrtcSesionCandidato(const char *cand);

/* La respuesta se pasa VERBATIM. Ni reescribir, ni normalizar finales de
 * linea, ni recortar: un '\r' de mas se cuela en el ice-ufrag y las
 * comprobaciones STUN se firman con la clave equivocada. El servidor las
 * tira en silencio y la conexion no llega nunca. */
int         wrtcSesionRespuesta(const char *answer);

/* El estado del mando, camino del juego.
 *
 * Lo llama el hilo de dibujo justo despues de leer el DS3; el envio de
 * verdad lo hace el hilo del bombeo a 60 Hz. Solo copia, asi que se puede
 * llamar en cada fotograma sin miedo.
 *
 * El mapeo es FISICO: el ajuste de intercambiar X y O es del menu, y al
 * juego le sigue llegando Cross = A. */
void        wrtcSesionPad(const gr33nPad *p);

void        wrtcSesionBombear(int si);
void        wrtcSesionSoltar(void);

/* Estado de ICE tal cual lo da libpeer, o -1 si todavia no ha dicho
 * nada. El texto que le corresponde sale de wrtcIceTexto(). */
int         wrtcSesionIce(void);
const char *wrtcIceTexto(int estado);

#endif /* GR33N_WEBRTC_H */
