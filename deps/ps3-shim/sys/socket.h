/* GR33N - envoltura del <sys/socket.h> de PSL1GHT.
 *
 * NO SUSTITUYE A LA DE LA CONSOLA: la envuelve con #include_next, igual que
 * netinet/in.h y por el mismo motivo. PSL1GHT trae sys/socket.h y funciona;
 * lo que le faltan son unos cuantos tipos que usrsctp declara sin guardar.
 *
 * El primero que salio fue struct sockaddr_storage, en sctp_uio.h:348 y
 * :574, dentro de estructuras que se reservan enteras. Compilar sin IPv6 y
 * sin sockets no evita esas lineas: solo evita el codigo que las mira.
 *
 * Lo que hay aqui se pone SOLO si la sonda dice que falta. Ver
 * deps/sonda-tipos.sh, que pregunta por los 33 de golpe -- llevabamos siete
 * vueltas descubriendo uno por compilacion, y cada vuelta cuesta un ciclo
 * entero de la maquina de otro.
 */

#ifndef GR33N_SYS_SOCKET_H
#define GR33N_SYS_SOCKET_H

/* La de la consola, primero y entera. */
#include_next <sys/socket.h>

#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------- */
/* struct sockaddr_storage                                               */
/* --------------------------------------------------------------------- */

/* Copiado literal de FreeBSD (sys/sys/_sockaddr_storage.h), no escrito de
 * memoria. LA DISPOSICION IMPORTA: RFC 2553 dice 128 bytes y alineado a 8,
 * y usrsctp reserva y copia estas estructuras por valor. Un tamaño distinto
 * del que espera su propio codigo es corrupcion de memoria silenciosa, que
 * es exactamente el fallo del sockaddr_conn que green-nx documento.
 *
 * ss_len va delante porque la newlib de PSL1GHT es de la familia BSD -- lo
 * confirmo la sonda: struct sockaddr_in tiene sin_len. */
#ifndef GR33N_HAVE_SS

#define _SS_MAXSIZE   128U
#define _SS_ALIGNSIZE (sizeof(int64_t))
#define _SS_PAD1SIZE  (_SS_ALIGNSIZE - sizeof(unsigned char) - \
                       sizeof(sa_family_t))
#define _SS_PAD2SIZE  (_SS_MAXSIZE - sizeof(unsigned char) - \
                       sizeof(sa_family_t) - _SS_PAD1SIZE - _SS_ALIGNSIZE)

struct sockaddr_storage {
	unsigned char ss_len;      /* longitud de la direccion */
	sa_family_t   ss_family;   /* familia                  */
	char          __ss_pad1[_SS_PAD1SIZE];
	int64_t       __ss_align;  /* fuerza la alineacion     */
	char          __ss_pad2[_SS_PAD2SIZE];
};

#endif /* !GR33N_HAVE_SS */

/* --------------------------------------------------------------------- */
/* SOCK_SEQPACKET                                                        */
/* --------------------------------------------------------------------- */

/* sctp_pcb.c:2701 compara el tipo del socket con SOCK_SEQPACKET para
 * decidir si es un socket "estilo UDP" (uno a muchos) o "estilo TCP" (uno a
 * uno). PSL1GHT solo trae los tipos que la consola sabe abrir de verdad:
 * SOCK_STREAM, SOCK_DGRAM y SOCK_RAW.
 *
 * EL VALOR SOLO TIENE QUE SER DISTINTO DE LOS OTROS. Aqui nadie llama al
 * socket() del sistema con esto: usrsctp_socket() se lo guarda en su propio
 * so->so_type y lo compara consigo mismo. El 5 es el de BSD y el de Linux,
 * y no choca con el 1, 2 ni 3 de la consola.
 *
 * (En GR33N el canal se abre con SOCK_STREAM, que es lo que pide libpeer
 * para los canales de datos de WebRTC, asi que esta rama no se pisa nunca.
 * Pero el fichero tiene que compilar.) */
#ifndef SOCK_SEQPACKET
#define SOCK_SEQPACKET 5
#endif

/* --------------------------------------------------------------------- */
/* SOMAXCONN                                                             */
/* --------------------------------------------------------------------- */

/* El tope de conexiones en espera que acepta un listen(). usrsctp lo usa en
 * un solo sitio, user_socket.c:1517:
 *
 *     static int somaxconn = SOMAXCONN;
 *
 * ...y solo para recortar el `backlog` que le pasen a solisten(). GR33N no
 * escucha nada: abre un canal de datos WebRTC hacia el servidor de xCloud y
 * ya. Esa variable existe y no se lee jamas en nuestro camino.
 *
 * PSL1GHT no lo trae porque la consola no monta servidores. Se pone el 128
 * de Linux y BSD moderno en vez del 5 historico: si algun dia alguien SI
 * escucha por aqui, 128 es lo razonable y 5 seria una limitacion heredada
 * de 1983 que nadie recordaria haber elegido. */
#ifndef SOMAXCONN
#define SOMAXCONN 128
#endif

/* --------------------------------------------------------------------- */
/* La familia CMSG_*                                                     */
/* --------------------------------------------------------------------- */

/* PSL1GHT trae struct msghdr y struct cmsghdr -- los campos se leen y se
 * escriben sin problema-, pero NO las macros que los recorren. usrsctp las
 * usa en 49 sitios (sctp_indata.c, sctp_output.c y user_recv_thread.c) para
 * montar y leer los datos auxiliares de sendv/recvv.
 *
 * ASI FUE COMO SE ESCONDIERON. La sonda de tipos dijo que estaban:
 *
 *     probar "CMSG_DATA / SPACE / LEN"  '#include <sys/socket.h>
 *     int main(void){struct cmsghdr c; (void)CMSG_DATA(&c);
 *     return (int)(CMSG_SPACE(4)+CMSG_LEN(4));}'
 *
 * Ese programa COMPILA aunque las macros no existan. En C99 llamar a algo
 * que no esta declarado es un AVISO, no un error: el compilador se inventa
 * `int CMSG_DATA()` y sigue. La sonda miraba el codigo de salida, veia cero
 * y decia "SI". Tercera vez esta semana que una sonda mide donde no es.
 *
 * Arreglado en las dos sondas y en el build con
 * -Werror=implicit-function-declaration, que convierte ese aviso en el
 * error que siempre debio ser.
 *
 * LA ALINEACION NO TIENE QUE CUADRAR CON NINGUN NUCLEO. Estos mensajes
 * auxiliares no cruzan a lv2 jamas: los escribe usrsctp en un buffer suyo y
 * los lee usrsctp -- o GR33N- en el mismo proceso. Lo unico que importa es
 * que las seis macros sean coherentes entre si. Se usa sizeof(long), que es
 * lo que hace _ALIGN en BSD, en vez de un 4 o un 8 a pelo: el binario de
 * PSL1GHT es ELF de 32 bits sobre un PPU de 64, y ese es justo el sitio
 * donde uno se equivoca de numero. */

#ifndef CMSG_ALIGN
#define __GR33N_CMSG_ALIGNBYTES (sizeof(long) - 1)
#define CMSG_ALIGN(n) \
	(((n) + __GR33N_CMSG_ALIGNBYTES) & ~__GR33N_CMSG_ALIGNBYTES)
#define GR33N_CMSG_ALIGN_PUESTA_AQUI 1
#endif

#ifndef CMSG_DATA
#define CMSG_DATA(cmsg) \
	((unsigned char *)(cmsg) + CMSG_ALIGN(sizeof(struct cmsghdr)))
#define GR33N_CMSG_PUESTAS_AQUI 1
#endif

#ifndef CMSG_LEN
#define CMSG_LEN(l) (CMSG_ALIGN(sizeof(struct cmsghdr)) + (l))
#endif

#ifndef CMSG_SPACE
#define CMSG_SPACE(l) (CMSG_ALIGN(sizeof(struct cmsghdr)) + CMSG_ALIGN(l))
#endif

#ifndef CMSG_FIRSTHDR
#define CMSG_FIRSTHDR(mhdr) \
	((mhdr)->msg_controllen >= sizeof(struct cmsghdr) ? \
	 (struct cmsghdr *)(mhdr)->msg_control : (struct cmsghdr *)0)
#endif

/* La de BSD, con la comprobacion de desbordamiento incluida: devuelve NULL
 * si el siguiente cabecero no cabe entero en el buffer. Sin esa
 * comprobacion, un cmsg_len corrupto -o simplemente el final de la lista-
 * hace que se lea fuera del buffer. */
#ifndef CMSG_NXTHDR
#define CMSG_NXTHDR(mhdr, cmsg)                                            \
	((char *)(cmsg) == (char *)0 ? CMSG_FIRSTHDR(mhdr) :               \
	 (((char *)(cmsg) + CMSG_ALIGN((cmsg)->cmsg_len)                   \
	   + CMSG_ALIGN(sizeof(struct cmsghdr))) >                         \
	  ((char *)(mhdr)->msg_control + (mhdr)->msg_controllen)) ?        \
	  (struct cmsghdr *)0 :                                            \
	  (struct cmsghdr *)((char *)(cmsg) + CMSG_ALIGN((cmsg)->cmsg_len)))
#endif

#ifdef __cplusplus
}
#endif

#endif /* GR33N_SYS_SOCKET_H */
