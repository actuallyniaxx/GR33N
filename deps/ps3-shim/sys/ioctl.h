/* GR33N - sustituto de <sys/ioctl.h> para PSL1GHT.
 *
 * La newlib de la PS3 no trae esta cabecera. usrsctp la incluye desde
 * sctp_os_userspace.h y sin ella no compila.
 *
 * usrsctp la usa para SIOCGIFMTU, o sea para preguntarle el MTU a una
 * interfaz. Con AF_CONN no hay interfaz a la que preguntar: el MTU
 * efectivo lo decide el transporte que le pongamos debajo.
 *
 * ioctl se declara pero NO se implementa a proposito. Si algun camino
 * llegara a llamarla, el enlazador lo diria con un simbolo sin resolver,
 * que es justo el aviso que queremos -- mejor eso que un stub silencioso
 * devolviendo exito con un MTU inventado
 *
 * Ver deps/ps3-shim/LEEME.md.
 */

#ifndef GR33N_SYS_IOCTL_H
#define GR33N_SYS_IOCTL_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SIOCGIFMTU
#define SIOCGIFMTU 0x8921
#endif
#ifndef SIOCGIFFLAGS
#define SIOCGIFFLAGS 0x8913
#endif

int ioctl(int fd, unsigned long request, ...);

#ifdef __cplusplus
}
#endif

#endif /* GR33N_SYS_IOCTL_H */
