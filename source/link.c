/* GR33N - enlace UDP con el servidor */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ppu-types.h>
#include <sys/systime.h>
#include <sys/thread.h>
#include <sys/mutex.h>

#include <net/net.h>
#include <net/netctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "gr33n.h"
#include "link.h"

#define RTT_WINDOW        128
#define STAT_WINDOW_US    2000000ull   /* 2 s: perdida y pps */
#define HELLO_PERIOD_US    500000ull   /* medio segundo mientras busca */
#define LINK_TIMEOUT_US   3000000ull   /* sin pong 3 s -> se cae el enlace */
#define RX_POLL_MS             100     /* para que el hilo note el apagado */
#define RX_STACK_SIZE       (64*1024)
#define RX_PRIORITY           1000     /* por encima del hilo principal (1001) */
#define PKT_MAX               2048

static int  sock = -1;
static int  net_up = 0;

/* --- estado compartido, protegido por mtx ---------------------------- */
static sys_mutex_t mtx;
static int         mtx_ok = 0;

static linkInfo status;
static struct sockaddr_in server_addr;
static int server_known = 0;

/* Cola de arranque.
 *
 * linkLog no puede enviar nada hasta que el servidor se ha anunciado, y
 * eso tarda cerca de un segundo. Todo lo que pase antes — buscar donde
 * guardar, cargar ajustes, inicializar el decodificador — se perdia en
 * silencio. Justo la parte del arranque, que es cuando mas cosas pueden
 * salir mal.
 *
 * Ahora se guarda y se suelta en cuanto aparece el servidor. Si se
 * desborda se cuenta cuantas se cayeron y se dice: una cola que miente
 * sobre lo que ha tirado es peor que no tener cola. */
#define PEND_LINES  40
#define PEND_LEN    200

static char pend[PEND_LINES][PEND_LEN];
static u32  pend_n = 0;
static u32  pend_lost = 0;
static u64 last_pong_us = 0;

static u32 rtt_ring[RTT_WINDOW];
static u32 rtt_count = 0;
static u32 rtt_head  = 0;

static u64 stat_t0     = 0;
static u32 stat_sent   = 0;
static u32 stat_recv   = 0;
static u32 stat_frames = 0;

static linkFrame latest_frame;
static int frame_fresh     = 0;
static u32 last_frame_seq  = 0;
static int have_frame_seq  = 0;
/* --------------------------------------------------------------------- */

static u32 next_seq = 1;
static u32 next_input_seq = 1;
static u64 last_hello_us = 0;
static linkState prev_state = LINK_DOWN;

static sys_ppu_thread_t rx_tid;
static volatile int rx_running = 0;
static int rx_started = 0;

#define LOCK()    do { if (mtx_ok) sysMutexLock(mtx, 0); } while (0)
#define UNLOCK()  do { if (mtx_ok) sysMutexUnlock(mtx); } while (0)

/* --------------------------------------------------------------------- */
/* Tiempo                                                                */
/* --------------------------------------------------------------------- */

static u64 now_us(void)
{
	u64 sec = 0, nsec = 0;
	sysGetCurrentTime(&sec, &nsec);
	return sec * 1000000ull + nsec / 1000ull;
}

/* --------------------------------------------------------------------- */
/* Formato de cable, byte a byte                                         */
/* --------------------------------------------------------------------- */

static void put_u32(u8 *p, u32 v)
{
	p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16);
	p[2] = (u8)(v >> 8);  p[3] = (u8)v;
}

static void put_u64(u8 *p, u64 v)
{
	put_u32(p,     (u32)(v >> 32));
	put_u32(p + 4, (u32)v);
}

static u32 get_u32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) |
	       ((u32)p[2] << 8)  |  (u32)p[3];
}

static u64 get_u64(const u8 *p)
{
	return ((u64)get_u32(p) << 32) | (u64)get_u32(p + 4);
}

/* --------------------------------------------------------------------- */
/* Estadisticas (siempre con el mutex cogido)                            */
/* --------------------------------------------------------------------- */

static void rtt_push_locked(u32 rtt)
{
	u32 i, n, prev;
	u64 sum = 0, jsum = 0;

	rtt_ring[rtt_head] = rtt;
	rtt_head = (rtt_head + 1) % RTT_WINDOW;
	if (rtt_count < RTT_WINDOW) rtt_count++;

	status.rtt_last = rtt;
	status.rtt_min  = 0xffffffffu;
	status.rtt_max  = 0;

	n = rtt_count;
	for (i = 0; i < n; i++) {
		u32 idx = (rtt_head + RTT_WINDOW - n + i) % RTT_WINDOW;
		u32 v = rtt_ring[idx];
		if (v < status.rtt_min) status.rtt_min = v;
		if (v > status.rtt_max) status.rtt_max = v;
		sum += v;
		if (i > 0) {
			prev = rtt_ring[(idx + RTT_WINDOW - 1) % RTT_WINDOW];
			jsum += (v > prev) ? (v - prev) : (prev - v);
		}
	}

	status.rtt_avg = (u32)(sum / n);
	status.jitter  = (n > 1) ? (u32)(jsum / (n - 1)) : 0;
}

static void stats_reset_locked(void)
{
	rtt_count = 0;
	rtt_head  = 0;
	status.rtt_last = status.rtt_min = status.rtt_max = 0;
	status.rtt_avg = status.jitter = 0;
	status.loss_pct = 0;
	status.pps = 0;
	status.fps_rx = 0;
	stat_sent = stat_recv = stat_frames = 0;
	stat_t0 = now_us();
	have_frame_seq = 0;
}

/* --------------------------------------------------------------------- */
/* Envio                                                                 */
/* --------------------------------------------------------------------- */

static void send_to(const struct sockaddr_in *dst, const void *buf, u32 len)
{
	if (sock < 0) return;
	netSendTo(sock, buf, len, 0, (const struct sockaddr*)dst, sizeof(*dst));
}

static void send_hello(void)
{
	u8 pkt[12];
	struct sockaddr_in bcast;

	put_u32(pkt,     LINK_MAGIC);
	put_u32(pkt + 4, LINK_MSG_HELLO);
	put_u32(pkt + 8, LINK_VERSION);

	memset(&bcast, 0, sizeof(bcast));
	bcast.sin_len         = sizeof(bcast);
	bcast.sin_family      = AF_INET;
	bcast.sin_port        = htons(LINK_SERVER_PORT);
	bcast.sin_addr.s_addr = htonl(INADDR_BROADCAST);

	send_to(&bcast, pkt, sizeof(pkt));
}

static void send_ping(void)
{
	u8 pkt[20];
	struct sockaddr_in dst;
	u32 seq;
	u64 t;

	LOCK();
	if (!server_known) { UNLOCK(); return; }
	dst = server_addr;
	seq = next_seq++;
	status.pings_sent++;
	stat_sent++;
	UNLOCK();

	/* La marca de tiempo se toma lo mas pegada posible al envio. */
	t = now_us();

	put_u32(pkt,      LINK_MAGIC);
	put_u32(pkt + 4,  LINK_MSG_PING);
	put_u32(pkt + 8,  seq);
	put_u64(pkt + 12, t);

	send_to(&dst, pkt, sizeof(pkt));
}

void linkSendInput(u32 buttons, s32 lx, s32 ly, s32 rx, s32 ry)
{
	u8 pkt[40];
	struct sockaddr_in dst;
	u32 seq;
	u64 t;

	if (sock < 0) return;

	LOCK();
	if (!server_known) { UNLOCK(); return; }
	dst = server_addr;
	seq = next_input_seq++;
	UNLOCK();

	t = now_us();

	put_u32(pkt,      LINK_MAGIC);
	put_u32(pkt + 4,  LINK_MSG_INPUT);
	put_u32(pkt + 8,  seq);
	put_u64(pkt + 12, t);
	put_u32(pkt + 20, buttons);
	put_u32(pkt + 24, (u32)lx);
	put_u32(pkt + 28, (u32)ly);
	put_u32(pkt + 32, (u32)rx);
	put_u32(pkt + 36, (u32)ry);

	send_to(&dst, pkt, sizeof(pkt));
}

int linkTakeFrame(linkFrame *out)
{
	int fresh;

	if (!out) return 0;

	LOCK();
	fresh = frame_fresh;
	if (fresh) {
		u32 last_sent = next_input_seq - 1;

		*out = latest_frame;
		frame_fresh = 0;

		/* Cuantos inputs de antiguedad trae. 0 = responde al ultimo que
		 * mandamos; 1 = vamos un frame por detras. */
		status.frame_lag = (last_sent > latest_frame.input_seq)
		                 ? (last_sent - latest_frame.input_seq) : 0;
	}
	UNLOCK();

	return fresh;
}

int linkWaitFrame(linkFrame *out, u32 timeout_us)
{
	u64 t0 = now_us();

	if (!out) return 0;

	for (;;) {
		u64 waited;

		if (linkTakeFrame(out)) {
			waited = now_us() - t0;
			LOCK();
			status.wait_us = (u32)waited;
			UNLOCK();
			return 1;
		}

		if (now_us() - t0 >= (u64)timeout_us) break;

		/* usleep corto: la granularidad real de lv2 la vera el HUD en
		 * wait_us. Si resulta ser gruesa, el siguiente paso es una
		 * variable de condicion en vez de sondear. */
		usleep(50);
	}

	LOCK();
	status.wait_us = timeout_us;
	UNLOCK();

	return 0;
}

static void send_log_line(const struct sockaddr_in *dst, const char *text)
{
	u8 pkt[PKT_MAX];
	u32 n = (u32)strlen(text);

	if (!n) return;
	if (n > PKT_MAX - 8 - 1) n = PKT_MAX - 8 - 1;

	put_u32(pkt,     LINK_MAGIC);
	put_u32(pkt + 4, LINK_MSG_LOG);
	memcpy(pkt + 8, text, n);

	send_to(dst, pkt, 8 + n);
}

/* Suelta la cola de arranque. La llama linkUpdate desde el hilo
 * principal: hacerlo dentro del hilo de recepcion, con el mutex cogido,
 * seria pedir un interbloqueo. */
static void flush_pending(void)
{
	struct sockaddr_in dst;
	char line[PEND_LEN];
	u32 i, n, lost;

	LOCK();
	if (!server_known || !pend_n) { UNLOCK(); return; }
	dst  = server_addr;
	n    = pend_n;
	lost = pend_lost;
	UNLOCK();

	for (i = 0; i < n; i++) {
		LOCK();
		memcpy(line, pend[i], PEND_LEN);
		UNLOCK();
		send_log_line(&dst, line);
	}

	if (lost) {
		snprintf(line, sizeof(line),
		         "!! se perdieron %u lineas de arranque (cola de %d llena)",
		         (unsigned)lost, PEND_LINES);
		send_log_line(&dst, line);
	}

	LOCK();
	pend_n = 0;
	pend_lost = 0;
	UNLOCK();
}

/* --------------------------------------------------------------------- */
/* Tapar lo que no debe salir en un log                                  */
/* --------------------------------------------------------------------- */

/* LOS LOGS DE ESTE PROYECTO SE COMPARTEN, y llevaban credenciales dentro.
 *
 * El metodo que ha resuelto el catalogo, la tienda y la sesion es volcar la
 * respuesta entera sin interpretarla. Funciona, y tiene un efecto lateral
 * que tarde tres versiones en ver: el volcado de POST .../connect llevaba
 * el token MSA de traspaso, y el de /configuration la clave SRTP de la
 * sesion. Los dos caducan pronto -el segundo en cinco minutos- pero un
 * fichero que se manda por ahi no es sitio para ninguno de los dos.
 *
 * Esto copia el texto sustituyendo el valor de las claves que se le pasen
 * por su longitud, que es lo unico que hacia falta saber de ellos:
 *
 *   {"userToken":"M.C517_BL2.0.U..."}  ->  {"userToken":"<564 bytes>"}
 *
 * Deliberadamente tonto: busca la clave literal, se salta las comillas y
 * los dos puntos, y tapa hasta la siguiente comilla sin escapar. No es un
 * analizador de JSON y no pretende serlo; si el formato cambia, lo peor que
 * pasa es que no tape, y eso es visible en el log. Un analizador de verdad
 * aqui seria mas codigo del que tapa.
 *
 * Devuelve `out`, siempre terminado en nulo. */
const char *linkRedact(char *out, u32 max, const char *src,
                       const char *const *claves, u32 n_claves)
{
	u32 o = 0;
	const char *p = src;

	if (!out || max < 2) return "";
	if (!src) { out[0] = '\0'; return out; }

	while (*p && o + 1 < max) {
		u32 k;
		int tapado = 0;

		for (k = 0; k < n_claves; k++) {
			u32 kl = (u32)strlen(claves[k]);
			const char *q;
			u32 vlen = 0;
			int esc;

			/* Las claves se pasan CON la comilla de delante
			 * ("\"userToken"), asi que "token" no casa dentro de otra
			 * palabra. */
			if (strncmp(p, claves[k], kl) != 0) continue;

			/* Detras: comilla de cierre de la clave, dos puntos, y la
			 * comilla de apertura del valor. Si no viene eso, no es esto. */
			q = p + kl;
			if (*q++ != '"') continue;
			while (*q == ' ') q++;
			if (*q++ != ':') continue;
			while (*q == ' ') q++;
			if (*q++ != '"') continue;

			while (q[vlen] && q[vlen] != '"') {
				if (q[vlen] == '\\' && q[vlen + 1]) vlen++;
				vlen++;
			}

			/* Sin valor cerrado no se tapa nada: es texto cortado, y
			 * inventarse un final aqui es peor que dejarlo como esta. */
			if (q[vlen] != '"') continue;

			esc = snprintf(out + o, max - o, "%s\":\"<%u bytes>\"",
			               claves[k], (unsigned)vlen);
			if (esc < 0 || (u32)esc >= max - o) { o = max - 1; tapado = 1; break; }

			o += (u32)esc;
			p  = q + vlen + 1;     /* pasada la comilla de cierre */
			tapado = 1;
			break;
		}

		if (tapado) continue;

		out[o++] = *p++;
	}

	out[o < max ? o : max - 1] = '\0';
	return out;
}

void linkLog(const char *fmt, ...)
{
	char text[PEND_LEN];
	struct sockaddr_in dst;
	va_list ap;
	int n;

	if (sock < 0) return;

	va_start(ap, fmt);
	n = vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);

	if (n <= 0) return;

	LOCK();

	if (!server_known) {
		/* Todavia no hay a quien contarselo: a la cola. */
		if (pend_n < PEND_LINES) {
			snprintf(pend[pend_n], PEND_LEN, "%s", text);
			pend_n++;
		} else {
			pend_lost++;
		}
		UNLOCK();
		return;
	}

	dst = server_addr;
	UNLOCK();

	send_log_line(&dst, text);
}

/* --------------------------------------------------------------------- */
/* Recepcion: hilo propio                                                */
/* --------------------------------------------------------------------- */

/* Con el mutex cogido. No llamar a linkLog desde aqui: el mutex no es
 * recursivo y linkLog tambien lo coge. El log de transicion lo emite
 * linkUpdate desde el hilo principal. */
static void adopt_server_locked(const struct sockaddr_in *from,
                                const u8 *pkt, int len, u64 t_rx)
{
	server_addr            = *from;
	server_addr.sin_len    = sizeof(server_addr);
	server_addr.sin_family = AF_INET;
	server_addr.sin_port   = htons(LINK_SERVER_PORT);
	server_known = 1;

	inet_ntop(AF_INET, &server_addr.sin_addr,
	          status.server_ip, sizeof(status.server_ip));

	if (len >= 12 + LINK_NAME_LEN) {
		memcpy(status.server_name, pkt + 12, LINK_NAME_LEN - 1);
		status.server_name[LINK_NAME_LEN - 1] = '\0';
	}

	if (status.state != LINK_UP) {
		status.state = LINK_UP;
		last_pong_us = t_rx;
		stats_reset_locked();
	}
}

static void handle_packet(const struct sockaddr_in *from,
                          const u8 *pkt, int len, u64 t_rx)
{
	u32 type;

	if (len < 8) return;
	if (get_u32(pkt) != LINK_MAGIC) return;

	type = get_u32(pkt + 4);

	LOCK();

	switch (type) {
	case LINK_MSG_HERE:
		adopt_server_locked(from, pkt, len, t_rx);
		break;

	case LINK_MSG_PONG: {
		u64 t_client, rtt;
		if (len < 28) break;

		t_client = get_u64(pkt + 12);
		if (t_rx <= t_client) break;          /* reloj hacia atras */
		rtt = t_rx - t_client;
		if (rtt > 5000000ull) break;          /* absurdo, lo tiramos */

		rtt_push_locked((u32)rtt);
		status.pongs_recv++;
		stat_recv++;
		last_pong_us = t_rx;

		if (status.state != LINK_UP)
			adopt_server_locked(from, pkt, 0, t_rx);
		break;
	}

	case LINK_MSG_FRAME: {
		u32 fseq, cols, rows, need;

		if (len < 32) break;

		fseq = get_u32(pkt + 8);
		cols = get_u32(pkt + 24);
		rows = get_u32(pkt + 28);

		if (cols == 0 || rows == 0) break;
		if (cols > LINK_FRAME_COLS || rows > LINK_FRAME_ROWS) break;

		need = cols * rows * 3;
		if ((u32)len < 32 + need) break;

		/* Un frame mas viejo que el que ya tenemos se tira: en UDP los
		 * datagramas pueden adelantarse entre si y pintar hacia atras es
		 * peor que perder el frame. */
		if (have_frame_seq && (s32)(fseq - last_frame_seq) <= 0) break;

		if (have_frame_seq && fseq > last_frame_seq + 1)
			status.frame_gaps += fseq - last_frame_seq - 1;

		last_frame_seq = fseq;
		have_frame_seq = 1;

		latest_frame.seq        = fseq;
		latest_frame.input_seq  = get_u32(pkt + 12);
		latest_frame.cols       = cols;
		latest_frame.rows       = rows;
		latest_frame.t_input_us = get_u64(pkt + 16);
		memcpy(latest_frame.rgb, pkt + 32, need);

		frame_fresh = 1;
		status.frames_recv++;
		stat_frames++;
		break;
	}

	default:
		break;
	}

	UNLOCK();
}

static void rx_thread(void *arg)
{
	u8 pkt[PKT_MAX];
	struct sockaddr_in from;
	socklen_t fromlen;
	struct pollfd pfd;

	(void)arg;

	while (rx_running) {
		ssize_t n;
		u64 t_rx;

		pfd.fd      = sock;
		pfd.events  = POLLIN;
		pfd.revents = 0;

		if (netPoll(&pfd, 1, RX_POLL_MS) <= 0) continue;
		if (!(pfd.revents & POLLIN)) continue;

		fromlen = sizeof(from);
		memset(&from, 0, sizeof(from));

		n = netRecvFrom(sock, pkt, sizeof(pkt), 0,
		                (struct sockaddr*)&from, &fromlen);

		/* AQUI. No despues de procesar, no en el frame siguiente: la hora
		 * de llegada es esta. Todo lo demas contamina la medida. */
		t_rx = now_us();

		if (n <= 0) continue;

		handle_packet(&from, pkt, (int)n, t_rx);
	}

	sysThreadExit(0);
}

/* --------------------------------------------------------------------- */
/* API publica                                                           */
/* --------------------------------------------------------------------- */

int linkInit(void)
{
	struct sockaddr_in local;
	union net_ctl_info info;
	sys_mutex_attr_t attr;
	int on = 1;
	s32 ret;

	memset(&status, 0, sizeof(status));
	status.state = LINK_DOWN;
	strcpy(status.local_ip, "?");
	strcpy(status.server_ip, "-");
	prev_state = LINK_DOWN;

	sysMutexAttrInitialize(attr);
	if (sysMutexCreate(&mtx, &attr) == 0)
		mtx_ok = 1;
	else
		printf("[link] sysMutexCreate fallo\n");

	ret = netInitialize();
	if (ret < 0) {
		printf("[link] netInitialize fallo: 0x%08x\n", (unsigned)ret);
		return -1;
	}
	net_up = 1;

	/* La IP local es informativa: si netCtl no coopera, seguimos igual. */
	if (netCtlInit() == 0) {
		memset(&info, 0, sizeof(info));
		if (netCtlGetInfo(NET_CTL_INFO_IP_ADDRESS, &info) == 0) {
			strncpy(status.local_ip, info.ip_address, sizeof(status.local_ip) - 1);
			status.local_ip[sizeof(status.local_ip) - 1] = '\0';
		}
	}

	sock = netSocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock < 0) {
		printf("[link] netSocket fallo: %d\n", (int)sock);
		return -1;
	}

	netSetSockOpt(sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
	netSetSockOpt(sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

	memset(&local, 0, sizeof(local));
	local.sin_len         = sizeof(local);
	local.sin_family      = AF_INET;
	local.sin_port        = htons(LINK_CLIENT_PORT);
	local.sin_addr.s_addr = htonl(INADDR_ANY);

	if (netBind(sock, (struct sockaddr*)&local, sizeof(local)) < 0) {
		printf("[link] netBind al puerto %d fallo\n", LINK_CLIENT_PORT);
		netClose(sock);
		sock = -1;
		return -1;
	}

	status.state = LINK_SEARCHING;
	stat_t0 = now_us();

	rx_running = 1;
	if (sysThreadCreate(&rx_tid, rx_thread, NULL, RX_PRIORITY,
	                    RX_STACK_SIZE, THREAD_JOINABLE, "GR33N link rx") == 0) {
		rx_started = 1;
	} else {
		rx_running = 0;
		printf("[link] sysThreadCreate fallo: sin hilo de recepcion\n");
		return -1;
	}

	printf("[link] listo. IP local %s, escuchando en %d\n",
	       status.local_ip, LINK_CLIENT_PORT);
	return 0;
}

void linkShutdown(void)
{
	/* Orden: parar el hilo, esperarlo, y SOLO despues cerrar el socket.
	 * Al reves el hilo se quedaria haciendo poll sobre un fd cerrado. */
	if (rx_started) {
		u64 rv = 0;
		rx_running = 0;
		sysThreadJoin(rx_tid, &rv);
		rx_started = 0;
	}

	if (sock >= 0) {
		netClose(sock);
		sock = -1;
	}

	if (net_up) {
		netCtlTerm();
		netDeinitialize();
		net_up = 0;
	}

	if (mtx_ok) {
		sysMutexDestroy(mtx);
		mtx_ok = 0;
	}

	status.state = LINK_DOWN;
}

void linkUpdate(void)
{
	linkState st;
	u64 t, since_pong;
	int known;

	if (sock < 0) return;

	/* Lo primero: si hay cola de arranque y ya hay servidor, soltarla.
	 * Antes de cualquier otra cosa, porque describe lo que paso ANTES. */
	flush_pending();

	t = now_us();

	LOCK();
	st         = status.state;
	known      = server_known;
	since_pong = (t > last_pong_us) ? (t - last_pong_us) : 0;

	if (st == LINK_UP && since_pong > LINK_TIMEOUT_US) {
		/* El servidor se ha ido. Volvemos a buscar, pero sin olvidar su
		 * direccion: casi siempre vuelve el mismo. */
		status.state = LINK_SEARCHING;
		stats_reset_locked();
		st = LINK_SEARCHING;
	}

	/* Ventana de perdida y de tasa. El error de borde por pings en vuelo
	 * es despreciable a 60 Hz con RTT de microsegundos. */
	if (t - stat_t0 >= STAT_WINDOW_US) {
		u64 secs = (t - stat_t0) / 1000000ull;
		if (stat_sent > 0) {
			u32 lost = (stat_sent > stat_recv) ? (stat_sent - stat_recv) : 0;
			status.loss_pct = (lost * 100u) / stat_sent;
		} else {
			status.loss_pct = 0;
		}
		status.pps    = (secs > 0) ? (u32)(stat_recv / secs) : 0;
		status.fps_rx = (secs > 0) ? (u32)(stat_frames / secs) : 0;
		stat_sent = stat_recv = stat_frames = 0;
		stat_t0 = t;
	}
	UNLOCK();

	if (st == LINK_UP) {
		send_ping();
	} else if (t - last_hello_us > HELLO_PERIOD_US) {
		last_hello_us = t;
		send_hello();
		/* Si ya conocemos un servidor de antes, le damos un toque
		 * directo ademas del broadcast. */
		if (known) send_ping();
	}

	/* Log de transicion. linkLog coge el mutex por su cuenta, asi que la
	 * copia del IP se hace antes y se suelta. */
	if (st != prev_state) {
		if (st == LINK_UP) {
			char ip[16];
			LOCK();
			memcpy(ip, status.server_ip, sizeof(ip));
			UNLOCK();
			linkLog("GR33N %s enlazado con %s", GR33N_VERSION, ip);
		} else if (prev_state == LINK_UP) {
			printf("[link] enlace caido\n");
		}
		prev_state = st;
	}
}

const linkInfo *linkStatus(void)
{
	static linkInfo snap;

	LOCK();
	snap = status;
	UNLOCK();

	return &snap;
}
