/* GR33N - sustituto de <sys/random.h> para PSL1GHT.
 *
 * La newlib de la PS3 no trae esta cabecera. usrsctp la incluye desde
 * sctp_os_userspace.h y sin ella no compila.
 *
 * La entropia de este puerto sale de sysGetRandomNumber, cableada en
 * user_environment.c por el parche de PS3.
 *
 * Vacia a proposito. Existe solo para que el #include no falle
 *
 * Ver deps/ps3-shim/LEEME.md.
 */

#ifndef GR33N_SYS_RANDOM_H
#define GR33N_SYS_RANDOM_H
#endif /* GR33N_SYS_RANDOM_H */
