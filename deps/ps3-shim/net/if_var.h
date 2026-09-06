/* GR33N - sustituto de <net/if_var.h> para PSL1GHT.
 *
 * La newlib de la PS3 no trae esta cabecera. usrsctp la incluye desde
 * sctp_os_userspace.h y sin ella no compila.
 *
 * Estructuras internas del subsistema de red del nucleo de BSD.
 * En espacio de usuario no se usa nada de aqui.
 *
 * Vacia a proposito. Existe solo para que el #include no falle
 *
 * Ver deps/ps3-shim/README.md.
 */

#ifndef GR33N_NET_IF_VAR_H
#define GR33N_NET_IF_VAR_H
#endif /* GR33N_NET_IF_VAR_H */
