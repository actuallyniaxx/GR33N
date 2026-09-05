/* GR33N - sustituto de <netinet/ip_icmp.h> para PSL1GHT.
 *
 * La newlib de la PS3 no trae esta cabecera. usrsctp la incluye desde
 * sctp_os_userspace.h y sin ella no compila.
 *
 * ICMP. usrsctp lo usa para enterarse de puerto inalcanzable en
 * el camino de sockets crudos, que con AF_CONN no existe. Ademas ya
 * trae su propio user_ip_icmp.h de repuesto.
 *
 * Vacia a proposito. Existe solo para que el #include no falle
 *
 * Ver deps/ps3-shim/LEEME.md.
 */

#ifndef GR33N_NETINET_IP_ICMP_H
#define GR33N_NETINET_IP_ICMP_H
#endif /* GR33N_NETINET_IP_ICMP_H */
