/* GR33N - envoltura del <sys/time.h> de PSL1GHT.
 *
 * NO SUSTITUYE A LA DE LA CONSOLA: la envuelve con #include_next, igual que
 * sys/socket.h y netinet/in.h. struct timeval, gettimeofday y todo lo demas
 * lo sigue poniendo newlib; aqui solo se anaden las TRES MACROS DE BSD que
 * usrsctp usa en nueve sitios y esta newlib no trae:
 *
 *     sctp_timer.c:529     timersub(&now, &tv, &min_wait);
 *     sctp_timer.c:626     timercmp(&now, &chk->rec.data.timetodrop, >)
 *     sctp_timer.c:1467    timersub(&now, &net->last_sent_time, &diff);
 *     sctp_input.c:2715    timercmp(&now, &time_entered, <)
 *     sctp_input.c:2729    timercmp(&now, &time_expires, >)
 *     sctp_input.c:2752    timersub(&now, &time_expires, &diff);
 *     sctp_indata.c:3358   timercmp(&now, &tp1->rec.data.timetodrop, >)
 *     sctp_indata.c:3781   (igual)
 *     sctp_output.c:6946   timeradd(&sp->ts, &tv, &sp->ts);
 *
 * ATENCION AL ERROR QUE PRODUCEN CUANDO FALTAN, porque no es el que parece.
 *
 * timercmp se usa asi:  timercmp(&a, &b, >)
 *
 * Si la macro no existe, el preprocesador deja la llamada tal cual y el
 * compilador se encuentra un `>` suelto como tercer argumento de una
 * funcion. El mensaje que salio fue:
 *
 *     error: expected expression before '>' token
 *
 * ...que no menciona timercmp por ningun lado y parece un fallo de sintaxis
 * en el codigo de usrsctp. El aviso de verdad -"implicit declaration of
 * function 'timercmp'"- iba TRES LINEAS MAS ARRIBA y era un aviso, no un
 * error. Ahi esta la trampa de esta plataforma: una macro que falta se
 * disfraza de otra cosa.
 *
 * Las tres estan copiadas de FreeBSD (sys/sys/time.h) tal cual, incluida la
 * normalizacion del acarreo de microsegundos. NO se escriben "mas simples":
 * sctp_timer.c:534 comprueba `min_wait.tv_sec < 0 || min_wait.tv_usec < 0`
 * justo despues de un timersub, o sea que depende de que el resto quede
 * normalizado como manda BSD.
 */

#ifndef GR33N_SYS_TIME_H
#define GR33N_SYS_TIME_H

/* La de la consola, primero y entera. */
#include_next <sys/time.h>

/* --------------------------------------------------------------------- */

#ifndef timercmp
#define GR33N_TIMERCMP_PUESTA_AQUI 1
#define timercmp(tvp, uvp, cmp)                                 \
	(((tvp)->tv_sec == (uvp)->tv_sec) ?                     \
	    ((tvp)->tv_usec cmp (uvp)->tv_usec) :               \
	    ((tvp)->tv_sec cmp (uvp)->tv_sec))
#endif

#ifndef timeradd
#define timeradd(tvp, uvp, vvp)                                 \
	do {                                                    \
		(vvp)->tv_sec = (tvp)->tv_sec + (uvp)->tv_sec;  \
		(vvp)->tv_usec = (tvp)->tv_usec + (uvp)->tv_usec; \
		if ((vvp)->tv_usec >= 1000000) {                \
			(vvp)->tv_sec++;                        \
			(vvp)->tv_usec -= 1000000;              \
		}                                               \
	} while (0)
#endif

#ifndef timersub
#define timersub(tvp, uvp, vvp)                                 \
	do {                                                    \
		(vvp)->tv_sec = (tvp)->tv_sec - (uvp)->tv_sec;  \
		(vvp)->tv_usec = (tvp)->tv_usec - (uvp)->tv_usec; \
		if ((vvp)->tv_usec < 0) {                       \
			(vvp)->tv_sec--;                        \
			(vvp)->tv_usec += 1000000;              \
		}                                               \
	} while (0)
#endif

/* timerclear, timerisset y timevalsub NO van aqui: usrsctp trae las suyas
 * con #ifndef en sctp_os_userspace.h (linea 1114 y siguientes). Ponerlas
 * tambien aqui seria definir la misma macro con un cuerpo distinto, que da
 * aviso de redefinicion en los veintitres ficheros. */

#endif /* GR33N_SYS_TIME_H */
