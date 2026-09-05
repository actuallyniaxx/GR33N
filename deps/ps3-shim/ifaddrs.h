/* GR33N - sustituto de <ifaddrs.h> para PSL1GHT.
 *
 * La newlib de la PS3 no trae esta cabecera. usrsctp la incluye desde
 * sctp_os_userspace.h y sin ella no compila.
 *
 * getifaddrs enumera las direcciones locales. Con AF_CONN eso no hace
 * falta: usrsctp no tiene que saber que direcciones tiene la consola
 * porque no es el quien manda los paquetes por la red.
 *
 * El stub de ps3_stubs.c devuelve una lista vacia y exito, que es
 * exactamente la verdad de esta plataforma
 *
 * Ver deps/ps3-shim/LEEME.md.
 */

#ifndef GR33N_IFADDRS_H
#define GR33N_IFADDRS_H

#include <sys/types.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ifaddrs {
	struct ifaddrs  *ifa_next;
	char            *ifa_name;
	unsigned int     ifa_flags;
	struct sockaddr *ifa_addr;
	struct sockaddr *ifa_netmask;
	struct sockaddr *ifa_dstaddr;
	void            *ifa_data;
};

#ifndef ifa_broadaddr
#define ifa_broadaddr ifa_dstaddr
#endif

int  getifaddrs(struct ifaddrs **ifap);
void freeifaddrs(struct ifaddrs *ifa);

#ifdef __cplusplus
}
#endif

#endif /* GR33N_IFADDRS_H */
