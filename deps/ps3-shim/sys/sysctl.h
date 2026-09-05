/* GR33N - sustituto de <sys/sysctl.h> para PSL1GHT.
 *
 * La newlib de la PS3 no trae esta cabecera. usrsctp la incluye desde
 * sctp_os_userspace.h y sin ella no compila.
 *
 * Ajustes del nucleo por nombre. usrsctp trae los suyos propios en
 * sctp_sysctl.c y no consulta los del sistema.
 *
 * Vacia a proposito. Existe solo para que el #include no falle
 *
 * Ver deps/ps3-shim/LEEME.md.
 */

#ifndef GR33N_SYS_SYSCTL_H
#define GR33N_SYS_SYSCTL_H
#endif /* GR33N_SYS_SYSCTL_H */
