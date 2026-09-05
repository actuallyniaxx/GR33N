/* GR33N - sustituto de <net/route.h> para PSL1GHT.
 *
 * La newlib de la PS3 no trae esta cabecera. usrsctp la incluye desde
 * sctp_os_userspace.h y sin ella no compila.
 *
 * Tabla de rutas del nucleo. usrsctp la deja explicitamente sin
 * portar: su propio comentario lo llama una ruta de mentira provisional.
 *
 * Vacia a proposito. Existe solo para que el #include no falle
 *
 * Ver deps/ps3-shim/LEEME.md.
 */

#ifndef GR33N_NET_ROUTE_H
#define GR33N_NET_ROUTE_H
#endif /* GR33N_NET_ROUTE_H */
