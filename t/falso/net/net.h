/* Falsa. Lo justo para compilar y EJECUTAR ping.c en el PC contra una red
 * de mentira que responde lo que le digamos.
 *
 * Los bits de poll son los de BSD, que es lo que usa lv2: se comprobo
 * contra el valor de POLLNVAL (0x20) que salio midiendo en la consola. */
#ifndef FALSO_NET_H
#define FALSO_NET_H
#include <stdint.h>
#include <netinet/in.h>

#define POLLIN    0x0001
#define POLLPRI   0x0002
#define POLLOUT   0x0004
#define POLLERR   0x0008
#define POLLHUP   0x0010
#define POLLNVAL  0x0020

/* A PROPOSITO: aqui NO se define SO_NBIO. Asi la compilacion del PC pasa
 * por la rama bloqueante, que es la que menos se mira, y no se queda sin
 * probar nunca. La consola dira en el log cual le ha tocado. */

struct pollfd { int fd; short events; short revents; };

/* h_addr_list son punteros de 32 bits guardados como u32: el ABI de lv2
 * asomando. En el PC son punteros de 64, asi que aqui se declara u64 para
 * que el cast del codigo siga siendo valido. */
struct net_hostent {
	uintptr_t h_name;
	uintptr_t h_aliases;
	int32_t   h_addrtype;
	int32_t   h_length;
	uintptr_t h_addr_list;
};

extern int net_errno;

int netSocket(int dom, int type, int proto);
int netConnect(int s, struct sockaddr *sa, unsigned len);
int netClose(int s);
int netPoll(struct pollfd *fds, int n, int ms);
int netSetSockOpt(int s, int lvl, int opt, const void *val, unsigned len);
struct net_hostent *netGetHostByName(const char *name);
#endif
