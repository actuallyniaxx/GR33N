/* GR33N - las funciones que usrsctp llama y la PS3 no tiene.
 *
 * TODAS SON DE ENUMERAR INTERFACES DE RED, y ninguna se ejecuta en el
 * camino que vamos a usar.
 *
 * usrsctp funciona en dos modos muy distintos:
 *
 *   - Con sockets de verdad (AF_INET): abre un socket UDP o crudo, tiene
 *     que saber que direcciones locales hay, preguntar el MTU de cada
 *     interfaz y elegir por donde salir.
 *
 *   - Con AF_CONN, que es el nuestro: usrsctp NO toca la red. Le damos
 *     los paquetes que llegan con usrsctp_conninput() y el nos devuelve
 *     los que quiere mandar por una funcion nuestra. Quien los pone en el
 *     cable es libpeer, por DTLS, sobre el socket UDP que ya maneja GR33N.
 *
 * En AF_CONN preguntar por las interfaces locales no es que sea dificil:
 * es que no significa nada. La direccion que importa es la del par
 * remoto, y esa la trae la negociacion ICE.
 *
 * Por eso estos devuelven "no hay nada", que es la verdad, y no un error:
 * un fallo aqui haria que usrsctp se creyera que la maquina esta rota,
 * cuando lo que pasa es que la pregunta no aplica.
 *
 * LO QUE NO SE HACE: ioctl() se declara en el sustituto de sys/ioctl.h
 * pero NO se implementa aqui. Si algun camino llegara a pedir el MTU por
 * ahi, el enlazador lo diria con un simbolo sin resolver -- y eso es
 * justo lo que se quiere, mejor que devolver en silencio un MTU
 * inventado y descubrirlo tres semanas despues con paquetes fragmentados.
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/net.h>
#include <net/netctl.h>

/* --------------------------------------------------------------------- */

/* LA INTERFAZ DE VERDAD, Y POR QUE ESTO YA NO ES UN TAPON.
 *
 * Esto devolvia lista vacia y exito, con un razonamiento que era CIERTO
 * para quien lo llamaba entonces: usrsctp con AF_CONN no manda por IP,
 * asi que no tiene ninguna direccion local que anunciar.
 *
 * Y luego aparecio un segundo cliente. libpeer llama a getifaddrs desde
 * ports_get_host_addr() para montar el CANDIDATO HOST de ICE
 * (agent.c:230). Ahi una lista vacia no es la verdad: es quedarse sin
 * candidatos. En el log del 2026-09-04 la oferta SDP salia entera y
 * valida y sin una sola linea a=candidate, o sea sin ninguna direccion a
 * la que xCloud pudiera contestar.
 *
 * Es la misma forma que el comentario de claim() en net_tls.c --"con el
 * flujo actual sobra"-- que dejo de ser verdad el dia que catalog.c se
 * llevo su propio hilo y nadie volvio a leerlo. Una justificacion atada
 * a QUIEN LLAMA caduca en cuanto llama otro.
 *
 * Ahora se contesta con la interfaz que diga netctl, que es de donde
 * saca GR33N la IP que pinta en el HUD (link.c:671). Una sola entrada:
 * la PS3 tiene una conexion activa a la vez, cable o WiFi, y netctl
 * devuelve esa.
 *
 * Si netctl no coopera se sigue devolviendo la lista vacia con exito, no
 * un error: sin red no hay direccion que dar, y eso es distinto de que
 * la maquina este rota. */
/* Un solo bloque con todo dentro, para que freeifaddrs sea un free().
 *
 * La cara publica va LA PRIMERA a proposito: asi &bloque->cara y el
 * bloque son la misma direccion y quien llama puede soltar lo que le
 * dimos sin saber que hay detras. */
struct gr33n_ifa {
	struct ifaddrs     cara;
	struct sockaddr_in dir;
	char               nombre[8];
};

int getifaddrs(struct ifaddrs **ifap)
{
	union net_ctl_info info;
	struct gr33n_ifa *b;
	struct in_addr ip;

	if (ifap == NULL)
		return -1;

	*ifap = NULL;

	/* netCtlInit() ya lo hizo linkInit() y no se suelta hasta
	 * linkShutdown(), asi que aqui solo se pregunta. Si contesta que no,
	 * lista vacia. */
	memset(&info, 0, sizeof(info));
	if (netCtlGetInfo(NET_CTL_INFO_IP_ADDRESS, &info) != 0)
		return 0;

	memset(&ip, 0, sizeof(ip));
	if (inet_pton(AF_INET, info.ip_address, &ip) != 1)
		return 0;

	/* 0.0.0.0 es "todavia no tengo direccion". Anunciarla como candidato
	 * host seria peor que no anunciar nada: el otro lado se pasaria toda
	 * la negociacion mandando comprobaciones a ninguna parte. */
	if (ip.s_addr == 0)
		return 0;

	b = (struct gr33n_ifa *)calloc(1, sizeof(*b));
	if (b == NULL)
		return -1;

	b->dir.sin_len    = sizeof(b->dir);
	b->dir.sin_family = AF_INET;
	b->dir.sin_addr   = ip;

	/* El nombre solo se mira si CONFIG_IFACE_PREFIX no esta vacio
	 * (ports.c:75). Con el prefijo vacio manda la rama de las banderas. */
	strncpy(b->nombre, "net0", sizeof(b->nombre) - 1);

	b->cara.ifa_next    = NULL;
	b->cara.ifa_name    = b->nombre;
	b->cara.ifa_flags   = IFF_UP | IFF_RUNNING | IFF_BROADCAST |
	                      IFF_MULTICAST;
	b->cara.ifa_addr    = (struct sockaddr *)&b->dir;

	/* ifa_netmask, ifa_dstaddr e ifa_data se quedan como los dejo el
	 * calloc, que es NULL, y no se tocan a mano A PROPOSITO: la <net/if.h>
	 * de glibc define ifa_dstaddr como MACRO (ifa_ifu.ifu_dstaddr), asi
	 * que asignarle por nombre revienta en cuanto alguien compile este
	 * fichero contra las cabeceras del PC -- que es justo lo que hacemos
	 * para comprobarlo antes de mandartelo. Escribir un valor que ya
	 * esta puesto no aporta nada y sujeta el fichero a un nombre que no
	 * controlamos. */

	*ifap = &b->cara;
	return 0;
}

void freeifaddrs(struct ifaddrs *ifa)
{
	/* Vale con un free porque la cara es el primer campo del bloque. Y
	 * solo hay una entrada, asi que no hay lista que recorrer. */
	free(ifa);
}

/* --------------------------------------------------------------------- */

/* Cero es "no existe esa interfaz", y es la respuesta honesta.
 *
 * El valor 0 NO es un indice valido en el interfaz de BSD, asi que quien
 * llame puede distinguirlo de un resultado bueno sin ambiguedad. */
unsigned int if_nametoindex(const char *ifname)
{
	(void)ifname;
	return 0;
}

char *if_indextoname(unsigned int ifindex, char *ifname)
{
	(void)ifindex;

	/* NULL es "no hay interfaz con ese indice". Se deja el buffer en
	 * cadena vacia de todos modos: quien llame podria mirarlo sin
	 * comprobar el retorno, y una cadena sin terminar ahi es un buffer
	 * mal leido esperando a pasar. */
	if (ifname != NULL)
		ifname[0] = '\0';

	return NULL;
}

/* --------------------------------------------------------------------- */
/* Los hilos de recepcion que no van a existir                           */
/* --------------------------------------------------------------------- */

/* user_recv_thread.c NO SE COMPILA. Estas dos funciones son todo lo que ese
 * fichero exporta -- comprobado con nm sobre la biblioteca del cruzado: dos
 * simbolos, `recv_thread_init` y `recv_thread_destroy`, y nada mas--, y se
 * llaman desde exactamente dos sitios: sctp_pcb.c:6621 y sctp_usrreq.c:190.
 *
 * POR QUE FUERA. Ese fichero es el modo en el que usrsctp abre sus PROPIOS
 * sockets: uno crudo de SCTP y otro UDP para el tunel, con sus hilos
 * dedicados leyendo de ellos. Son 1500 lineas que llaman a socket(), bind(),
 * setsockopt(), recvmsg() y close(), ninguna de las cuales existe en
 * PSL1GHT: su API es netSocket/netBind/netSetSockOpt/netRecv/netClose, que
 * es la que GR33N usa desde el primer dia. Eran cinco simbolos sin resolver
 * en el enlazado final del EBOOT, por codigo que nunca se ejecutaria.
 *
 * Y NUNCA SE EJECUTARIA porque libpeer arranca usrsctp asi (sctp.c:567):
 *
 *     usrsctp_init(0, sctp_outgoing_data_cb, NULL);
 *     ...
 *     usrsctp_socket(AF_CONN, SOCK_STREAM, IPPROTO_SCTP, ...)
 *
 * Puerto 0 -sin tunel UDP- y AF_CONN, que quiere decir que usrsctp no toca
 * la red: los paquetes que llegan se los damos con usrsctp_conninput() y los
 * que quiere mandar nos los devuelve por sctp_outgoing_data_cb. Quien los
 * pone en el cable es libpeer por DTLS, sobre el socket UDP de GR33N.
 *
 * NO ES UN APAÑO NUESTRO: es lo que hace WebRTC. Su integracion de usrsctp
 * llama a usrsctp_init_nothreads() por este mismo motivo. La diferencia es
 * que ese `nothreads` tambien apaga el HILO DE TEMPORIZADORES, que nosotros
 * SI queremos -es el que retransmite-, asi que en vez de apagar los dos
 * hilos se deja el de temporizadores y se vacia solo este.
 *
 * Sin esto, recv_thread_init() intentaria abrir un socket crudo de SCTP al
 * arrancar. En Linux sin root eso falla en silencio y por eso nadie se
 * queja; aqui ni siquiera hay a que llamar. */
void recv_thread_init(void)
{
	/* Nada que arrancar: con AF_CONN no hay socket del que recibir. */
}

void recv_thread_destroy(void)
{
	/* Y nada que parar. */
}
