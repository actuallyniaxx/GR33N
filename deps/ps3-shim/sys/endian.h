/* GR33N - sustituto de <sys/endian.h> para PSL1GHT.
 *
 * La newlib de la PS3 no trae esta cabecera. usrsctp la incluye desde
 * sctp_os_userspace.h y sin ella no compila.
 *
 * El orden de bytes de esta consola no es una pregunta: es PowerPC
 * big-endian y punto. Se responde con las macros que predefine el propio
 * ppu-gcc, comprobadas con el cruzado de la misma familia:
 *
 *     __BIG_ENDIAN__ 1
 *     __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
 *
 * y con un #error si alguna vez dejara de ser cierto, porque una
 * conversion de orden equivocada no da error: da paquetes que no se
 * entienden
 *
 * Ver deps/ps3-shim/LEEME.md.
 */

#ifndef GR33N_SYS_ENDIAN_H
#define GR33N_SYS_ENDIAN_H

#define __LITTLE_ENDIAN 1234
#define __BIG_ENDIAN    4321
#define __PDP_ENDIAN    3412

#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__)
#if __BYTE_ORDER__ != __ORDER_BIG_ENDIAN__
#error "esta cabecera es para la PS3, que es big-endian, y el compilador dice que no"
#endif
#endif

#define __BYTE_ORDER  __BIG_ENDIAN
#define BYTE_ORDER    __BYTE_ORDER
#define LITTLE_ENDIAN __LITTLE_ENDIAN
#define BIG_ENDIAN    __BIG_ENDIAN
#define PDP_ENDIAN    __PDP_ENDIAN

/* En big-endian el orden de red y el del anfitrion coinciden, asi que
 * todas estas son la identidad. No es un atajo perezoso: es lo que
 * significan. */
#define htobe16(x) (x)
#define htobe32(x) (x)
#define htobe64(x) (x)
#define be16toh(x) (x)
#define be32toh(x) (x)
#define be64toh(x) (x)

#endif /* GR33N_SYS_ENDIAN_H */
