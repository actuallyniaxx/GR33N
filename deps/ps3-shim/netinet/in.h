/* GR33N - envoltura del <netinet/in.h> de PSL1GHT.
 *
 * ESTA CABECERA NO SUSTITUYE A LA DE LA CONSOLA: la envuelve.
 *
 * PSL1GHT trae netinet/in.h y funciona -- link.c de GR33N lleva semanas
 * usandola. Lo que NO trae es IPv6, y usrsctp declara campos de tipo
 * `struct in6_addr` y `struct sockaddr_in6` SIN guardarlos tras INET6:
 *
 *     user_inpcb.h:72    struct in6_addr ie6_foreign;
 *     user_inpcb.h:77    struct in6_addr ie6_local;
 *     usrsctp.h:152      struct sockaddr_in6 sin6;
 *     user_ip6_var.h:74  struct in6_addr ip6_src;
 *
 * Estan dentro de uniones y de estructuras que se reservan enteras, asi que
 * el tipo tiene que EXISTIR aunque no se use jamas. Compilar con -DINET a
 * secas no evita esas lineas: solo evita el codigo que las mira.
 *
 * #include_next es la herramienta para exactamente esto. Continua la
 * busqueda DESPUES del directorio donde se encontro este fichero, o sea que
 * trae la de PSL1GHT de verdad, y luego se le anade lo que falta. Sin ese
 * truco, poner un netinet/in.h en el arbol de sustitutos taparia la buena y
 * nos quedariamos sin sockaddr_in, sin htons y sin todo lo demas.
 *
 * Las dos estructuras estan copiadas literalmente de FreeBSD
 * (sys/netinet6/in6.h) y no escritas de memoria: la disposicion importa
 * porque usrsctp reserva y copia estructuras que las contienen, y un
 * tamaño distinto del que espera su propio codigo es corrupcion de memoria
 * silenciosa. sin6_len va delante porque la newlib de PSL1GHT es de la
 * familia BSD y usrsctp se compila con HAVE_SIN6_LEN.
 */

#ifndef GR33N_NETINET_IN_H
#define GR33N_NETINET_IN_H

/* La de la consola, primero y entera. */
#include_next <netinet/in.h>

#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GR33N_HAVE_IN6 lo pone build-usrsctp.sh si la consola ya las trae. Igual
 * que con struct iovec: desde el preprocesador no se puede preguntar si un
 * struct existe, asi que se pregunta compilando. */
#ifndef GR33N_HAVE_IN6

struct in6_addr {
	union {
		uint8_t  __u6_addr8[16];
		uint16_t __u6_addr16[8];
		uint32_t __u6_addr32[4];
	} __u6_addr;
};

#define s6_addr   __u6_addr.__u6_addr8
#define s6_addr8  __u6_addr.__u6_addr8
#define s6_addr16 __u6_addr.__u6_addr16
#define s6_addr32 __u6_addr.__u6_addr32

struct sockaddr_in6 {
	uint8_t         sin6_len;
	sa_family_t     sin6_family;
	in_port_t       sin6_port;
	uint32_t        sin6_flowinfo;
	struct in6_addr sin6_addr;
	uint32_t        sin6_scope_id;
};

#ifndef AF_INET6
#define AF_INET6 28        /* el valor de BSD */
#endif
#ifndef PF_INET6
#define PF_INET6 AF_INET6
#endif
#ifndef IPPROTO_IPV6
#define IPPROTO_IPV6 41
#endif

/* La direccion "cualquiera" de IPv6, o sea dieciseis ceros.
 *
 * libpeer la usa en socket.c:42, dentro del `case AF_INET6:` de un switch
 * de tiempo de ejecucion. Ese caso no se toma nunca -- CONFIG_IPV6 es 0 y
 * la PS3 no habla IPv6-- pero el compilador tiene que traducir la rama, y
 * sin esto son dos "undeclared" que paran el build.
 *
 * Se declara aqui y se define en ps3_net_stubs.c. No puede ser una macro:
 * el codigo hace una ASIGNACION de estructura (`sin6_addr = in6addr_any`),
 * asi que tiene que ser un objeto de verdad. */
extern const struct in6_addr in6addr_any;
extern const struct in6_addr in6addr_loopback;

#endif /* !GR33N_HAVE_IN6 */

/* --------------------------------------------------------------------- */
/* INET6_ADDRSTRLEN                                                      */
/* --------------------------------------------------------------------- */

/* Cuanto ocupa una direccion IPv6 escrita en texto, con el cero final.
 *
 * PSL1GHT trae INET_ADDRSTRLEN (16, para "255.255.255.255") pero no la de
 * IPv6, y address.h:13 de libpeer hace
 *
 *     #define ADDRSTRLEN INET6_ADDRSTRLEN
 *
 * o sea que la usa para dimensionar TODOS sus buffers de direccion, IPv4
 * incluidas. Se queda fuera del guarda GR33N_HAVE_IN6 a proposito: la
 * consola podria traer los tipos de IPv6 algun dia y seguir sin esta macro,
 * y son dos cosas distintas.
 *
 * 46 es el valor de siempre: 45 caracteres del caso mas largo -- una IPv4
 * empotrada en IPv6 con prefijo, "0000:...:255.255.255.255"-- mas el cero.
 * Ponerlo mas corto seria un buffer que se desborda al formatear. */
#ifndef INET6_ADDRSTRLEN
#define INET6_ADDRSTRLEN 46
#endif

/* --------------------------------------------------------------------- */
/* Como se averigua a que direccion venia un datagrama                   */
/* --------------------------------------------------------------------- */

/* user_recv_thread.c se planta con un #error si no encuentra NI IP_PKTINFO
 * NI IP_RECVDSTADDR (linea 69): son las dos formas, la de Linux y la de
 * BSD, de preguntar por el destino de un datagrama recibido.
 *
 * A nosotros no nos hace falta ninguna -- AF_CONN no abre sockets, los
 * paquetes se los damos nosotros -- pero el fichero tiene que compilar.
 *
 * Se declara la de BSD, que es la que le pega a esta newlib y ademas solo
 * necesita struct in_addr, que la consola ya trae. El valor da igual: ese
 * setsockopt no se ejecuta jamas en nuestro camino. */
#if !defined(IP_PKTINFO) && !defined(IP_RECVDSTADDR)
#define IP_RECVDSTADDR 7
#endif

/* Y por si PSL1GHT si definiera IP_PKTINFO en alguna version: entonces la
 * rama que se compila es la de Linux y hace falta este struct. La
 * disposicion es la de Linux porque IP_PKTINFO es suyo. */
#ifndef GR33N_HAVE_PKTINFO
struct in_pktinfo {
	int            ipi_ifindex;
	struct in_addr ipi_spec_dst;
	struct in_addr ipi_addr;
};
#endif

/* --------------------------------------------------------------------- */
/* IPPORT_RESERVED                                                       */
/* --------------------------------------------------------------------- */

/* El limite historico de Unix: los puertos por debajo de 1024 solo los
 * puede pedir root. usrsctp lo mira en sctp_pcb.c:3332 al hacer bind:
 *
 *     if (ntohs(lport) < IPPORT_RESERVED &&
 *     #elif defined(__Userspace__)
 *         0) {
 *
 * O sea que en espacio de usuario la condicion entera es SIEMPRE FALSA -el
 * segundo operando es un cero literal-, y el valor de esta macro da
 * exactamente igual. Lo unico que hace falta es que exista para que el
 * fichero compile. Se pone el 1024 de siempre por no inventarse otro.
 *
 * PSL1GHT no lo trae porque en la PS3 no hay usuarios ni privilegios de
 * puerto: la consola es el unico que abre sockets. */
#ifndef IPPORT_RESERVED
#define IPPORT_RESERVED 1024
#endif

#ifdef __cplusplus
}
#endif

#endif /* GR33N_NETINET_IN_H */
