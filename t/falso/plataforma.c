/* La PS3 de mentira: lo justo para que ping.c compile Y SE EJECUTE en el
 * PC, contra una red guionizada.
 *
 * Los hilos son pthreads de verdad, no un apaño de un solo hilo: asi la
 * prueba pasa por el mismo reparto entre el hilo del medidor y el que
 * pregunta, que es donde estarian los fallos de candado. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>

#include "falso/net/net.h"
#include "guion.h"

/* --- reloj --------------------------------------------------------- */

uint64_t sysGetTimebaseFrequency(void) { return 1000000ull; }

uint64_t __gettime(void)
{
	static uint64_t base = 0;
	struct timespec ts;
	uint64_t us;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	us = (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;

	if (!base) base = us;
	return us - base;
}

/* --- hilos --------------------------------------------------------- */

typedef struct { void (*fn)(void*); void *arg; } arranque;

static void *trampolin(void *p)
{
	/* Se copia y se libera ANTES de entrar, no despues.
	 *
	 * ping_thread termina con sysThreadExit(), que aqui es pthread_exit:
	 * no vuelve nunca, asi que un free() puesto debajo no se ejecuta. Lo
	 * canto LeakSanitizer, y tenia razon. */
	arranque a = *(arranque*)p;
	free(p);
	a.fn(a.arg);
	return NULL;
}

int sysThreadCreate(uint64_t *id, void (*entry)(void*), void *arg,
                    int prio, size_t stack, int flags, const char *name)
{
	pthread_t t;
	arranque *a = malloc(sizeof(*a));

	(void)prio; (void)stack; (void)flags; (void)name;
	a->fn = entry; a->arg = arg;

	if (pthread_create(&t, NULL, trampolin, a) != 0) { free(a); return -1; }
	*id = (uint64_t)t;
	return 0;
}

int sysThreadJoin(uint64_t id, uint64_t *ret)
{
	(void)ret;
	return pthread_join((pthread_t)id, NULL);
}

void sysThreadExit(int code) { (void)code; pthread_exit(NULL); }

/* --- candados ------------------------------------------------------ */

int sysMutexCreate(uint64_t *m, void *a)
{
	pthread_mutex_t *p = malloc(sizeof(*p));
	(void)a;
	pthread_mutex_init(p, NULL);
	*m = (uint64_t)(uintptr_t)p;
	return 0;
}
int sysMutexDestroy(uint64_t m)
{
	pthread_mutex_t *p = (pthread_mutex_t*)(uintptr_t)m;
	pthread_mutex_destroy(p); free(p); return 0;
}
int sysMutexLock(uint64_t m, unsigned t)
{
	(void)t; return pthread_mutex_lock((pthread_mutex_t*)(uintptr_t)m);
}
int sysMutexUnlock(uint64_t m)
{
	return pthread_mutex_unlock((pthread_mutex_t*)(uintptr_t)m);
}

/* --- log ----------------------------------------------------------- */

int guion_ruidoso = 0;

void linkLog(const char *fmt, ...)
{
	va_list ap;
	if (!guion_ruidoso) return;
	fputs("      | ", stdout);
	va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
	putchar('\n');
}

/* --- la red de mentira --------------------------------------------- */

int net_errno = 0;

guionHost guion[GUION_MAX];
int       guion_n = 0;

/* LA LISTA DE DIRECCIONES VIVE POR DEBAJO DE LOS 4 GB, Y NO ES UN CAPRICHO.
 *
 * net_hostent guarda punteros como u32: es el ABI de lv2, donde todo cabe
 * en 32 bits. resolver() en ping.c hace el cast que corresponde a eso, y
 * hace bien. En un PC de 64 bits ese cast lee media direccion y revienta
 * -- lo hizo, con ASan senalando la linea exacta.
 *
 * El fallo era de ESTA cabecera de mentira, no de ping.c: la de mentira
 * prometia un ABI de 32 bits y devolvia punteros de 64. Se arregla donde
 * estaba: se pide la memoria con MAP_32BIT, que en x86-64 la coloca por
 * debajo de los 2 GB, y entonces un u32 la representa entera igual que en
 * la consola. Cambiar resolver() para que leyera uintptr_t habria hecho
 * pasar la prueba y roto la PS3. */
static uint32_t *g_baja = NULL;   /* [0..7] lista, [8..15] direcciones */

static void baja_init(void)
{
	if (g_baja) return;

	g_baja = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
	              MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);

	if (g_baja == MAP_FAILED || (uintptr_t)g_baja > 0xffffffffu) {
		fprintf(stderr,
		        "!! no hay memoria por debajo de 4 GB: esta prueba no "
		        "puede imitar el ABI de lv2 en esta maquina\n");
		exit(2);
	}
}

uint16_t htons(uint16_t x) { return (uint16_t)((x << 8) | (x >> 8)); }

struct net_hostent *netGetHostByName(const char *name)
{
	static struct net_hostent he;
	int i;

	baja_init();

	for (i = 0; i < guion_n; i++) {
		if (strcmp(guion[i].host, name) != 0) continue;
		if (!guion[i].resuelve) return NULL;

		/* g_baja[8] son los cuatro bytes de la direccion; g_baja[0] y
		 * [1] son la lista terminada en cero que apunta a ellos. Todo
		 * en u32, como en la consola. */
		g_baja[8] = 0x0a000000u | (uint32_t)i;
		g_baja[0] = (uint32_t)(uintptr_t)&g_baja[8];
		g_baja[1] = 0;

		he.h_name = 0;
		he.h_aliases = 0;
		he.h_addrtype = AF_INET;
		he.h_length = 4;
		he.h_addr_list = (uintptr_t)g_baja;
		return &he;
	}
	return NULL;
}

/* fd -> que host es, para saber cuanto tardar */
static int fd_host[64];
static int fd_seq[64];
static int prox_fd = 3;

int netSocket(int dom, int type, int proto)
{
	(void)dom; (void)type; (void)proto;
	if (prox_fd >= 64) prox_fd = 3;
	return prox_fd++;
}

int netConnect(int s, struct sockaddr *sa, unsigned len)
{
	struct sockaddr_in *in = (struct sockaddr_in*)sa;
	uint32_t a = in->sin_addr.s_addr;
	int i = (int)(a & 0xff);
	int n;

	(void)len;

	if (i < 0 || i >= guion_n) return -1;

	fd_host[s % 64] = i;
	n = guion[i].intentos++;
	fd_seq[s % 64] = n;

	if (!guion[i].acepta) return -1;   /* rechaza: al netPoll */

	/* Bloqueante: se duerme lo guionizado y se vuelve con exito. */
	usleep((useconds_t)guion[i].us[n % GUION_US]);
	return 0;
}

int netPoll(struct pollfd *fds, int n, int ms)
{
	int i = fd_host[fds[0].fd % 64];

	(void)n; (void)ms;

	/* Solo se llega aqui cuando netConnect ha fallado, y en esta red de
	 * mentira eso solo pasa si el host rechaza. */
	if (i >= 0 && i < guion_n && !guion[i].acepta) {
		fds[0].revents = POLLERR | POLLHUP;
		return 1;
	}

	fds[0].revents = 0;
	return 0;
}

int netClose(int s) { (void)s; return 0; }

int netSetSockOpt(int s, int lvl, int opt, const void *v, unsigned l)
{
	(void)s; (void)lvl; (void)opt; (void)v; (void)l;
	return 0;
}
