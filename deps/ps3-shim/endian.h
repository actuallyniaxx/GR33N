/* GR33N - sustituto de <endian.h> para PSL1GHT.
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
 * Ver deps/ps3-shim/README.md.
 */

#ifndef GR33N_ENDIAN_H
#define GR33N_ENDIAN_H

#define __LITTLE_ENDIAN 1234
#define __BIG_ENDIAN    4321
#define __PDP_ENDIAN    3412

#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__)
#if __BYTE_ORDER__ != __ORDER_BIG_ENDIAN__
#error "esta cabecera es para la PS3, que es big-endian, y el compilador dice que no"
#endif
#endif

/* SI ALGUIEN YA LO HA DEFINIDO, SE COMPRUEBA EN VEZ DE PISARLO.
 *
 * Antes aqui habia un `#define __BYTE_ORDER __BIG_ENDIAN` a secas, y eso
 * TAPABA un -D de la linea de ordenes en silencio. Se descubrio probando a
 * romperlo a proposito: compilando libpeer con
 *
 *     -D__BYTE_ORDER=__LITTLE_ENDIAN
 *
 * -- que es literalmente lo que lleva el CMakeLists.txt de libpeer en su
 * rama de ESP32, o sea el copy-paste que uno hace cuando <endian.h> no
 * compila-- el resultado era: compila, sin un aviso, y __BYTE_ORDER acababa
 * valiendo __BIG_ENDIAN de todos modos.
 *
 * O sea que salia bien. Pero salia bien POR ACCIDENTE, dependiendo de que
 * algo incluyera esta cabecera y de que lo hiciera despues. Y las
 * aserciones estaticas de rtp.h y rtcp.h, escritas justamente para cazar
 * ese caso, no llegaban a saltar nunca.
 *
 * Ahora se contesta donde se sabe la verdad. Esta cabecera es la unica que
 * conoce el orden de bytes real -- lo saca de lo que predefine ppu-gcc-- y
 * si alguien le lleva la contraria, se para. Un desacuerdo aqui no es algo
 * que se pueda resolver eligiendo: es que uno de los dos esta equivocado, y
 * el que trae datos es este fichero. */
#if defined(__BYTE_ORDER)
#if __BYTE_ORDER != __BIG_ENDIAN
#error "alguien ha definido __BYTE_ORDER como algo que no es big-endian -- probablemente un -D__BYTE_ORDER=__LITTLE_ENDIAN copiado de la rama de ESP32 de libpeer. La PS3 es big-endian: quitalo."
#endif
#else
#define __BYTE_ORDER  __BIG_ENDIAN
#endif

/* Los nombres sin guiones bajos, que son los que usa la familia BSD.
 *
 * Van con guarda y ANTES de comprobar BYTE_ORDER: si se comprobara primero
 * -- `#if BYTE_ORDER != BIG_ENDIAN`-- y BIG_ENDIAN aun no estuviera
 * definido, el preprocesador lo tomaria como cero y la comparacion diria
 * que hay conflicto donde no lo hay. Es el mismo mecanismo por el que
 * netinet/ip.h compilaba sus dos ramas a la vez: en un #if, un
 * identificador desconocido vale 0. */
#ifndef LITTLE_ENDIAN
#define LITTLE_ENDIAN __LITTLE_ENDIAN
#endif
#ifndef BIG_ENDIAN
#define BIG_ENDIAN    __BIG_ENDIAN
#endif
#ifndef PDP_ENDIAN
#define PDP_ENDIAN    __PDP_ENDIAN
#endif

#if defined(BYTE_ORDER)
#if BYTE_ORDER != __BIG_ENDIAN
#error "BYTE_ORDER (sin guiones bajos) ya venia definido y no dice big-endian"
#endif
#else
#define BYTE_ORDER    __BYTE_ORDER
#endif

/* En big-endian el orden de red y el del anfitrion coinciden, asi que
 * todas estas son la identidad. No es un atajo perezoso: es lo que
 * significan. */
#define htobe16(x) (x)
#define htobe32(x) (x)
#define htobe64(x) (x)
#define be16toh(x) (x)
#define be32toh(x) (x)
#define be64toh(x) (x)

#endif /* GR33N_ENDIAN_H */
