/* GR33N - sustituto de <netinet/tcp.h> para PSL1GHT.
 *
 * La newlib de la PS3 no trae esta cabecera. usrsctp la incluye desde
 * sctp_os_userspace.h y sin ella no compila.
 *
 * Solo por TCP_NODELAY en el camino de sockets. AF_CONN no abre
 * ninguno.
 *
 * Vacia a proposito. Existe solo para que el #include no falle
 *
 * Ver deps/ps3-shim/README.md.
 */

#ifndef GR33N_NETINET_TCP_H
#define GR33N_NETINET_TCP_H
#endif /* GR33N_NETINET_TCP_H */
