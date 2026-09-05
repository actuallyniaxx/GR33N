/* GR33N - envoltura del <errno.h> de newlib, para una sola constante.
 *
 * NO SUSTITUYE A LA DE LA CONSOLA: la envuelve con #include_next. Todos los
 * errno de verdad -EAGAIN, EINVAL, ENOMEM- los sigue poniendo newlib.
 *
 * Lo unico que se anade es ERESTART, que usrsctp mira en user_socket.c:1027:
 *
 *     if ((auio.uio_resid != ulen) &&
 *         (error == EINTR || error == ERESTART || error == EWOULDBLOCK))
 *             error = 0;
 *
 * ERESTART no es un errno de POSIX: es una senal INTERNA de los nucleos BSD
 * que quiere decir "la llamada se interrumpio, reintentala", y nunca sale
 * al programa de usuario. Por eso newlib no lo trae y por eso usrsctp se lo
 * define el mismo... pero solo para las plataformas que reconoce
 * (user_socketvar.h:57).
 *
 * EL VALOR ES EL DE usrsctp, -1, Y ESO ESTA BIEN A PROPOSITO. No tiene que
 * coincidir con ningun nucleo: en el camino de espacio de usuario, el unico
 * que puede devolver ERESTART es el propio usrsctp, y el unico que lo mira
 * es esta linea. Un numero negativo ademas garantiza que no choca con
 * ningun errno de newlib, que son todos positivos.
 *
 * POR QUE AQUI Y NO ENSANCHANDO EL GUARDA DE user_socketvar.h: ese #if
 * define ERESTART y UIO_MAXIOV a la vez, y UIO_MAXIOV ya lo pone
 * ps3-shim/sys/uio.h con otro cuerpo (IOV_MAX). Serian veintitres avisos de
 * redefinicion para ahorrarse este fichero.
 */

#ifndef GR33N_ERRNO_H
#define GR33N_ERRNO_H

/* La de newlib, primero y entera. */
#include_next <errno.h>

#ifndef ERESTART
#define ERESTART (-1)
#endif

#endif /* GR33N_ERRNO_H */
