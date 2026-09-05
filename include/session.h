/* GR33N - abrir una sesion de juego en xCloud
 *
 * ESTO NO REPRODUCE NADA TODAVIA. Lo que hace es pedirle a Microsoft una
 * maquina y contarnos con detalle lo que contesta, y ese es todo el
 * proposito: hay dos preguntas abiertas que no se resuelven leyendo
 * codigo ajeno, solo preguntandole al servidor de verdad.
 *
 *   1. ¿HACE FALTA SISU? El gsToken salio con la cadena simple, cambiando
 *      solo el RelyingParty, y eso ya nos ahorro implementar ECDSA P-256
 *      con firma de peticiones. Pero nadie sabe si el gateway lo exige mas
 *      adelante. Si aqui contesta 401 o 403, la respuesta es que si, y
 *      entonces hay trabajo grande antes de tocar WebRTC. Mejor saberlo
 *      ahora que con medio cliente escrito.
 *
 *   2. ¿QUE PINTA TIENE LA OFERTA SDP? De eso depende cuanto WebRTC hay
 *      que portar de verdad. Es la diferencia entre "libpeer entero con
 *      usrsctp" y "algo bastante mas pequeno".
 *
 * Igual que con el catalogo y la tienda, aqui NO se adivina la forma de la
 * respuesta: se piden los sospechosos habituales, y si ninguno cuela se
 * registran las claves de primer nivel y el cuerpo en bruto. Las dos veces
 * anteriores eso convirtio una suposicion equivocada en un dato en una
 * sola ejecucion.
 *
 * OJO CON UNA COSA: una sesion RESERVA UNA MAQUINA en un centro de datos
 * de Microsoft, y va contra la cuota de la cuenta. Por eso sesStop() hace
 * un DELETE de verdad y se llama tambien al salir de la aplicacion. Dejar
 * sesiones colgando es quedarse sin poder jugar.
 */

#ifndef GR33N_SESSION_H
#define GR33N_SESSION_H

#include <ppu-types.h>

/* EL ORDEN DE VERDAD, que costo tres vueltas averiguar:
 *
 *   POST /play                      -> 202, sessionPath
 *   GET  /state                     -> "ReadyToConnect"
 *   POST /connect  {userToken:...}  <- LA MAQUINA NO ES TUYA HASTA AQUI
 *   GET  /state                     -> "Provisioning" -> "Provisioned"
 *   GET  /configuration             -> con quien se habla
 *
 * "ReadyToConnect" no es un estado, es una orden: quiere decir "listo para
 * que TE conectes", no "listo para jugar". Pedir la configuracion ahi
 * contesta 410 SessionNotActive, que es el servidor diciendo, con toda la
 * razon, que todavia no le has dicho quien eres. */
typedef enum {
	SES_IDLE = 0,
	SES_ASKING,        /* POST .../play, pidiendo la maquina    */
	SES_QUEUED,        /* hay cola: WaitingForResources         */
	SES_CONNECTING,    /* POST .../connect, el token de traspaso */
	SES_PROVISIONING,  /* nos la estan preparando               */
	SES_CONFIG,        /* pidiendo la configuracion de conexion */
	SES_READY,         /* tenemos con quien hablar              */
	SES_FAILED
} sesState;

typedef struct {
	sesState state;

	char title_id[64];

	/* La ruta que devuelve el servidor. Todo lo demas cuelga de aqui:
	 * /state, /configuration, /sdp, /ice... y el DELETE para soltarla. */
	char path[192];

	/* Lo que dice el servidor tal cual, sin traducir. Si manda un estado
	 * que no conocemos, se ve en pantalla en vez de convertirse en un
	 * "desconocido" que no ayuda a nadie. */
	char server_state[64];

	/* Cola. -1 mientras no se sepa. */
	int  queue_pos;
	u32  wait_s;

	/* Cuanto se ha tardado en llegar a READY, y cuantas veces se ha
	 * preguntado por el estado. */
	u32  ms;
	u32  polls;

	/* El codigo HTTP del ultimo paso, que es lo primero que se mira
	 * cuando algo va mal. 401 o 403 en el POST = SISU. */
	int  http_status;

	/* Lo poco que se saca de /configuration, CON DOS AVISOS.
	 *
	 * server_ip NO es con quien se habla: es un candidato de relleno que
	 * no contesta a STUN. El bueno llega goteando por /ice.
	 *
	 * have_srtp dice que el servidor manda una clave SRTP en claro por
	 * HTTPS. Parece un atajo para saltarse DTLS y NO lo es: ningun cliente
	 * que funcione la lee, y la oferta SDP real lleva a=fingerprint. Se
	 * guarda porque saber que llega dice que la sesion esta viva. */
	char server_ip[64];
	u32  server_port;
	int  have_srtp;
	u32  keepalive_s;   /* keepAlivePulseInSeconds: lo dice el servidor */

	char err[224];
} sesInfo;

int  sesInit(void);
void sesShutdown(void);

/* Pide una maquina para este titulo. No bloquea. */
int  sesStart(const char *title_id);

/* Suelta la maquina. Se puede llamar siempre: si no hay sesion, no hace
 * nada. BLOQUEA lo que tarde el DELETE, porque se llama al salir. */
void sesStop(void);

const sesInfo *sesStatus(void);

#endif /* GR33N_SESSION_H */
