/* GR33N - sustituto de <machine/in_cksum.h> para PSL1GHT.
 *
 * La newlib de la PS3 no trae esta cabecera. usrsctp la incluye desde
 * sctp_os_userspace.h y sin ella no compila.
 *
 * Suma de comprobacion de IP acelerada por arquitectura. Aqui no hay
 * cabeceras IP que sumar: se las damos ya montadas al transporte.
 *
 * Vacia a proposito. Existe solo para que el #include no falle
 *
 * Ver deps/ps3-shim/README.md.
 */

#ifndef GR33N_MACHINE_IN_CKSUM_H
#define GR33N_MACHINE_IN_CKSUM_H
#endif /* GR33N_MACHINE_IN_CKSUM_H */
