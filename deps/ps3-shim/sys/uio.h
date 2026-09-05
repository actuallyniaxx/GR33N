/* GR33N - sustituto minimo de <sys/uio.h> para PSL1GHT.
 *
 * LA PS3 TIENE struct iovec PERO NO TIENE sys/uio.h. Esa combinacion es
 * rara y costo dos vueltas entenderla:
 *
 *   1. Sin este fichero, los 23 ficheros de usrsctp fallan con
 *      "sys/uio.h: No such file or directory". El fichero no existe.
 *   2. Con este fichero definiendo struct iovec, fallan con "redefinition
 *      of struct iovec". El TIPO si existe, lo define algo que incluye el
 *      <sys/socket.h> de PSL1GHT.
 *
 * O sea que lo que falta es el FICHERO, no el tipo. Y como PSL1GHT lo
 * define sin usar ninguna de las macros que suelen marcarlo -ni
 * __iovec_defined, ni _STRUCT_IOVEC, ni _SYS_UIO_H_-, desde el
 * preprocesador no hay forma de preguntar si un struct ya existe.
 *
 * Asi que se pregunta desde fuera: build-usrsctp.sh compila un programa de
 * tres lineas que usa struct iovec y, si cuela, pasa -DGR33N_HAVE_IOVEC=1.
 * Mismo patron que la sonda de pthreads, y por el mismo motivo: comprobar
 * sale mas barato que suponer.
 *
 * LO UNICO QUE HACE FALTA DE VERDAD es que el fichero exista y traiga
 * UIO_MAXIOV. En todo usrsctplib no hay UNA sola llamada a readv, writev,
 * preadv ni pwritev: iovec se usa solo como estructura de datos, para
 * pasar listas de trozos a usrsctp_sendv/recvv y recorrerlas con memcpy en
 * user_socket.c (lineas 612-639).
 *
 * ORDEN DE INCLUSION. Este fichero NO incluye <sys/socket.h> para
 * asegurarse el tipo, y es a proposito: seria una dependencia circular con
 * la cabecera que nos define. usrsctp siempre incluye socket.h antes que
 * uio.h -- se ve en las trazas del compilador: sctp_os_userspace.h:293
 * mete socket.h y :439 mete user_socketvar.h, que es quien pide uio.h. Si
 * eso dejara de ser cierto algun dia, el error seria un claro "unknown
 * type name 'iovec'" y no un fallo silencioso.
 */

#ifndef GR33N_SYS_UIO_H
#define GR33N_SYS_UIO_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GR33N_HAVE_IOVEC lo pone build-usrsctp.sh tras comprobarlo de verdad.
 * Los otros tres guardas son para poder compilar esta misma cabecera en el
 * PC con un cruzado de PowerPC, donde la glibc la marca de forma normal. */
#if !defined(GR33N_HAVE_IOVEC) && !defined(__iovec_defined) && \
    !defined(_STRUCT_IOVEC) && !defined(_SYS_UIO_H_) && !defined(_UIO_H_)
struct iovec {
	void  *iov_base;   /* donde empieza el trozo */
	size_t iov_len;    /* cuanto mide            */
};
#define __iovec_defined 1
#endif

/* El tope de trozos por operacion.
 *
 * UIO_MAXIOV es el nombre BSD y es el que usa usrsctp de verdad
 * (user_socket.c:585, para rechazar listas absurdamente largas antes de
 * recorrerlas). IOV_MAX es el nombre POSIX del mismo numero. Se ponen los
 * dos porque no cuesta nada y porque adivinar cual pide una biblioteca
 * ajena ya nos ha costado un ciclo esta semana.
 *
 * PSL1GHT define el tipo pero no estas constantes, asi que aunque el
 * struct venga de la consola, ESTAS lineas siguen haciendo falta. Es la
 * razon por la que el fichero no puede quedarse vacio. */
#ifndef IOV_MAX
#define IOV_MAX 1024
#endif

#ifndef UIO_MAXIOV
#define UIO_MAXIOV IOV_MAX
#endif

/* readv y writev NO se declaran a proposito.
 *
 * Declararlas seria prometer algo que no existe, y el fallo se moveria del
 * compilador al enlazador o, peor, a la consola. Si hicieran falta, hay que
 * escribirlas sobre netRecv/netSend de lv2 y saber que se esta haciendo. */

#ifdef __cplusplus
}
#endif

#endif /* GR33N_SYS_UIO_H */
