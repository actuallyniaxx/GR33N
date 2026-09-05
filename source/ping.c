/* GR33N - medida de latencia por region. Ver ping.h para QUE se mide de
 * verdad y, sobre todo, que no. */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include <ppu-types.h>
#include <ppu-asm.h>
#include <sys/systime.h>
#include <sys/thread.h>
#include <sys/mutex.h>
#include <netinet/in.h>
#include <net/net.h>

#include "gr33n.h"
#include "link.h"
#include "auth.h"
#include "ping.h"

#define PING_PRIO    1010          /* por debajo del bombeo de WebRTC */
#define PING_STACK   (32 * 1024)

/* NO BLOQUEANTE, SI SE PUEDE.
 *
 * Un connect() bloqueante contra una region caida se queda esperando lo
 * que diga lv2, que puede ser un minuto largo, y durante ese minuto no se
 * mide ninguna de las otras. Con el socket en modo no bloqueante el
 * connect vuelve al momento y esperamos NOSOTROS con netPoll, que ya
 * sabemos que funciona con descriptores de netSocket.
 *
 * El nombre de la opcion es el problema: en la API de red de lv2 se llama
 * SO_NBIO, pero PSL1GHT ha cambiado nombres de constantes mas de una vez
 * y aqui no hay forma de mirar la cabecera. Asi que se prueban las dos
 * formas que puede tener y, si no esta ninguna, se compila la version
 * bloqueante y SE DICE EN EL LOG. Un degradado silencioso seria peor que
 * el problema: mediriamos igual y no sabriamos por que una region tarda
 * treinta segundos. */
#if defined(SO_NBIO)
#  define GR_NBIO       SO_NBIO
#  define GR_NBIO_TXT   "no bloqueante (SO_NBIO)"
#elif defined(SYS_NET_SO_NBIO)
#  define GR_NBIO       SYS_NET_SO_NBIO
#  define GR_NBIO_TXT   "no bloqueante (SYS_NET_SO_NBIO)"
#else
#  define GR_NBIO_TXT   "BLOQUEANTE: no hay SO_NBIO en las cabeceras"
#endif

/* --------------------------------------------------------------------- */

static sys_mutex_t mtx;
static int         mtx_ok = 0;
#define LOCK()    do { if (mtx_ok) sysMutexLock(mtx, 0); } while (0)
#define UNLOCK()  do { if (mtx_ok) sysMutexUnlock(mtx); } while (0)

static sys_ppu_thread_t tid;
static volatile int running = 0;
static int          started = 0;
static volatile int medir_req = 0;

static pingState estado = PING_NADA;
static u32       lat[XC_REGIONS_MAX];
static int       fallo[XC_REGIONS_MAX];
static int       hechas = 0, total = 0;

static u64 tb_hz = 0;

static u64 ahora_us(void)
{
	return (__gettime() * 1000000ull) / tb_hz;
}

static void plog(const char *fmt, ...)
{
	char buf[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	linkLog("[ping] %s", buf);
}

/* --------------------------------------------------------------------- */
/* Una medida                                                            */
/* --------------------------------------------------------------------- */

/* Resuelve el nombre. Se hace UNA vez por region y FUERA del cronometro:
 * la primera consulta puede irse a cientos de milisegundos si toca
 * preguntarle al servidor de nombres, y eso no es distancia a Azure, es
 * distancia al router de casa. Meterlo dentro seria medir otra cosa y
 * llamarla latencia. */
static int resolver(const char *host, u32 *out)
{
	struct net_hostent *he;
	u32 *list;

	he = netGetHostByName(host);
	if (!he) return -1;
	if (he->h_addrtype != AF_INET || he->h_length != 4) return -2;
	if (he->h_addr_list == 0) return -3;

	list = (u32*)(uintptr_t)he->h_addr_list;
	if (list[0] == 0) return -4;

	*out = *(u32*)(uintptr_t)list[0];
	return 0;
}

/* Un intento. Devuelve los microsegundos del apreton de manos, o 0 si no
 * se pudo. */
static u32 un_intento(u32 addr)
{
	struct pollfd pfd;
	struct sockaddr_in sa;
	int s, r, bien = 0;
	u64 t0;
	u32 us = 0;

	s = netSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s < 0) return 0;

#ifdef GR_NBIO
	{
		int uno = 1;
		netSetSockOpt(s, SOL_SOCKET, GR_NBIO, &uno, sizeof(uno));
	}
#endif

	memset(&sa, 0, sizeof(sa));
	sa.sin_len    = sizeof(sa);
	sa.sin_family = AF_INET;
	sa.sin_port   = htons(443);
	sa.sin_addr.s_addr = addr;

	t0 = ahora_us();
	r = netConnect(s, (struct sockaddr*)&sa, sizeof(sa));

	if (r == 0) {
		/* Conectado de golpe. Pasa con el bloqueante siempre, y con el
		 * no bloqueante si la respuesta ya estaba en camino. */
		us = (u32)(ahora_us() - t0);
		bien = 1;
	} else {
		/* NO SE MIRA EL ERRNO, Y ES A PROPOSITO.
		 *
		 * Aqui tocaria comprobar que el error es EINPROGRESS, pero los
		 * numeros de error de la red de lv2 no son los de newlib y no
		 * hay manera de verificar la constante desde aqui. Comprobar
		 * contra el valor equivocado seria peor que no comprobar: daria
		 * "no se pudo medir" en las regiones que SI contestan.
		 *
		 * Da igual por que ha vuelto -1: si el connect esta en marcha,
		 * netPoll dara POLLOUT cuando termine; si ha fallado de verdad,
		 * dara POLLERR o POLLHUP o se agotara el tiempo. Las tres
		 * respuestas son la misma que buscabamos. */
		memset(&pfd, 0, sizeof(pfd));
		pfd.fd     = s;
		pfd.events = POLLOUT;

		r = netPoll(&pfd, 1, PING_TIMEOUT_MS);

		if (r > 0 && (pfd.revents & POLLOUT) &&
		    !(pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
			us = (u32)(ahora_us() - t0);
			bien = 1;
		}
	}

	netClose(s);

	/* EL EXITO SE LLEVA EN UNA BANDERA, NO EN EL NUMERO.
	 *
	 * Cero significa "sin medir" en toda esta capa, asi que devolver el
	 * tiempo a secas confunde dos cosas: un fallo y una medida que salio
	 * cero. El comentario que habia aqui decia que se redondeaba hacia
	 * arriba, y el codigo de debajo no lo hacia -- un comentario que
	 * describe algo que no pasa es peor que no tener comentario, porque
	 * el siguiente que lo lea se lo cree.
	 *
	 * Ahora: si conecto, minimo 1 us; si no, 0. Sin ambiguedad. */
	if (!bien) return 0;
	return us ? us : 1;
}

static void medir_una(int i)
{
	xcRegion reg;
	u32 addr = 0, mejor = 0;
	int k, buenas = 0;

	if (authRegionCopy(i, &reg) != 0 || !reg.host[0]) {
		LOCK();
		lat[i] = 0;
		fallo[i] = 1;
		UNLOCK();
		plog("%d: sin host, no se puede medir", i);
		return;
	}

	if (resolver(reg.host, &addr) != 0) {
		LOCK();
		lat[i] = 0;
		fallo[i] = 1;
		UNLOCK();
		plog("%s: no resuelve %s", reg.name, reg.host);
		return;
	}

	for (k = 0; k < PING_MUESTRAS; k++) {
		u32 us;

		if (!running) return;

		us = un_intento(addr);
		if (us) {
			buenas++;
			if (!mejor || us < mejor) mejor = us;
		}

		if (k + 1 < PING_MUESTRAS) usleep(PING_PAUSA_US);
	}

	LOCK();
	/* Milisegundos redondeados, y con suelo de 1: una region a 400 us
	 * existe (LAN, o un proxy en casa) y enseñarla como 0 ms la pintaria
	 * de "sin medir". */
	lat[i] = buenas ? (mejor + 500) / 1000 : 0;
	if (buenas && lat[i] == 0) lat[i] = 1;
	fallo[i] = buenas ? 0 : 1;
	UNLOCK();

	plog("%-20s %s -> %u ms (%d/%d respuestas)",
	     reg.name, reg.host,
	     (unsigned)(buenas ? (mejor + 500) / 1000 : 0),
	     buenas, PING_MUESTRAS);
}

/* --------------------------------------------------------------------- */

static void ping_thread(void *arg)
{
	(void)arg;

	while (running) {
		int n, i;

		if (!medir_req) {
			usleep(100000);
			continue;
		}
		medir_req = 0;

		n = (int)authRegionsN();
		if (n <= 0) {
			plog("no hay regiones que medir todavia");
			/* Y se DESHACE el "midiendo" que puso pingMedir. Sin esto
			 * la interfaz se queda con "midiendo 0/0" para siempre
			 * porque nadie va a volver a pasar por aqui. */
			LOCK();
			estado = PING_NADA;
			UNLOCK();
			continue;
		}

		LOCK();
		total = n;
		for (i = 0; i < n && i < XC_REGIONS_MAX; i++) {
			lat[i] = 0;
			fallo[i] = 0;
		}
		UNLOCK();

		plog("midiendo %d regiones, %d muestras cada una, %s",
		     n, PING_MUESTRAS, GR_NBIO_TXT);

		for (i = 0; i < n && i < XC_REGIONS_MAX; i++) {
			if (!running) break;

			medir_una(i);

			LOCK();
			hechas = i + 1;
			UNLOCK();
		}

		LOCK();
		estado = PING_HECHO;
		UNLOCK();

		{
			int mej = pingMejor();
			if (mej >= 0) {
				xcRegion r;
				if (authRegionCopy(mej, &r) == 0)
					plog("la mas rapida: %s con %u ms",
					     r.name, (unsigned)pingMs(mej));
			} else {
				plog("no contesto ninguna region");
			}
		}
	}

	sysThreadExit(0);
}

/* --------------------------------------------------------------------- */

int pingInit(void)
{
	sys_mutex_attr_t attr;

	memset(lat, 0, sizeof(lat));
	memset(fallo, 0, sizeof(fallo));
	estado = PING_NADA;

	tb_hz = sysGetTimebaseFrequency();
	if (tb_hz == 0) tb_hz = 79800000ull;

	sysMutexAttrInitialize(attr);
	mtx_ok = (sysMutexCreate(&mtx, &attr) == 0);

	running = 1;
	if (sysThreadCreate(&tid, ping_thread, NULL, PING_PRIO, PING_STACK,
	                    THREAD_JOINABLE, "GR33N ping") != 0) {
		running = 0;
		plog("sysThreadCreate fallo: no habra medidas");
		return -1;
	}

	started = 1;
	return 0;
}

void pingShutdown(void)
{
	if (!started) return;

	running = 0;
	sysThreadJoin(tid, NULL);
	started = 0;

	if (mtx_ok) {
		sysMutexDestroy(mtx);
		mtx_ok = 0;
	}
}

void pingMedir(void)
{
	if (!started) return;

	/* EL ESTADO SE PONE AQUI, NO EN EL HILO, y esto lo encontro la prueba
	 * del PC.
	 *
	 * El hilo duerme en tramos de 100 ms, asi que entre esta llamada y el
	 * momento en que se entera podian pasar hasta 100 ms. Durante esa
	 * ventana pingEstado() seguia diciendo PING_HECHO -- "ya esta medido"
	 * cuando lo que acaba de pasar es que te han pedido medir otra vez.
	 *
	 * Y no era solo cosmetico: la guarda de "no relanzar si ya se esta
	 * midiendo" leia ese mismo estado. Dos pulsaciones seguidas del boton
	 * dentro de la ventana pasaban las dos, y la segunda dejaba
	 * medir_req puesto para que en cuanto terminara la ronda empezara
	 * otra entera. Cinco regiones por tres intentos, otra vez, contra los
	 * servidores de Microsoft, por pulsar dos veces. */
	LOCK();
	if (estado == PING_MIDIENDO) {
		UNLOCK();
		return;
	}
	estado = PING_MIDIENDO;
	hechas = 0;
	total  = (int)authRegionsN();
	UNLOCK();

	medir_req = 1;
}

pingState pingEstado(void)
{
	pingState s;

	LOCK();
	s = estado;
	UNLOCK();

	return s;
}

u32 pingMs(int i)
{
	u32 v = 0;

	if (i < 0 || i >= XC_REGIONS_MAX) return 0;

	LOCK();
	v = lat[i];
	UNLOCK();

	return v;
}

int pingFallo(int i)
{
	int v = 0;

	if (i < 0 || i >= XC_REGIONS_MAX) return 0;

	LOCK();
	v = fallo[i];
	UNLOCK();

	return v;
}

int pingMejor(void)
{
	int i, mej = -1;
	u32 min = 0;

	LOCK();
	for (i = 0; i < XC_REGIONS_MAX; i++) {
		if (!lat[i] || fallo[i]) continue;
		if (mej < 0 || lat[i] < min) { min = lat[i]; mej = i; }
	}
	UNLOCK();

	return mej;
}

void pingProgreso(int *h, int *t)
{
	LOCK();
	if (h) *h = hechas;
	if (t) *t = total;
	UNLOCK();
}
