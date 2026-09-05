/* GR33N - sustituto de <net/if.h> para PSL1GHT.
 *
 * La newlib de la PS3 no trae esta cabecera. usrsctp la incluye desde
 * sctp_os_userspace.h y sin ella no compila.
 *
 * SOLO LLEVA LO QUE usrsctp TOCA, y eso es poco: usa net/if.h para
 * enumerar interfaces y preguntar el MTU, y con AF_CONN no hace ni una
 * cosa ni la otra -- los paquetes se los damos y se los recogemos
 * nosotros, sin pasar por ninguna interfaz del sistema.
 *
 * Los tipos estan para que compile. Las funciones las resuelve
 * ps3_stubs.c, que devuelve "no hay interfaces", que es la verdad
 *
 * Ver deps/ps3-shim/LEEME.md.
 */

#ifndef GR33N_NET_IF_H
#define GR33N_NET_IF_H

#include <sys/types.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

/* El tamano del nombre de interfaz. 16 es el valor de BSD y de Linux, y
 * usrsctp lo usa para dimensionar buffers: cambiarlo cambia tamanos de
 * estructura. */
#ifndef IFNAMSIZ
#define IFNAMSIZ 16
#endif
#ifndef IF_NAMESIZE
#define IF_NAMESIZE IFNAMSIZ
#endif

/* Recortada a lo que usa sctp_userspace.c: ifr_name para pedir el MTU e
 * ifr_mtu para recogerlo. La union completa de BSD tiene una docena de
 * miembros mas que nadie mira aqui. */
struct ifreq {
	char ifr_name[IFNAMSIZ];
	union {
		struct sockaddr ifru_addr;
		short           ifru_flags;
		int             ifru_mtu;
		int             ifru_index;
	} ifr_ifru;
};

#define ifr_addr  ifr_ifru.ifru_addr
#define ifr_flags ifr_ifru.ifru_flags
#define ifr_mtu   ifr_ifru.ifru_mtu
#define ifr_index ifr_ifru.ifru_index

/* --------------------------------------------------------------------- */
/* Las banderas de estado de una interfaz                                */
/* --------------------------------------------------------------------- */

/* libpeer las mira en ports.c:82-90, recorriendo lo que devuelve
 * getifaddrs() para quedarse con la primera interfaz que este levantada,
 * en marcha y que no sea la de bucle local.
 *
 * Aqui getifaddrs() devuelve la lista VACIA -- es ps3_stubs.c, y con
 * AF_CONN esa pregunta no significa nada--, asi que ese bucle no da ni una
 * vuelta y estas tres constantes no se llegan a leer nunca. Pero el fichero
 * tiene que compilar, y sin ellas son tres "undeclared" que paran el build.
 *
 * Los valores son los de BSD y los de Linux, que coinciden en estos tres
 * desde los anos ochenta. Da igual cual se ponga mientras sean distintos
 * entre si -- nadie del otro lado los va a leer-- pero poner los de verdad
 * cuesta lo mismo y evita que alguien los compare algun dia con los de una
 * captura y se vuelva loco. */
#ifndef IFF_UP
#define IFF_UP       0x1     /* la interfaz esta levantada        */
#endif
#ifndef IFF_BROADCAST
#define IFF_BROADCAST 0x2    /* tiene direccion de difusion       */
#endif
#ifndef IFF_LOOPBACK
#define IFF_LOOPBACK 0x8     /* es la de bucle local              */
#endif
#ifndef IFF_POINTOPOINT
#define IFF_POINTOPOINT 0x10 /* enlace punto a punto              */
#endif
#ifndef IFF_RUNNING
#define IFF_RUNNING  0x40    /* hay recursos asignados            */
#endif
#ifndef IFF_MULTICAST
#define IFF_MULTICAST 0x8000 /* admite multidifusion              */
#endif

unsigned int  if_nametoindex(const char *ifname);
char         *if_indextoname(unsigned int ifindex, char *ifname);

#ifdef __cplusplus
}
#endif

#endif /* GR33N_NET_IF_H */
