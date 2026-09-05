/* GR33N - lo que libpeer pide de la red y PSL1GHT no da con ese nombre.
 *
 * ESTE FICHERO ES CORTO, Y ESA ES LA NOTICIA.
 *
 * libpeer, a diferencia de usrsctp, SI abre sockets: su agente de ICE monta
 * el UDP, manda las peticiones STUN y espera las respuestas. Antes de
 * escribir nada se pregunto que trae la consola, ENLAZANDO de verdad y no
 * leyendo cabeceras (deps/sonda-sockets.sh), y la respuesta fue mucho mejor
 * de lo esperado: PSL1GHT trae socket, bind, connect, close, setsockopt,
 * getsockname, send, recv, sendto, recvfrom, inet_pton, inet_ntop y las
 * cuatro de orden de bytes. Con los nombres de BSD, tal cual.
 *
 * De las diecinueve cosas que libpeer usa, faltan dos, y solo UNA esta
 * aqui:
 *
 *   select()      -> aqui, sobre netSelect().
 *
 *   getaddrinfo() -> NO. Se resuelve reescribiendo ports_resolve_addr() en
 *                    deps/libpeer-ps3.patch, sobre netGetHostByName.
 *                    Fabricar un getaddrinfo falso obligaria a declarar
 *                    struct addrinfo -- que puede no existir siquiera en
 *                    este netdb.h-- y a montar y luego liberar una lista
 *                    enlazada, todo para quedarse con la primera entrada.
 *
 * LO QUE NO ESTA AQUI Y PODRIA PARECER QUE FALTA: getifaddrs, freeifaddrs,
 * if_nametoindex e if_indextoname. Las usa libpeer (ports.c) y las usa
 * usrsctp, y estan en ps3_stubs.c, que entra en libusrsctp.a. Definirlas
 * tambien aqui serian cuatro simbolos duplicados en el enlazado final.
 */

/* GR33N_FALTA_SELECT lo pone deps/build-libpeer.sh SOLO si la consola no
 * trae select con ese nombre. No se decide aqui, y no se decide leyendo
 * cabeceras: se decide enlazando un programa que lo llama.
 *
 * HAY QUE PREGUNTARLO PORQUE LA PRIMERA SONDA NO LO DEJO CLARO. Su prueba
 * de select() incluia fd_set, FD_ZERO, FD_SET, select Y FD_ISSET; la de
 * netSelect() lo mismo MENOS FD_ISSET. Fallo la primera y paso la segunda,
 * y de ahi se dedujo "falta select" -- pero se diferenciaban en DOS cosas,
 * asi que la que faltaba podia ser cualquiera de ellas. Misma clase de
 * error que ya salio con BYTE_ORDER y con CMSG: una prueba que mide dos
 * cosas a la vez no contesta ninguna.
 *
 * DONDE SE USA: agent.c:94, en el bucle de recepcion del agente de ICE, con
 * un tiempo de espera de 1 ms; y mdns.c:147, que no nos hace falta pero
 * tiene que compilar. El tercero, ssl_transport.c:27, es de la
 * señalizacion y se compila a nada con DISABLE_PEER_SIGNALING.
 *
 * OJO CON EL TIEMPO DE ESPERA, y esto es para el dia que se mida en
 * hardware: green-nx cuenta VUELTAS y no milisegundos -- AGENT_CONNCHECK_MAX
 * son 1500 iteraciones-- dando por hecho que cada select de 1 ms tarda uno
 * o dos. Si netSelect redondea al tick del sistema, cada vuelta cuesta diez
 * y el presupuesto entero de conncheck se multiplica por cinco. Es lo mismo
 * que ya paso con FRAME_WAIT_US, calibrado contra localhost. */

/* --------------------------------------------------------------------- */
/* Las dos direcciones fijas de IPv6                                     */
/* --------------------------------------------------------------------- */

/* libpeer las usa en socket.c:42, dentro del `case AF_INET6:` de un switch
 * de tiempo de ejecucion. Ese caso NO se toma nunca: CONFIG_IPV6 es 0, la
 * PS3 no habla IPv6, y la unica direccion IPv6 que da xCloud es Teredo --
 * un tunel sobre IPv4 que tampoco podriamos usar. Pero el compilador tiene
 * que traducir la rama, y el enlazador que resolver el simbolo.
 *
 * No pueden ser macros: el codigo hace una asignacion de estructura,
 * `sin6_addr = in6addr_any`, asi que hace falta un objeto de verdad.
 *
 * Los valores son los que dice el RFC 4291 y no hay margen: "cualquiera"
 * son dieciseis ceros y "bucle local" son quince ceros y un uno. Se
 * escriben byte a byte y no con un memset o un designador, porque asi se
 * ve lo que son. */
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#ifndef GR33N_HAVE_IN6
const struct in6_addr in6addr_any = { { {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
} } };

const struct in6_addr in6addr_loopback = { { {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1
} } };
#endif

/* --------------------------------------------------------------------- */
/* select()                                                              */
/* --------------------------------------------------------------------- */

#if defined(GR33N_FALTA_SELECT)

#include <sys/types.h>
#include <sys/time.h>
#include <sys/select.h>
#include <net/net.h>

int select(int nfds, fd_set *lectura, fd_set *escritura, fd_set *error,
           struct timeval *espera)
{
	/* Reenvio directo: netSelect tiene la misma firma y la misma
	 * semantica, y usa los MISMOS fd_set -- la sonda confirmo que
	 * fd_set, FD_ZERO y FD_SET son los de la consola. No hay nada que
	 * traducir; lo unico que falta es que el simbolo exista con el
	 * nombre que espera libpeer. */
	return netSelect(nfds, lectura, escritura, error, espera);
}

#else

/* La consola SI trae select. Este fichero se queda vacio a proposito en vez
 * de no compilarse: que el objeto exista y este vacio deja constancia en el
 * build de que la pregunta se hizo y la respuesta fue "no hace falta". */
typedef int gr33n_select_no_hacia_falta;

#endif /* GR33N_FALTA_SELECT */
