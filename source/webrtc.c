/* GR33N - despertar libpeer en la consola y cronometrarlo.
 *
 * Ver el comentario largo de include/webrtc.h para el por que. En corto:
 * las cuatro bibliotecas compilan y enlazan, pero no ha pasado una sola
 * instruccion por ellas en una PS3. Esto es lo primero que las ejecuta.
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ppu-types.h>
#include <ppu-asm.h>
#include <sys/thread.h>
#include <sys/mutex.h>
#include <sys/systime.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <net/net.h>

#include <peer.h>
#include <peer_connection.h>

#include "gr33n.h"
#include "link.h"
#include "webrtc.h"
#include "xcmsg.h"
#include "vjitter.h"
#include "decoder.h"
#include "input.h"
#include "ui.h"
#include "aud.h"

#define WRTC_THREAD_PRIO   1006
/* Generosa a proposito: por aqui pasa la generacion de una clave RSA y el
 * armado del SDP, y una pila corta en un hilo que no se ejecuta a menudo es
 * de los fallos que solo aparecen en hardware. */
#define WRTC_THREAD_STACK  (256 * 1024)

static sys_ppu_thread_t wrtc_tid;
static volatile int wrtc_running = 0;
static volatile int probe_req = 0;
static int wrtc_started = 0;

static wrtcInfo info;

/* Los servidores STUN que dice xCloud en /configuration. Se guardan aqui
 * porque PeerConfiguration guarda el PUNTERO, no una copia: si esto
 * viviera en la pila de quien llama, libpeer leeria memoria muerta al
 * recolectar candidatos. */
static char stun_url[160];

/* El estado de la sesion de verdad va AQUI ARRIBA y no junto a sus
 * funciones, porque el hilo (wrtc_thread) y wrtcInit/wrtcShutdown lo
 * tocan y estan antes en el fichero. La primera version lo dejo al final
 * y no compilaba. */
static PeerConnection *ses_pc = NULL;
static sys_mutex_t     pc_mtx;
static int             pc_mtx_ok = 0;
static volatile int    bombear = 0;
static volatile int    ice_estado = -1;

/* La conversacion por los canales de datos.
 *
 * xCloud NO emite nada por conectarse: espera a que el cliente abra sus
 * canales, salude, y le cuente que aguanta. Hasta entonces la conexion
 * esta viva y muda. */
static volatile int    sctp_arriba = 0;   /* la asociacion SCTP esta lista */
static volatile int    canales_abiertos = 0;
static volatile int    saludo_enviado = 0;
static volatile int    arranque_hecho = 0;
static volatile u32    video_bytes = 0;
static volatile u32    video_paquetes = 0;
static u32             input_sec = 0;

/* El reensamblado de video. Grande (unos 800 KB entre el deposito de
 * paquetes y el buffer de la unidad de acceso), asi que estatico y no en
 * la pila de nadie. */
static vjBuf           vj;
static volatile u32    au_n = 0;
static volatile u32    au_bytes = 0;

/* El mando, de camino al servidor.
 *
 * Lo escribe el hilo de dibujo (que es quien lee el DS3) y lo lee el del
 * bombeo (que es quien puede hablar con libpeer). Un candado propio y
 * diminuto: coger PC_LOCK desde el hilo de dibujo seria esperar a que
 * termine una vuelta entera de peer_connection_loop, y eso son tirones en
 * la interfaz. Aqui solo se copian cuarenta bytes. */
static gr33nPad     pad_ultimo;
static sys_mutex_t  pad_mtx;
static int          pad_mtx_ok = 0;
/* Declarada aqui porque el volcado de estadisticas la usa y esta 100
 * lineas por encima de su definicion. Cuarta vez esta semana que meto un
 * uso por delante de su declaracion; ahora lo comprueba
 * deps/sonda-orden.sh en vez de mi memoria. */
static s32          eje_gatillo(s32 presion, int pulsado);

static u64          pad_t0 = 0;      /* cuando empezo el stream */
static u64          pad_ultimo_envio = 0;
static u32          pad_enviados = 0;

/* --------------------------------------------------------------------- */
/* libpeer SE ARRANCA UNA VEZ Y NO SE PARA HASTA SALIR                    */
/*                                                                       */
/* MEDIDO, log del 5 de septiembre a las 14:42:                          */
/*                                                                       */
/*   [795.89] [wrtc] sesion: bombeo parado                               */
/*   [796.91] [peer/WARN] sctp.c:548 usrsctp_finish() never completed;   */
/*                        its threads are still running                  */
/*   [796.91] [wrtc] sesion: soltada                                     */
/*   ...                                                                 */
/*   [827.51] [wrtc] sesion: servidor STUN stun:...                      */
/*   [832.57] [-] 127.0.0.1 se ha ido        <- la consola desaparece    */
/*                                                                       */
/* Entre esas dos ultimas lineas tenia que salir "sesion: conexion       */
/* creada". No sale. peer_connection_create() de la SEGUNDA sesion no    */
/* vuelve nunca.                                                         */
/*                                                                       */
/* Y el motivo esta escrito tres lineas mas arriba, en un aviso que      */
/* llevaba semanas saliendo en todos los logs y que estaba en la lista   */
/* de pendientes como "molesto": usrsctp_finish() no termina y sus hilos */
/* siguen vivos. peer_deinit() lo llama, falla, y peer_init() de la      */
/* siguiente sesion hace usrsctp_init() OTRA VEZ encima de una pila SCTP */
/* que todavia tiene hilos tocandola. Eso no es un fallo que de un       */
/* error: es memoria compartida entre dos versiones de la misma          */
/* biblioteca.                                                           */
/*                                                                       */
/* La forma correcta de usar libpeer es la que dice su propia API:       */
/* peer_init/peer_deinit son GLOBALES (srtp_init y usrsctp_init), y      */
/* peer_connection_create/destroy son POR CONEXION. Nosotros llamabamos  */
/* a los cuatro por sesion.                                              */
/*                                                                       */
/* Asi que ahora: init una vez, perezoso, y deinit solo al salir de la   */
/* aplicacion -- donde si sigue fallando ya da igual, porque el proceso  */
/* se acaba de todas formas.                                             */
/* --------------------------------------------------------------------- */

static int peer_listo = 0;

static int peer_arranca(void)
{
	if (peer_listo) return 0;

	if (peer_init() != 0) return -1;

	peer_listo = 1;
	return 0;
}

#define PC_LOCK()    do { if (pc_mtx_ok) sysMutexLock(pc_mtx, 0); } while (0)
#define PC_UNLOCK()  do { if (pc_mtx_ok) sysMutexUnlock(pc_mtx); } while (0)


/* El timebase de la PPE, igual que en main.c: una lectura de registro en
 * vez de una llamada al sistema. Aqui importa de verdad, porque lo que se
 * esta midiendo son esperas de un milisegundo -- medir eso con una syscall
 * seria medir el termometro. */
static u64 tb_hz = 79800000ull;

static u32 tb_to_us(u64 ticks)
{
	return (u32)((ticks * 1000000ull) / tb_hz);
}

/* Milisegundos desde que arranco la consola, con el registro de timebase.
 * El buffer de jitter solo necesita diferencias, asi que el origen da
 * igual mientras avance. */
static u64 ahora_ms(void)
{
	return (__gettime() * 1000ull) / tb_hz;
}

static void wlog(const char *fmt, ...)
{
	char buf[192];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	printf("[wrtc] %s\n", buf);
	linkLog("[wrtc] %s", buf);
}

static void wfail(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(info.err, sizeof(info.err), fmt, ap);
	va_end(ap);

	info.state = WRTC_FALLO;
	printf("[wrtc] %s\n", info.err);
	linkLog("[wrtc] FALLO: %s", info.err);
}

/* --------------------------------------------------------------------- */
/* 1. Cuanto dura de verdad una espera corta                             */
/* --------------------------------------------------------------------- */

/* Se mide la MEDIA de varias, no una sola.
 *
 * Una espera suelta puede caer justo antes o justo despues de un tick del
 * sistema y dar cualquier cosa. Lo que decide si los contadores de green-nx
 * valen aqui no es el mejor caso ni el peor: es cuanto cuesta una vuelta en
 * promedio, porque de eso es de lo que se componen sus 1500 iteraciones. */
static u32 medir_usleep(u32 us, int veces)
{
	u64 t0 = __gettime();
	int i;

	for (i = 0; i < veces; i++)
		usleep(us);

	return tb_to_us(__gettime() - t0) / (u32)veces;
}

/* Y lo mismo con select, que es lo que usa de verdad el agente de ICE
 * (agent.c:94). NO se mide netSelect: libpeer llama a select, y medir la
 * otra seria medir donde no es -- que es el error que mas veces ha aparecido
 * en este proyecto.
 *
 * POR QUE ESTA PARTIDA EN PASOS QUE SE ANUNCIAN.
 *
 * La primera vez que esto corrio se llevo por delante la aplicacion entera
 * --el hilo principal tambien, no solo este-- unos 150 ms despues de decir
 * "midiendo". El log se quedo en esa linea, o sea que no distinguia entre
 * el socket, el bind y el select, que son tres culpables con tres arreglos
 * distintos.
 *
 * Ahora cada paso se anuncia ANTES de darlo. linkLog envia en el acto desde
 * el hilo que lo llama (link.c:483), no encola para que lo suelte el
 * principal, asi que la linea sale aunque lo siguiente se cuelgue. Es la
 * unica forma de localizar algo que se lleva el proceso por delante. */
static u32 medir_select(u32 us, int veces)
{
	int fd, r1, limite;
	struct sockaddr_in a;
	fd_set r;
	struct timeval tv;
	u64 t0;
	int i;

	/* AQUI ESTA LA RAZON DE QUE ESTO COLGARA LA CONSOLA.
	 *
	 * En la PS3 un descriptor de socket no es un numero pequeno. El
	 * primero que reparte vale 1073741862, o sea 0x40000026. Y una
	 * fd_set de PSL1GHT mide 64 bytes.
	 *
	 * FD_SET no comprueba nada:
	 *
	 *   #define FD_SET(n,p) ((p)->fds_bits[(n)/NFDBITS] |= (1L << ...))
	 *
	 * con NFDBITS = 64. Asi que el indice sale 0x40000026/64 =
	 * 16.777.216, sobre un array de 16 elementos. Eso no es escribir un
	 * poco fuera: es escribir 64 MB mas alla de una variable de pila.
	 * Una sola llamada, y adios.
	 *
	 * (De paso: los 64 bytes tampoco cuadran con FD_SETSIZE=1024. La
	 * estructura de net/select.h dimensiona fds_bits con
	 * howmany(FD_SETSIZE, NFDBITS) --donde NFDBITS sale de fd_mask, de 8
	 * bytes-- pero declara los elementos como _net_fd_mask, que es
	 * unsigned int, de 4. La mitad de lo que cree. Da igual: aunque
	 * midiera lo "correcto", 1024 huecos no alcanzan para 0x40000026.
	 * select y fd_set no sirven en esta maquina, y punto.)
	 *
	 * FD_ZERO si es honesto: usa sizeof(*(p)), o sea que limpia lo que
	 * hay de verdad. El que miente es FD_SET a solas. */
	limite = (int)(sizeof(fd_set) * 8);

	wlog("select: FD_SETSIZE=%d  sizeof(fd_set)=%d bytes (%d fds de "
	     "verdad)", (int)FD_SETSIZE, (int)sizeof(fd_set), limite);
	if ((int)FD_SETSIZE != limite)
		wlog("select: OJO, FD_SETSIZE miente: dice %d y caben %d. El "
		     "limite bueno es sizeof(fd_set)*8.",
		     (int)FD_SETSIZE, limite);

	wlog("select: creando socket UDP...");
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		wlog("select: socket fallo (%d). No se mide.", fd);
		return 0;
	}
	wlog("select: fd=%d", fd);

	/* Y el limite se comprueba contra el tamano REAL, no contra
	 * FD_SETSIZE. La version anterior de esta linea comprobaba contra
	 * FD_SETSIZE, que es exactamente el numero que miente: dejaba pasar
	 * todo el rango 512..1023, que es justo el que corrompe. Una guarda
	 * puesta contra la cifra plausible en vez de contra la de verdad no
	 * es una guarda, es un adorno. */
	if (fd >= limite) {
		wlog("select: fd=%d y solo caben %d. FD_SET escribiria fuera "
		     "de la pila; se cancela la medida.", fd, limite);
		close(fd);
		return 0;
	}

	memset(&a, 0, sizeof(a));
	a.sin_len    = sizeof(a);
	a.sin_family = AF_INET;
	a.sin_port   = 0;            /* que elija el sistema */
	a.sin_addr.s_addr = INADDR_ANY;

	/* Si el bind falla se mide igual: para un select que solo va a vencer
	 * el tiempo de espera, un descriptor sin bind vale. Lo que no vale es
	 * no tener descriptor, porque un select con todos los conjuntos
	 * vacios puede tomar otro camino dentro del sistema. */
	wlog("select: bind...");
	i = bind(fd, (struct sockaddr *)&a, sizeof(a));
	wlog("select: bind devolvio %d", i);

	/* UNA sola vuelta antes de las cincuenta. Si es esta la que cuelga,
	 * el log se para en "primera vuelta" y ya sabemos que el problema es
	 * select y no el bucle ni la suma de esperas. */
	FD_ZERO(&r);
	FD_SET(fd, &r);
	tv.tv_sec  = 0;
	tv.tv_usec = (long)us;

	wlog("select: primera vuelta, nfds=%d, espera=%u us...", fd + 1, us);
	t0 = __gettime();
	r1 = select(fd + 1, &r, NULL, NULL, &tv);
	wlog("select: primera vuelta devolvio %d en %u us",
	     r1, tb_to_us(__gettime() - t0));

	wlog("select: y ahora las %d vueltas...", veces);
	t0 = __gettime();
	for (i = 0; i < veces; i++) {
		FD_ZERO(&r);
		FD_SET(fd, &r);
		tv.tv_sec  = 0;
		tv.tv_usec = (long)us;

		(void)select(fd + 1, &r, NULL, NULL, &tv);
	}
	i = (int)tb_to_us(__gettime() - t0);

	wlog("select: las %d vueltas, hechas", veces);
	close(fd);
	return (u32)i / (u32)veces;
}

/* Lo que SI se puede usar: netPoll.
 *
 * El descriptor va en un campo int de struct pollfd, no en un mapa de
 * bits con tope, asi que 0x40000026 le da igual. Y no es una apuesta:
 * source/link.c recibe por ahi desde el primer dia de este proyecto.
 *
 * Esta es la medida que de verdad hacia falta. El bucle de ICE de
 * libpeer da una vuelta por espera (agent.c, AGENT_POLL_TIMEOUT = 1 ms),
 * y AGENT_CONNCHECK_MAX cuenta VUELTAS, no tiempo. Con lo que cueste una
 * vuelta aqui se sabe si los contadores heredados de green-nx valen o
 * hay que recalibrarlos. */
/* PSL1GHT tiene DOS familias de red, y mezclarlas es el sospechoso.
 *
 * La POSIX de toda la vida --socket, bind, recvfrom, close-- y la suya
 * --netSocket, netBind, netRecvFrom, netClose, netPoll--. link.c usa la
 * segunda ENTERA y lleva meses recibiendo. webrtc.c pedia el socket con
 * la primera y luego lo pasaba a netPoll, que es de la segunda.
 *
 * Y netPoll contesto revents=0x20, o sea POLLNVAL: "ese descriptor no
 * existe". Sobre un descriptor al que bind() acababa de decir que si.
 * Las dos cosas solo caben a la vez si cada API mira en un sitio
 * distinto.
 *
 * Esto lo prueba en vez de suponerlo: el mismo programa, la misma
 * espera, cambiando SOLO de familia. La vez pasada cambie el protocolo
 * (0 contra IPPROTO_UDP) y deje la funcion fija, que era justo la otra
 * variable -- y encima la habia escrito dos mensajes antes.
 */
#define FAM_POSIX  0
#define FAM_NET    1

static u32 sondear_poll(const char *nombre, int familia, int proto,
                        int ms, int veces)
{
	int fd, r1;
	struct sockaddr_in a;
	struct pollfd pfd;
	u64 t0;
	u32 tardo;
	int i;

	fd = (familia == FAM_NET) ? netSocket(AF_INET, SOCK_DGRAM, proto)
	                          : socket(AF_INET, SOCK_DGRAM, proto);
	if (fd < 0) {
		wlog("netPoll[%s]: socket fallo (%d)", nombre, fd);
		return 0;
	}

	memset(&a, 0, sizeof(a));
	a.sin_len    = sizeof(a);
	a.sin_family = AF_INET;
	a.sin_port   = 0;            /* que elija el sistema */
	a.sin_addr.s_addr = INADDR_ANY;

	i = (familia == FAM_NET)
	    ? netBind(fd, (struct sockaddr *)&a, sizeof(a))
	    : bind(fd, (struct sockaddr *)&a, sizeof(a));
	wlog("netPoll[%s]: fd=%d  bind=%d", nombre, fd, i);

	/* CONTROL NEGATIVO. Nadie ha mandado nada a este socket, asi que lo
	 * correcto es devolver 0 al cabo del plazo. Se pide un plazo LARGO a
	 * proposito: con 1 ms no se distingue "ha esperado" de "ha vuelto al
	 * momento", y esa es justo la pregunta. */
	pfd.fd      = fd;
	pfd.events  = POLLIN;
	pfd.revents = 0;

	wlog("netPoll[%s]: sin nada que leer, esperando 200 ms...", nombre);
	t0 = __gettime();
	r1 = netPoll(&pfd, 1, 200);
	tardo = tb_to_us(__gettime() - t0);

	wlog("netPoll[%s]: devolvio %d revents=0x%x en %u us",
	     nombre, r1, (unsigned)pfd.revents, tardo);

	if (r1 != 0 || tardo < 150000) {
		if (r1 > 0 && (pfd.revents & POLLNVAL))
			wlog("netPoll[%s]: POLLNVAL. Este descriptor no le suena "
			     "a netPoll.", nombre);
		else if (r1 > 0 && (pfd.revents & POLLIN))
			wlog("netPoll[%s]: dice POLLIN sin que nadie haya enviado "
			     "nada. No se puede confiar en el.", nombre);
		else if (r1 > 0)
			wlog("netPoll[%s]: vuelve por un error del descriptor, no "
			     "por datos.", nombre);
		else
			wlog("netPoll[%s]: devolvio 0 pero solo tardo %u us de "
			     "200000. No respeta el plazo.", nombre, tardo);

		if (familia == FAM_NET) netClose(fd); else close(fd);
		return 0;
	}

	wlog("netPoll[%s]: BIEN. Espera de verdad y no se inventa nada.",
	     nombre);

	/* Y ahora si, el tiempo por vuelta con el plazo de verdad del bucle
	 * de ICE, que es lo que decide AGENT_CONNCHECK_MAX. */
	t0 = __gettime();
	for (i = 0; i < veces; i++) {
		pfd.fd      = fd;
		pfd.events  = POLLIN;
		pfd.revents = 0;
		(void)netPoll(&pfd, 1, ms);
	}
	tardo = tb_to_us(__gettime() - t0) / (u32)veces;

	if (familia == FAM_NET) netClose(fd); else close(fd);
	return tardo;
}

/* Plan B: un recvfrom que se rinde solo, sin poll de ninguna clase.
 *
 * Solo se prueba si netPoll no ha servido, porque tiene un riesgo que
 * las otras no: si el sistema ACEPTA SO_RCVTIMEO y luego no lo respeta,
 * el recvfrom se queda ahi para siempre y adios. Por eso va el ultimo y
 * por eso se anuncia antes de llamar: si el log se para en esa linea, ya
 * sabemos por que.
 *
 * Se prueba sobre la familia POSIX a proposito: es la que usa libpeer en
 * socket.c, o sea el arreglo mas pequeno si sale bien. */
static void probar_rcvtimeo(void)
{
	int fd, r;
	struct sockaddr_in a, de;
	struct timeval tv;
	socklen_t delen;
	char buf[64];
	u64 t0;
	u32 tardo;

	fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (fd < 0) {
		wlog("rcvtimeo: socket fallo (%d)", fd);
		return;
	}

	memset(&a, 0, sizeof(a));
	a.sin_len    = sizeof(a);
	a.sin_family = AF_INET;
	a.sin_port   = 0;
	a.sin_addr.s_addr = INADDR_ANY;
	r = bind(fd, (struct sockaddr *)&a, sizeof(a));

	tv.tv_sec  = 0;
	tv.tv_usec = 200000;
	r = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	wlog("rcvtimeo: bind ok, setsockopt(SO_RCVTIMEO, 200 ms) -> %d", r);

	if (r != 0) {
		wlog("rcvtimeo: no lo acepta. Quedaria socket no bloqueante + "
		     "usleep, que si mide bien (%u us por milisegundo pedido).",
		     info.us_usleep_1000);
		close(fd);
		return;
	}

	wlog("rcvtimeo: recvfrom que DEBERIA rendirse en 200 ms. Si el log "
	     "se para aqui, lo acepta pero no lo respeta.");

	delen = sizeof(de);
	memset(&de, 0, sizeof(de));
	t0 = __gettime();
	r = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&de, &delen);
	tardo = tb_to_us(__gettime() - t0);

	wlog("rcvtimeo: recvfrom devolvio %d en %u us (pedidos 200000)",
	     r, tardo);

	if (tardo > 150000 && tardo < 400000)
		wlog("rcvtimeo: FUNCIONA. Ese es el camino para libpeer: un "
		     "recvfrom con plazo, sin poll y sin fd_set.");
	else
		wlog("rcvtimeo: el plazo no se respeta. Queda socket no "
		     "bloqueante + usleep.");

	close(fd);
}

static u32 medir_netpoll(int ms, int veces)
{
	u32 r;

	/* Los valores, para poder leer el revents de arriba sin adivinar.
	 *
	 * Con #ifdef porque link.c solo usa POLLIN, o sea que de los otros
	 * tres no consta que PSL1GHT los declare. Dar por hecho que estan
	 * seria cambiar una ronda de "por que devuelve 1" por una de "no
	 * compila", y ya llevamos bastantes. Un -1 en el log significa "esta
	 * cabecera no lo define", que tambien es informacion. */
#ifndef POLLERR
#define POLLERR (-1)
#endif
#ifndef POLLHUP
#define POLLHUP (-1)
#endif
#ifndef POLLNVAL
#define POLLNVAL (-1)
#endif
	wlog("netPoll: POLLIN=%d POLLERR=%d POLLHUP=%d POLLNVAL=%d "
	     "(-1 = no lo declara la cabecera)",
	     (int)POLLIN, (int)POLLERR, (int)POLLHUP, (int)POLLNVAL);

	/* El contraste primero, para que las dos lineas queden juntas en el
	 * mismo log y no haya que comparar contra el recuerdo de otro dia. */
	r = sondear_poll("socket() POSIX", FAM_POSIX, IPPROTO_UDP, ms, veces);
	if (r > 0) return r;

	r = sondear_poll("netSocket()", FAM_NET, IPPROTO_UDP, ms, veces);
	if (r > 0) return r;

	wlog("netPoll: no sirve con ninguna de las dos familias.");
	probar_rcvtimeo();
	return 0;
}

/* --------------------------------------------------------------------- */
/* 2. La oferta                                                          */
/* --------------------------------------------------------------------- */

/* La plantilla que xCloud exige, LITERAL Y ENTERA.
 *
 * Una coincidencia a medias hace que el servidor conteste "m=video 0" y no
 * haya video. Y peor: si falta packetization-mode NO rechaza, cae en un
 * modo de capacidad minima con el codificador clavado en paquetes de un
 * solo NALU a unos 150 kbps. O sea que conecta, se ve imagen, y es un
 * churro -- que es el fallo mas dificil de diagnosticar de los dos.
 *
 * Se comprueba sobre el texto GENERADO. Que el literal este en sdp.c es una
 * cosa y que salga en la cadena es otra: entre medias hay un sdp_append con
 * un buffer que podria recortar. */
#define FMTP_XCLOUD \
	"a=fmtp:102 level-asymmetry-allowed=0;packetization-mode=1;" \
	"profile-level-id=42e01f;max-fs=3600;max-mbps=108000"

static void revisar_oferta(const char *sdp)
{
	info.sdp_len          = (u32)strlen(sdp);
	info.sdp_pt102        = strstr(sdp, "m=video 9 UDP/TLS/RTP/SAVPF 102") != NULL;
	info.sdp_fmtp         = strstr(sdp, FMTP_XCLOUD) != NULL;
	info.sdp_setup_active = strstr(sdp, "a=setup:active") != NULL;
	info.sdp_fingerprint  = strstr(sdp, "a=fingerprint:sha-256") != NULL;
	info.sdp_recvonly     = strstr(sdp, "a=recvonly") != NULL;
	info.sdp_remb         = strstr(sdp, "a=rtcp-fb:102 goog-remb") != NULL;
	info.sdp_host         = strstr(sdp, "typ host") != NULL;
	info.sdp_srflx        = strstr(sdp, "typ srflx") != NULL;

	wlog("oferta: %u bytes", info.sdp_len);
	wlog("  m=video SAVPF 102 : %s", info.sdp_pt102 ? "SI" : "NO <-- xCloud rechaza el video");
	wlog("  fmtp de xCloud    : %s", info.sdp_fmtp  ? "SI" : "NO <-- 150 kbps y single-NALU");
	wlog("  a=setup:active    : %s", info.sdp_setup_active ? "SI" : "NO <-- no hay saludo DTLS");
	wlog("  a=fingerprint     : %s", info.sdp_fingerprint ? "SI" : "NO <-- sin DTLS no hay claves");
	wlog("  a=recvonly        : %s", info.sdp_recvonly ? "SI" : "NO");
	wlog("  goog-remb         : %s", info.sdp_remb ? "SI" : "NO <-- el REMB se ignora");
	wlog("  candidato host    : %s", info.sdp_host ? "SI" : "NO <-- getifaddrs no da direccion");
	wlog("  candidato srflx   : %s", info.sdp_srflx ? "SI" : "NO <-- sin direccion publica no hay Azure");
}

/* El SDP entero al log, linea a linea.
 *
 * Se parte por saltos de linea y no en trozos de 180 bytes porque el SDP ES
 * lineas: cortarlo por bytes deja atributos partidos por la mitad y hay que
 * pegarlos a mano en el PC. Y este texto es exactamente lo que habra que
 * enviarle a xCloud, asi que conviene poder copiarlo tal cual. */
static void volcar_oferta(const char *sdp)
{
	const char *p = sdp;
	int n = 0;

	wlog("--- oferta SDP ---");
	while (*p && n < 80) {
		char linea[184];
		const char *fin = strchr(p, '\n');
		size_t len = fin ? (size_t)(fin - p) : strlen(p);

		if (len > 0 && p[len - 1] == '\r')
			len--;
		if (len >= sizeof(linea))
			len = sizeof(linea) - 1;

		memcpy(linea, p, len);
		linea[len] = '\0';
		linkLog("[sdp] %s", linea);

		n++;
		if (!fin) break;
		p = fin + 1;
	}
	wlog("--- fin de la oferta (%d lineas) ---", n);
}

/* --------------------------------------------------------------------- */
/* La prueba                                                             */
/* --------------------------------------------------------------------- */

static void correr_prueba(void)
{
	PeerConfiguration cfg;
	PeerConnection *pc = NULL;
	const char *sdp;
	wrtcState veredicto;
	u64 t;

	memset(&info, 0, sizeof(info));

	/* --- 1. las esperas ------------------------------------------- */

	info.state = WRTC_MIDIENDO;
	wlog("midiendo lo que dura de verdad una espera corta");

	/* Cada una se anuncia y se cuenta por separado. Medir las tres y
	 * contarlas al final fue lo que dejo la primera ejecucion sin poder
	 * decir cual de las tres colgo la consola. */
	wlog("usleep: 50 vueltas de 1 ms...");
	info.us_usleep_1000  = medir_usleep(1000, 50);
	wlog("usleep(1000)  -> %u us de verdad", info.us_usleep_1000);

	wlog("usleep: 5 vueltas de 20 ms...");
	info.us_usleep_20000 = medir_usleep(20000, 5);
	wlog("usleep(20000) -> %u us de verdad", info.us_usleep_20000);

	/* select va la ULTIMA, al final del todo. Ver el comentario del paso
	 * 6: es la que se llevo la aplicacion por delante, y las respuestas
	 * caras (RSA, el SDP) no se pueden perder por eso otra vez. */

	/* --- 2. arrancar libpeer --------------------------------------- */

	info.state = WRTC_INIT;
	t = __gettime();
	if (peer_arranca() != 0) {
		wfail("peer_init fallo");
		return;
	}
	info.ms_init = tb_to_us(__gettime() - t) / 1000u;
	/* En la segunda pasada saldra 0 ms, y es correcto: ya estaba
	 * arrancado. El numero que interesaba de esta medida era el de la
	 * PRIMERA vez, que es la que paga srtp_init y usrsctp_init. */
	wlog("peer_init: %u ms%s", info.ms_init,
	     info.ms_init == 0 ? " (ya estaba arrancado)" : "");

	/* --- 3. la conexion, que es donde esta el riesgo ---------------- */

	info.state = WRTC_CREANDO;
	wlog("creando la conexion (aqui dentro se genera la clave RSA; "
	     "puede tardar)");

	memset(&cfg, 0, sizeof(cfg));

	/* UN SERVIDOR STUN, Y NO ES EL QUE SE USARA DE VERDAD.
	 *
	 * Los de xCloud llegan en /configuration cuando haya sesion. Este
	 * esta aqui para contestar una pregunta que el candidato host no
	 * contesta: si la consola sabe averiguar su direccion PUBLICA.
	 *
	 * El log del 2026-09-04 ya saca "a=candidate:0 1 UDP 2128600831
	 * 192.168.1.150 57310 typ host", que es la IP de la LAN. Un servidor
	 * de Azure no puede hacer nada con esa: hace falta un candidato
	 * srflx, y para eso hay que mandar un Binding Request por el socket
	 * y entender la respuesta. Eso ejercita agent_create_stun_addr, el
	 * analizador de STUN de libpeer (que la auditoria dio por limpio en
	 * big-endian pero nunca se ejecuto) y el camino de envio/recepcion
	 * por la familia net*.
	 *
	 * Sin riesgo de cuelgue: agent_socket_recv_attempts reintenta
	 * AGENT_STUN_RECV_MAXTIMES (1000) veces con netPoll de 1 ms, o sea
	 * un tope de algo mas de un segundo si no contesta nadie. */
	cfg.ice_servers[0].urls = "stun:stun.l.google.com:19302";

	cfg.video_codec = CODEC_H264;
	cfg.datachannel = DATA_CHANNEL_STRING;

	t = __gettime();
	pc = peer_connection_create(&cfg);
	info.ms_create = tb_to_us(__gettime() - t) / 1000u;

	if (pc == NULL) {
		wfail("peer_connection_create devolvio NULL tras %u ms",
		      info.ms_create);
		return;
	}

	wlog("peer_connection_create: %u ms", info.ms_create);
	if (info.ms_create > 10000)
		wlog("   ESO ES MUCHO. La clave RSA se lleva casi todo: mirar "
		     "si conviene ECDSA (mbedtls_pk_setup con MBEDTLS_PK_ECKEY).");

	/* --- 4. la oferta ---------------------------------------------- */

	info.state = WRTC_OFERTA;
	t = __gettime();
	sdp = peer_connection_create_offer(pc);
	info.ms_offer = tb_to_us(__gettime() - t) / 1000u;

	if (sdp == NULL) {
		wfail("peer_connection_create_offer devolvio NULL");
		peer_connection_destroy(pc);
		return;
	}

	wlog("peer_connection_create_offer: %u ms", info.ms_offer);
	revisar_oferta(sdp);
	volcar_oferta(sdp);

	/* --- 5. recoger ------------------------------------------------ */

	info.state = WRTC_CERRANDO;
	t = __gettime();
	peer_connection_destroy(pc);
	info.ms_close = tb_to_us(__gettime() - t) / 1000u;
	wlog("cierre: %u ms", info.ms_close);

	/* Que la oferta sea VALIDA es lo que decide si esto ha ido bien. Que
	 * las llamadas no revienten es el minimo, no el objetivo. */
	if (!info.sdp_fmtp || !info.sdp_setup_active || !info.sdp_fingerprint) {
		wfail("la oferta sale, pero le falta algo que xCloud exige "
		      "(fmtp:%d setup:%d fingerprint:%d)",
		      info.sdp_fmtp, info.sdp_setup_active, info.sdp_fingerprint);
		veredicto = WRTC_FALLO;
	} else {
		veredicto = WRTC_LISTO;
		wlog("libpeer arranca en la consola y la oferta es la que "
		     "xCloud pide.");
	}

	/* --- 6. select, la ultima a proposito -------------------------- */

	/* Va aqui, detras del veredicto, porque en la primera ejecucion se
	 * llevo la aplicacion entera por delante estando la PRIMERA. Con ese
	 * orden, un cuelgue costaba las tres respuestas caras --cuanto tarda
	 * la clave RSA, si el SDP lleva el fmtp de xCloud, si el cierre
	 * libera-- por medir una espera de un milisegundo.
	 *
	 * Una prueba que se suicida antes de contestar lo que fue a
	 * preguntar no es una prueba. Ahora, si vuelve a colgarse, se cuelga
	 * habiendo contestado. */
	info.state = WRTC_MIDIENDO;
	wlog("y ahora select, que es lo que colgo la consola la vez pasada");
	info.us_select_1000 = medir_select(1000, 50);

	/* Y netPoll, que es por donde va a ir de verdad. El parche de
	 * libpeer cambia los tres select por netPoll, asi que ESTE es el
	 * numero que manda para los contadores de ICE. */
	info.us_poll_1000 = medir_netpoll(1, 50);
	info.state = veredicto;

	if (info.us_poll_1000 > 0) {
		u32 conncheck_ms = (info.us_poll_1000 * 1500u) / 1000u;

		wlog("netPoll(1 ms) -> %u us de verdad", info.us_poll_1000);

		/* Lo que eso significa para los contadores heredados, dicho
		 * aqui en vez de dejarlo para que alguien lo calcule luego. */
		wlog("=> AGENT_CONNCHECK_MAX (1500 vueltas) serian %u ms aqui",
		     conncheck_ms);
		if (conncheck_ms > 6000)
			wlog("   OJO: green-nx cuenta con unos 1500-3000 ms. "
			     "Hay que recalibrar, no copiar.");
		else if (conncheck_ms < 1000)
			wlog("   OJO al otro lado: %u ms puede ser POCO para un "
			     "servidor de Azure lejano. Se rendiria antes de "
			     "tiempo.", conncheck_ms);
		else
			wlog("   Eso cae dentro de los 1500-3000 ms que da por "
			     "hecho green-nx: los contadores valen tal cual.");
	} else {
		wlog("netPoll: sin medida. Sin ella no se pueden recalibrar los "
		     "contadores de ICE, que cuentan vueltas y no tiempo.");
	}
}

/* --------------------------------------------------------------------- */

/* --------------------------------------------------------------------- */
/* Los canales de datos: la conversacion que hace que llegue video        */
/* --------------------------------------------------------------------- */

/* xCloud no empieza a emitir por conectarse. Hay que abrirle cuatro
 * canales por DCEP, saludarle por "message", y cuando conteste, contarle
 * quien eres. La secuencia es la del cliente web, sacada de green-nx.
 *
 * LOS SID SON PARES A PROPOSITO. Por la RFC 8832 el lado que actua de
 * cliente DTLS --nosotros, que mandamos a=setup:active-- usa los
 * identificadores de flujo pares. Si los cogieramos impares chocariamos
 * con los que abra el servidor.
 *
 * Y "input" va FIABLE Y ORDENADO, que no es una preferencia: un informe
 * v1 es el estado absoluto del mando numerado por una secuencia, y el
 * servidor deja de aplicar entrada en cuanto ve un hueco. Un canal no
 * fiable convierte una perdida corriente de UDP en exactamente ese hueco,
 * y el mando se queda muerto sin que nada lo diga. */

#define SID_CONTROL  0
#define SID_INPUT    2
#define SID_MESSAGE  4
#define SID_CHAT     6

/* Lo que declaramos que aguanta la consola. La superficie son 1280x720 y
 * cellVdec hace 135 fps de 720p, asi que 720p60 es lo suyo. El tope de
 * bitrate es el que usa green-nx para su nivel de 720. */
#define XC_ANCHO   1280
#define XC_ALTO     720
#define XC_KBPS   10000
#define XC_FPS       60

static void abrir_canales(void)
{
	struct { const char *label; const char *proto; u16 sid; } c[] = {
		{ "control", "controlV1", SID_CONTROL },
		{ "input",   "1.0",       SID_INPUT   },
		{ "message", "messageV1", SID_MESSAGE },
		{ "chat",    "chatV1",    SID_CHAT    },
	};
	int i;

	for (i = 0; i < 4; i++)
		peer_connection_create_datachannel_sid(
			ses_pc, DATA_CHANNEL_RELIABLE, 0, 0,
			(char *)c[i].label, (char *)c[i].proto, c[i].sid);

	wlog("canales abiertos: control/input/message/chat");
}

/* Con el candado YA COGIDO. Se llama desde el bombeo y desde el callback
 * de mensajes, que ya vienen de dentro de peer_connection_loop(). */
static void enviar_texto(u16 sid, const char *txt)
{
	if (ses_pc == NULL || txt == NULL) return;
	peer_connection_datachannel_send_text_sid(ses_pc, (char *)txt,
	                                          strlen(txt), sid);
}

/* La rafaga de arranque, cuando el servidor ha contestado al saludo. */
static void secuencia_arranque(void)
{
	char buf[1024];
	u8 bin[XC_METADATA_LEN];
	int i;

	wlog("HandshakeAck: mandando capacidades");

	enviar_texto(SID_CONTROL, xcAuthorizationRequest());

	if (xcGamepadChanged(buf, sizeof(buf), 0, 1))
		enviar_texto(SID_CONTROL, buf);

	if (xcResolution(buf, sizeof(buf), "720"))
		enviar_texto(SID_CONTROL, buf);

	for (i = 0; i < XC_STARTUP_N; i++)
		if (xcStartup(buf, sizeof(buf), i, XC_ANCHO, XC_ALTO,
		              XC_KBPS, XC_FPS))
			enviar_texto(SID_MESSAGE, buf);

	/* Los metadatos consumen numero de secuencia aunque sean lo primero
	 * que va por "input": la cuenta empieza aqui. */
	if (xcInputMetadata(bin, sizeof(bin), ++input_sec, 0))
		peer_connection_datachannel_send_sid(ses_pc, (char *)bin,
		                                     XC_METADATA_LEN, SID_INPUT);

	/* Y se pide un fotograma clave por las DOS vias: el PLI de RTCP, que
	 * es al que xCloud hace caso, y el mensaje de aplicacion. Sin esto
	 * hay que esperar al periodico del servidor, que puede tardar. */
	peer_connection_request_keyframe(ses_pc);
	enviar_texto(SID_CONTROL, xcKeyframeRequested());

	wlog("secuencia de arranque enviada (%dx%d @%d, %d kbps)",
	     XC_ANCHO, XC_ALTO, XC_FPS, XC_KBPS);
}

/* --- los callbacks de libpeer ---------------------------------------- */

/* SCTP arriba. Llega desde dentro de peer_connection_loop(), o sea con el
 * candado ya cogido por el bombeo: aqui NO se puede volver a cogerlo ni
 * hacer nada que tarde. Solo se levanta una bandera. */
static void on_canal_abierto(void *ud)
{
	(void)ud;
	sctp_arriba = 1;
}

static void on_canal_cerrado(void *ud)
{
	(void)ud;
	sctp_arriba = 0;
	wlog("SCTP: la asociacion se ha cerrado");
}

static void on_canal_mensaje(char *msg, size_t len, void *ud, u16 sid)
{
	char *label;

	(void)ud;

	label = peer_connection_lookup_sid_label(ses_pc, sid);

	if (label && strcmp(label, "message") == 0 && !arranque_hecho) {
		if (xcIsHandshakeAck(msg, len)) {
			arranque_hecho = 1;
			secuencia_arranque();
			return;
		}
	}

	/* Todo lo demas, a la vista y recortado: por "message" llega mucho
	 * texto y llenar el log de eso taparia lo que importa. */
	wlog("<- [%s] %.90s", label ? label : "?", msg ? msg : "");
}

/* El video. De momento solo se cuenta: pasarlo al decodificador es el
 * paso siguiente, y mezclar las dos cosas dejaria sin saber si el fallo
 * es que no llega o que no se decodifica. */
/* Una unidad de acceso completa: al decodificador.
 *
 * Esto corre DENTRO de peer_connection_loop(), o sea en el hilo que bombea
 * los sockets. decFeedAu solo copia a una ranura y vuelve -- no abre nada,
 * no espera a cellVdec-- porque pararse aqui deja que se desborde la cola
 * de recepcion UDP, y eso se ve como un tiron de video cada pocos
 * segundos. */
static void on_au(const u8 *au, size_t n, void *ud)
{
	(void)ud;

	au_n++;
	au_bytes += (u32)n;

	decFeedAu(au, (u32)n);

	/* La primera SIEMPRE se mira, porque es el IDR que abre el flujo y
	 * es donde se veria si el codigo de arranque sale al reves. */
	if (au_n == 1 || (au_n % 120) == 0)
		wlog("AU #%u: %u bytes, empieza %02x %02x %02x %02x %02x "
		     "(nal %u)", (unsigned)au_n, (unsigned)n,
		     au[0], au[1], au[2], au[3], au[4],
		     (unsigned)(au[4] & 0x1f));
}

static void on_nack(u16 pid, u16 blp, void *ud)
{
	(void)ud;
	/* Con el candado ya cogido: esto viene de dentro del bombeo. */
	if (ses_pc) peer_connection_send_nack(ses_pc, pid, blp);
}

/* El video que entra: RTP crudo, cabecera incluida. libpeer no despaqueta
 * nada (rtp.c:rtp_decode_h264 pasa el paquete entero), asi que el trabajo
 * de juntarlo es de vjitter.c. */
/* EL AUDIO, que hasta hoy no se pedia siquiera.
 *
 * libpeer entrega el paquete RTP ENTERO, cabecera incluida: eso lo hace el
 * parche, y a proposito, porque aud.c necesita el numero de secuencia para
 * reordenar antes de descodificar. Opus tiene estado; descodificar
 * desordenado no da error, da un chasquido por trama.
 *
 * Esto solo pasa el paquete. Lo caro --descodificar-- lo hace el hilo del
 * audio, porque este de aqui es el del bombeo y no puede pararse. */
static void on_audio(u8 *datos, size_t n, void *ud)
{
	(void)ud;
	audRtp(datos, (u32)n);
}

static void on_video(u8 *datos, size_t n, void *ud)
{
	int pedir = 0;

	(void)ud;

	video_bytes += (u32)n;
	video_paquetes++;

	vjRecibe(&vj, datos, n, ahora_ms(), on_au, on_nack, NULL,
	         &pedir);

	if (pedir && ses_pc) {
		/* Sin IDR no se sale de un corte: se pide por las dos vias,
		 * pero con freno -- pedir uno por cada paquete perdido es
		 * pedirle al servidor que emita solo fotogramas clave. */
		static u64 ultimo = 0;
		u64 ahora = ahora_ms();

		if (ahora - ultimo > 1000) {
			ultimo = ahora;
			peer_connection_request_keyframe(ses_pc);
			enviar_texto(SID_CONTROL, xcKeyframeRequested());
			wlog("hueco sin recuperar: pido fotograma clave "
			     "(tirados %u, resyncs %u, nacks %u)",
			     (unsigned)vjEstado(&vj)->tirados,
			     (unsigned)vjEstado(&vj)->resyncs,
			     (unsigned)vjEstado(&vj)->nacks);
		}
	}

	if ((video_paquetes % 600) == 1) {
		const decInfo *d = decStatus();

		wlog("VIDEO: %u paq, %u KB -> %u AU, %u KB | dec: %u imagenes, "
		     "%u tiradas, ritmo %u us%s",
		     (unsigned)video_paquetes, (unsigned)(video_bytes / 1024),
		     (unsigned)au_n, (unsigned)(au_bytes / 1024),
		     (unsigned)d->frames_decoded, (unsigned)decLiveDrops(),
		     (unsigned)d->pace_us_avg,
		     vjEsperandoClave(&vj) ? "  (esperando IDR)" : "");
		{
			/* EL VALOR CRUDO DE LOS GATILLOS, y no por curiosidad.
			 *
			 * En io/pad.h son "unsigned int PRE_L2 : 16", o sea campos de
			 * 16 bits. Que el DS3 meta ahi 0..255 es lo que dice todo el
			 * mundo, pero "lo que dice todo el mundo" es exactamente lo
			 * que este proyecto lleva cinco meses cobrandose.
			 *
			 * Si al apretar a fondo aqui sale ~255, eje_gatillo esta bien.
			 * Si sale ~65535, el divisor esta mal por un factor de 257 y
			 * el gatillo se ira a tope con rozarlo. Se mira, no se supone. */
			gr33nPad q;

			if (pad_mtx_ok) sysMutexLock(pad_mtx, 0);
			q = pad_ultimo;
			if (pad_mtx_ok) sysMutexUnlock(pad_mtx);

			wlog("       mando: %u informes | gatillos crudos L=%d R=%d "
			     "-> %d/%d milesimas",
			     (unsigned)pad_enviados, (int)q.lt, (int)q.rt,
			     (int)eje_gatillo(q.lt, (q.held & GR33N_BTN_L2) != 0),
			     (int)eje_gatillo(q.rt, (q.held & GR33N_BTN_R2) != 0));
		}

		{
			const audInfo *a = audStatus();

			/* Se enseñan los CUATRO numeros y no solo "va bien":
			 * "tarde" subiendo es el buffer corto, "ocultadas" subiendo
			 * es perdida de verdad, y "vacios" subiendo es que nos
			 * quedamos sin audio y sono silencio. Los tres se arreglan
			 * en sitios distintos. */
			wlog("       audio: %u rtp, %u tramas, %u ocultadas, "
			     "%u tarde, %u vacios, %u ms en cola",
			     (unsigned)a->rtp_recibidos, (unsigned)a->tramas,
			     (unsigned)a->ocultadas, (unsigned)a->rtp_tarde,
			     (unsigned)a->vacios, (unsigned)a->pcm_ms);
		}
	}
}


/* --------------------------------------------------------------------- */
/* El mando                                                              */
/* --------------------------------------------------------------------- */

/* Mapeo DS3 -> xCloud, fisico y no logico.
 *
 * El ajuste de intercambiar X y O es SOLO del menu: si el jugador lo tiene
 * puesto, en la interfaz acepta con O, pero al juego le sigue llegando
 * Cross = A. Cambiarlo aqui seria decirle a Cyberpunk que has pulsado B
 * cuando has pulsado X.
 *
 * El boton de PS no aparece: se lo queda el XMB antes de que llegue aqui.
 * Ese es el Nexus de Xbox, asi que el menu de guia de xCloud no se puede
 * abrir desde la consola. Se anota, no se disimula. */
static u16 mapear_botones(u32 held)
{
	u16 m = 0;

	if (held & GR33N_BTN_CROSS)    m |= XC_BTN_A;
	if (held & GR33N_BTN_CIRCLE)   m |= XC_BTN_B;
	if (held & GR33N_BTN_SQUARE)   m |= XC_BTN_X;
	if (held & GR33N_BTN_TRIANGLE) m |= XC_BTN_Y;

	if (held & GR33N_BTN_UP)       m |= XC_BTN_UP;
	if (held & GR33N_BTN_DOWN)     m |= XC_BTN_DOWN;
	if (held & GR33N_BTN_LEFT)     m |= XC_BTN_LEFT;
	if (held & GR33N_BTN_RIGHT)    m |= XC_BTN_RIGHT;

	if (held & GR33N_BTN_L1)       m |= XC_BTN_LB;
	if (held & GR33N_BTN_R1)       m |= XC_BTN_RB;
	if (held & GR33N_BTN_L3)       m |= XC_BTN_LS;
	if (held & GR33N_BTN_R3)       m |= XC_BTN_RS;

	if (held & GR33N_BTN_START)    m |= XC_BTN_MENU;
	if (held & GR33N_BTN_SELECT)   m |= XC_BTN_VIEW;

	return m;
}

/* Un eje de -128..127 a milesimas, con zona muerta y REESCALADO.
 *
 * Lo importante es lo segundo. Recortar a cero por debajo del umbral y
 * dejar el resto tal cual crea un salto: el personaje pasa de quieto a
 * andar a un quinto de velocidad en cuanto cruzas el umbral, y los
 * movimientos finos --apuntar-- se vuelven imposibles. Reescalando, el
 * borde de la zona muerta es cero y el tope sigue siendo el tope.
 *
 * La zona muerta es la misma que el usuario calibro en Ajustes mirando
 * los numeros en vivo: si su mando se va solo en el menu, se va solo en el
 * juego. */
static s32 eje_ps3(s32 v, int dz)
{
	s32 signo = (v < 0) ? -1 : 1;
	s32 a = (v < 0) ? -v : v;
	s32 tope = 127;

	if (dz < 0)   dz = 0;
	if (dz > 100) dz = 100;

	if (a <= dz) return 0;
	if (a > tope) a = tope;

	return signo * (((a - dz) * 1000) / (tope - dz));
}

/* LOS GATILLOS, DE 0..255 A MILESIMAS.
 *
 * Y CON RESPALDO DIGITAL, que es la parte que importa. Un DS3 mide
 * presion, pero un Sixaxis de los primeros o un mando por un adaptador
 * chungo pueden mandar 0 en la presion y el bit de pulsado a 1. Si solo
 * se mirara la presion, en esos mandos el gatillo no haria nada -- y el
 * jugador no tendria forma de saber por que.
 *
 * Asi que: si hay presion, manda la presion; si no la hay pero el boton
 * esta pulsado, se va a tope. Nunca al reves.
 *
 * Zona muerta pequena y fija, de 8 sobre 255. Los gatillos del DS3 se
 * quedan en dos o tres unidades en reposo cuando tienen anos encima, y
 * eso en un juego de coches es acelerar solo. No usa la zona muerta de
 * los sticks: son piezas distintas y se ensucian distinto. */
static s32 eje_gatillo(s32 presion, int pulsado)
{
	const s32 DZ = 8;

	if (presion > DZ) {
		s32 v = ((presion - DZ) * 1000) / (255 - DZ);
		return v > 1000 ? 1000 : v;
	}

	return pulsado ? 1000 : 0;
}

/* Lo llama el hilo de dibujo, justo despues de leer el DS3. */
void wrtcSesionPad(const gr33nPad *p)
{
	if (p == NULL) return;

	if (pad_mtx_ok) sysMutexLock(pad_mtx, 0);
	pad_ultimo = *p;
	if (pad_mtx_ok) sysMutexUnlock(pad_mtx);
}

/* Y esto lo llama el bombeo, con PC_LOCK ya cogido.
 *
 * A 60 Hz, no a la velocidad del bucle: el protocolo v1 manda el estado
 * ABSOLUTO del mando numerado por una secuencia, asi que mas informes no
 * es mas precision, es mas trafico. Y el servidor deja de aplicar entrada
 * en cuanto ve un hueco en esa numeracion, o sea que la secuencia sube de
 * uno en uno pase lo que pase. */
static void enviar_mando(void)
{
	gr33nPad p;
	xcPad x;
	u8 bin[XC_GAMEPAD_LEN];
	u64 ahora = ahora_ms();
	int dz;

	if (!arranque_hecho || ses_pc == NULL) return;

	if (pad_t0 == 0) pad_t0 = ahora;
	if (ahora - pad_ultimo_envio < 16) return;
	pad_ultimo_envio = ahora;

	if (pad_mtx_ok) sysMutexLock(pad_mtx, 0);
	p = pad_ultimo;
	if (pad_mtx_ok) sysMutexUnlock(pad_mtx);

	if (!p.connected) return;

	dz = uiDeadzone();

	memset(&x, 0, sizeof(x));
	x.indice  = 0;
	x.botones = mapear_botones(p.held);

	x.lx = eje_ps3(p.lx, dz);
	x.ly = eje_ps3(p.ly, dz);
	x.rx = eje_ps3(p.rx, dz);
	x.ry = eje_ps3(p.ry, dz);

	x.lt = eje_gatillo(p.lt, (p.held & GR33N_BTN_L2) != 0);
	x.rt = eje_gatillo(p.rt, (p.held & GR33N_BTN_R2) != 0);

	if (xcInputGamepad(bin, sizeof(bin), ++input_sec,
	                   (double)(ahora - pad_t0), &x) == XC_GAMEPAD_LEN) {
		peer_connection_datachannel_send_sid(ses_pc, (char *)bin,
		                                     XC_GAMEPAD_LEN, SID_INPUT);
		pad_enviados++;
	}
}

/* --------------------------------------------------------------------- */

static void wrtc_thread(void *arg)
{
	(void)arg;

	while (wrtc_running) {
		if (probe_req) {
			probe_req = 0;
			correr_prueba();
			continue;
		}

		/* EL BOMBEO DE LA SESION VA AQUI, en el hilo que ya existe.
		 *
		 * peer_connection_loop() es lo que manda las comprobaciones de
		 * conectividad, adelanta el saludo DTLS y saca el video. Tiene
		 * que girar sin parar: green-nx avisa de que cualquier cosa que
		 * lo detenga --por ejemplo un keepalive HTTPS bloqueante-- deja
		 * que se desborde la cola de recepcion UDP, y eso se ve como un
		 * tiron de video y un PLI cada quince segundos clavados.
		 *
		 * Por eso el keepalive de xCloud lo manda session.c desde SU
		 * hilo, y aqui no se hace nada que pueda bloquear. */
		if (bombear && ses_pc) {
			PC_LOCK();
			if (ses_pc) {
				peer_connection_loop(ses_pc);

				/* Y en cuanto SCTP este arriba, abrir los canales y
				 * saludar. Va AQUI, dentro del candado del bombeo, y
				 * no en el callback: aquel se ejecuta desde dentro de
				 * peer_connection_loop() y ponerse a mandar cosas
				 * desde ahi es reentrar en libpeer. */
				if (sctp_arriba && !canales_abiertos) {
					canales_abiertos = 1;
					abrir_canales();
				} else if (canales_abiertos && !saludo_enviado) {
					saludo_enviado = 1;
					enviar_texto(SID_MESSAGE, xcHandshake());
					wlog("saludo enviado por 'message'");
				}

				enviar_mando();

				/* EL DESCODIFICADOR SE HA REABIERTO: hace falta un IDR.
				 *
				 * Esto va aqui y no en decoder.c porque el PLI sale por
				 * la conexion, y de la conexion manda este hilo. El
				 * descodificador solo levanta la mano.
				 *
				 * Se piden las dos vias, igual que en el arranque: el
				 * PLI de RTCP, que es al que xCloud hace caso, y el
				 * mensaje de aplicacion. */
				if (decQuiereClave()) {
					peer_connection_request_keyframe(ses_pc);
					enviar_texto(SID_CONTROL, xcKeyframeRequested());
					wlog("el descodificador se ha reabierto: "
					     "pedido fotograma clave");
				}
			}
			PC_UNLOCK();
			usleep(1000);
			continue;
		}

		usleep(100000);
	}

	sysThreadExit(0);
}

int wrtcInit(void)
{
	memset(&info, 0, sizeof(info));

	tb_hz = sysGetTimebaseFrequency();
	if (tb_hz == 0) tb_hz = 79800000ull;

	{
		sys_mutex_attr_t attr;
		sysMutexAttrInitialize(attr);
		pc_mtx_ok = (sysMutexCreate(&pc_mtx, &attr) == 0);
		if (!pc_mtx_ok)
			printf("[wrtc] sysMutexCreate fallo: sin candado\n");

		sysMutexAttrInitialize(attr);
		pad_mtx_ok = (sysMutexCreate(&pad_mtx, &attr) == 0);
	}

	wrtc_running = 1;
	if (sysThreadCreate(&wrtc_tid, wrtc_thread, NULL, WRTC_THREAD_PRIO,
	                    WRTC_THREAD_STACK, THREAD_JOINABLE,
	                    "GR33N webrtc") != 0) {
		wrtc_running = 0;
		wfail("sysThreadCreate del hilo de WebRTC fallo");
		return -1;
	}

	wrtc_started = 1;
	return 0;
}

void wrtcShutdown(void)
{
	if (!wrtc_started) return;

	wrtcSesionSoltar();

	wrtc_running = 0;
	sysThreadJoin(wrtc_tid, NULL);
	wrtc_started = 0;

	if (pc_mtx_ok) {
		sysMutexDestroy(pc_mtx);
		pc_mtx_ok = 0;
	}
	if (pad_mtx_ok) {
		sysMutexDestroy(pad_mtx);
		pad_mtx_ok = 0;
	}
}

void wrtcProbe(void)
{
	if (!wrtc_started) return;
	if (info.state != WRTC_IDLE && info.state != WRTC_LISTO &&
	    info.state != WRTC_FALLO)
		return;   /* ya hay una en marcha */

	probe_req = 1;
}

const wrtcInfo *wrtcStatus(void)
{
	return &info;
}

/* --------------------------------------------------------------------- */
/* LA SESION DE VERDAD                                                   */
/* --------------------------------------------------------------------- */

/* Lo de arriba es la prueba: crea una conexion, mira la oferta y recoge.
 * Esto es lo otro: una conexion que se queda viva mientras session.c
 * negocia con xCloud por HTTPS y luego se bombea hasta que ICE conecta.
 *
 * POR QUE UN CANDADO ALREDEDOR DE TODO.
 *
 * libpeer no es reentrante y aqui hay dos hilos de verdad: el de sesion
 * (session.c) llamando a crear/anadir/contestar, y este, que da vueltas a
 * peer_connection_loop(). green-nx usa un mutex por lo mismo y lo dice en
 * un comentario. Un PeerConnection tocado por dos sitios a la vez no da
 * error: da corrupcion, y del tipo que aparece tres capas mas alla.
 *
 * El orden que impone esta API tampoco es decorativo, y esta copiado de
 * green-nx porque alli costo descubrirlo:
 *
 *   1. wrtcSesionCrear()      crea la conexion
 *   2. wrtcSesionOferta()     saca el SDP que hay que mandar
 *   3. wrtcSesionCandidato()  TODOS los candidatos remotos, uno a uno
 *   4. wrtcSesionRespuesta()  y SOLO ENTONCES la respuesta
 *   5. wrtcSesionBombear()    a partir de aqui gira el bucle
 *
 * El 3 va antes que el 4 porque libpeer arma los pares de candidatos UNA
 * sola vez, dentro de set_remote_description. Un candidato anadido
 * despues no entra en ningun par y no se usa jamas -- sin un solo aviso.
 */


static void on_ice_estado(PeerConnectionState estado, void *ud)
{
	(void)ud;
	ice_estado = (int)estado;
	wlog("ICE -> %s", peer_connection_state_to_string(estado));
}


int wrtcSesionCrear(void)
{
	PeerConfiguration cfg;

	if (!wrtc_started) return -1;

	wrtcSesionSoltar();

	if (peer_arranca() != 0) {
		wfail("peer_init fallo");
		return -1;
	}

	memset(&cfg, 0, sizeof(cfg));
	cfg.video_codec = CODEC_H264;
	cfg.datachannel = DATA_CHANNEL_BINARY;

	/* EL CALLBACK DE VIDEO, que en la primera version se me quedo fuera.
	 * Sin el, el RTP que entre se descarta dentro de libpeer y no hay
	 * forma de distinguirlo de que no llegue nada. */
	cfg.onvideotrack = on_video;

	/* Y EL AUDIO. ESTAS DOS LINEAS ES LO QUE FALTABA, Y NO ERA "conectar
	 * el descodificador": era PEDIRLO.
	 *
	 * El parche de libpeer lleva desde el primer dia arreglando la
	 * plantilla SDP del audio -- SAVPF en vez de SAVP, el fmtp estereo, y
	 * hasta un apaño para cuando xCloud no manda a=ssrc. Con todo eso
	 * escrito, el m=audio NO SALIA EN LA OFERTA, porque libpeer lo mete
	 * dentro de un switch(config.audio_codec) y aqui cfg iba a memset(0),
	 * que es CODEC_NONE.
	 *
	 * O sea que xCloud nunca nos ha mandado un solo byte de audio: no se
	 * lo habiamos pedido. La plantilla arreglada era una carta escrita y
	 * sin echar al buzon. */
	cfg.audio_codec  = CODEC_OPUS;
	cfg.onaudiotrack = on_audio;
	/* Los servidores STUN de verdad los pone session.c con
	 * wrtcSesionStun() antes de crear, si /configuration los trae. */
	if (stun_url[0]) cfg.ice_servers[0].urls = stun_url;

	sctp_arriba = 0;
	canales_abiertos = 0;
	saludo_enviado = 0;
	arranque_hecho = 0;
	video_bytes = 0;
	video_paquetes = 0;
	input_sec = 0;
	au_n = 0;
	au_bytes = 0;
	vjReset(&vj);
	pad_t0 = 0;
	pad_ultimo_envio = 0;
	pad_enviados = 0;
	memset(&pad_ultimo, 0, sizeof(pad_ultimo));

	PC_LOCK();
	ses_pc = peer_connection_create(&cfg);
	if (ses_pc) {
		peer_connection_oniceconnectionstatechange(ses_pc, on_ice_estado);
		/* Los canales NO se crean aqui: DATA_CHANNEL_OPEN no se puede
		 * mandar hasta que la asociacion SCTP este arriba, y eso lo
		 * avisa on_canal_abierto. */
		peer_connection_ondatachannel(ses_pc, on_canal_mensaje,
		                              on_canal_abierto, on_canal_cerrado);
	}
	PC_UNLOCK();

	if (ses_pc == NULL) {
		wfail("peer_connection_create devolvio NULL");
		return -1;
	}

	ice_estado = -1;
	wlog("sesion: conexion creada");
	return 0;
}

const char *wrtcSesionOferta(void)
{
	const char *sdp;

	if (ses_pc == NULL) return NULL;

	PC_LOCK();
	sdp = peer_connection_create_offer(ses_pc);
	PC_UNLOCK();

	if (sdp == NULL) {
		wfail("create_offer devolvio NULL");
		return NULL;
	}

	wlog("sesion: oferta de %u bytes", (unsigned)strlen(sdp));
	return sdp;
}

int wrtcSesionCandidato(const char *cand)
{
	if (ses_pc == NULL || cand == NULL) return -1;

	PC_LOCK();
	/* libpeer se guarda una copia; el char* no es const en su firma pero
	 * no lo modifica (agent.c: ice_candidate_parse copia). */
	peer_connection_add_ice_candidate(ses_pc, (char *)cand);
	PC_UNLOCK();

	return 0;
}

int wrtcSesionRespuesta(const char *answer)
{
	if (ses_pc == NULL || answer == NULL) return -1;

	/* VERBATIM. Ni una reescritura, ni normalizar finales de linea, ni
	 * recortar. green-nx lo avisa por escrito: si se reserializa, un
	 * '\r' de mas se cuela en el ice-ufrag y las comprobaciones STUN se
	 * firman con la clave equivocada. El servidor las tira en silencio y
	 * la conexion no llega nunca -- sin un solo mensaje de error. */
	PC_LOCK();
	peer_connection_set_remote_description(ses_pc, answer,
	                                       SDP_TYPE_ANSWER);
	PC_UNLOCK();

	wlog("sesion: respuesta puesta (%u bytes), armando pares",
	     (unsigned)strlen(answer));
	return 0;
}

void wrtcSesionBombear(int si)
{
	bombear = si ? 1 : 0;
	wlog("sesion: bombeo %s", si ? "en marcha" : "parado");
}

int wrtcSesionIce(void)
{
	return ice_estado;
}

void wrtcSesionSoltar(void)
{
	PeerConnection *p;

	bombear = 0;

	PC_LOCK();
	p = ses_pc;
	ses_pc = NULL;
	PC_UNLOCK();

	if (p) {
		peer_connection_close(p);
		peer_connection_destroy(p);
		/* SIN peer_deinit(). Ver el comentario largo de arriba: esta
		 * linea es la que mataba la segunda partida. */
		wlog("sesion: soltada");
	}
}

void wrtcSesionStun(const char *url)
{
	if (url == NULL || url[0] == '\0') {
		stun_url[0] = '\0';
		return;
	}

	/* SE COPIA, no se guarda el puntero. PeerConfiguration se queda con
	 * la direccion que le des y libpeer la lee al recolectar candidatos,
	 * que es despues de que quien llamo haya vuelto. Guardar el puntero
	 * de una variable local seria leer pila muerta. */
	snprintf(stun_url, sizeof(stun_url), "%s", url);
	wlog("sesion: servidor STUN %s", stun_url);
}

const char *wrtcIceTexto(int estado)
{
	if (estado < 0) return "sin empezar";
	return peer_connection_state_to_string((PeerConnectionState)estado);
}
