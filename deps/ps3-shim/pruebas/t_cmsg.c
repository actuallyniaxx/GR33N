/* GR33N - prueba de las macros que ps3-shim anade cuando la consola no las
 * trae: la familia CMSG_* y timercmp/timeradd/timersub.
 *
 * Se compila contra una consola FALSA (/tmp/consola) que tiene struct
 * cmsghdr, struct msghdr y struct timeval pero NINGUNA de las macros. Es la
 * situacion que creemos que hay en la PS3, y la que la glibc del PC tapa
 * porque ahi si estan.
 *
 * Corre en el PC, nativo. No prueba la PS3: prueba que la aritmetica de las
 * macros es la de BSD y que recorrer una lista de mensajes auxiliares con
 * ellas no se sale del buffer.
 */

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <errno.h>

static int fallos;

#define OK(cond, ...) do { \
	if (cond) { \
		printf("   bien  "); \
	} else { \
		printf("   MAL   "); \
		fallos++; \
	} \
	printf(__VA_ARGS__); \
	printf("\n"); \
} while (0)

int main(void)
{
	unsigned char buf[256];
	struct msghdr msg;
	struct cmsghdr *c;
	size_t cab = CMSG_ALIGN(sizeof(struct cmsghdr));
	int cuantos;
	unsigned int datos_a, datos_b;

	printf("== quien pone las macros ==\n");
#ifdef GR33N_CMSG_PUESTAS_AQUI
	printf("   CMSG_*: ps3-shim (que es lo que se quiere probar)\n");
#else
	printf("   CMSG_*: la consola falsa -- la prueba NO vale\n");
	return 1;
#endif
#ifdef GR33N_TIMERCMP_PUESTA_AQUI
	printf("   timer*: ps3-shim\n");
#else
	printf("   timer*: la consola falsa -- la prueba NO vale\n");
	return 1;
#endif

	printf("\n== aritmetica ==\n");

	/* La alineacion es la de BSD: al tamaño de long. */
	OK(CMSG_ALIGN(0) == 0, "CMSG_ALIGN(0) = %u", (unsigned)CMSG_ALIGN(0));
	OK(CMSG_ALIGN(1) == sizeof(long), "CMSG_ALIGN(1) = %u (long mide %u)",
	   (unsigned)CMSG_ALIGN(1), (unsigned)sizeof(long));
	OK(CMSG_ALIGN(sizeof(long)) == sizeof(long),
	   "CMSG_ALIGN(sizeof long) no crece");

	/* LEN no alinea el dato; SPACE si. Esa es toda la diferencia entre
	 * las dos, y confundirlas es el error clasico: reservar con LEN deja
	 * el siguiente cabecero sin sitio para su relleno. */
	OK(CMSG_LEN(4) == cab + 4, "CMSG_LEN(4) = %u", (unsigned)CMSG_LEN(4));
	OK(CMSG_SPACE(4) == cab + CMSG_ALIGN(4), "CMSG_SPACE(4) = %u",
	   (unsigned)CMSG_SPACE(4));
	OK(CMSG_SPACE(4) >= CMSG_LEN(4), "SPACE nunca es menor que LEN");

	/* CMSG_DATA cae justo detras del cabecero alineado. */
	memset(buf, 0, sizeof(buf));
	c = (struct cmsghdr *)buf;
	OK((unsigned char *)CMSG_DATA(c) == buf + cab,
	   "CMSG_DATA apunta %u bytes despues del cabecero",
	   (unsigned)((unsigned char *)CMSG_DATA(c) - buf));

	printf("\n== recorrer una lista de dos ==\n");

	/* Se montan dos mensajes auxiliares como los monta sctp_indata.c y
	 * se recorren como los recorre sctp_output.c. */
	memset(buf, 0xAA, sizeof(buf));
	memset(&msg, 0, sizeof(msg));
	msg.msg_control = buf;
	msg.msg_controllen = (socklen_t)(CMSG_SPACE(4) + CMSG_SPACE(12));

	c = CMSG_FIRSTHDR(&msg);
	OK(c == (struct cmsghdr *)buf, "el primero es el principio del buffer");
	c->cmsg_len = (socklen_t)CMSG_LEN(4);
	c->cmsg_level = 132;   /* IPPROTO_SCTP */
	c->cmsg_type = 1;
	datos_a = 0x11223344u;
	memcpy(CMSG_DATA(c), &datos_a, 4);

	c = (struct cmsghdr *)(buf + CMSG_SPACE(4));
	c->cmsg_len = (socklen_t)CMSG_LEN(12);
	c->cmsg_level = 132;
	c->cmsg_type = 2;
	datos_b = 0x55667788u;
	memcpy(CMSG_DATA(c), &datos_b, 4);

	cuantos = 0;
	for (c = CMSG_FIRSTHDR(&msg); c != NULL; c = CMSG_NXTHDR(&msg, c)) {
		unsigned char *fin = (unsigned char *)c + c->cmsg_len;
		OK(fin <= buf + msg.msg_controllen,
		   "el mensaje %d cabe entero en el buffer", cuantos);
		cuantos++;
		if (cuantos > 8) {
			printf("   MAL   CMSG_NXTHDR no termina nunca\n");
			fallos++;
			break;
		}
	}
	OK(cuantos == 2, "salen %d mensajes (se pusieron 2)", cuantos);

	/* Y que NO se pase del final: con el buffer justo, el segundo
	 * NXTHDR tiene que devolver NULL y no una direccion de mas alla. */
	msg.msg_controllen = (socklen_t)CMSG_SPACE(4);
	c = CMSG_FIRSTHDR(&msg);
	c->cmsg_len = (socklen_t)CMSG_LEN(4);
	OK(CMSG_NXTHDR(&msg, c) == NULL,
	   "con sitio para uno solo, no se inventa un segundo");

	/* Y con el buffer mas corto que un cabecero, ni el primero. */
	msg.msg_controllen = (socklen_t)(sizeof(struct cmsghdr) - 1);
	OK(CMSG_FIRSTHDR(&msg) == NULL,
	   "sin sitio ni para el cabecero, FIRSTHDR da NULL");

	printf("\n== timercmp / timeradd / timersub ==\n");
	{
		struct timeval a, b, r;

		a.tv_sec = 10; a.tv_usec = 500000;
		b.tv_sec = 10; b.tv_usec = 500001;
		OK(timercmp(&a, &b, <), "10.500000 < 10.500001");
		OK(!timercmp(&a, &b, >), "y no es mayor");
		b.tv_sec = 9;
		OK(timercmp(&a, &b, >), "10.5 > 9.500001 (manda el segundero)");

		/* El acarreo, que es donde se equivoca uno al escribirlas de
		 * memoria. */
		a.tv_sec = 1; a.tv_usec = 800000;
		b.tv_sec = 2; b.tv_usec = 300000;
		timeradd(&a, &b, &r);
		OK(r.tv_sec == 4 && r.tv_usec == 100000,
		   "1.8 + 2.3 = %ld.%06ld", r.tv_sec, r.tv_usec);

		a.tv_sec = 4; a.tv_usec = 100000;
		b.tv_sec = 2; b.tv_usec = 300000;
		timersub(&a, &b, &r);
		OK(r.tv_sec == 1 && r.tv_usec == 800000,
		   "4.1 - 2.3 = %ld.%06ld", r.tv_sec, r.tv_usec);

		/* Y el caso del que depende sctp_timer.c:534, que justo
		 * despues del timersub comprueba `tv_sec < 0 || tv_usec < 0`:
		 * un resultado negativo tiene que quedar con los segundos en
		 * negativo y los microsegundos normalizados. */
		a.tv_sec = 1; a.tv_usec = 0;
		b.tv_sec = 3; b.tv_usec = 500000;
		timersub(&a, &b, &r);
		OK(r.tv_sec < 0, "1.0 - 3.5 deja los segundos negativos (%ld.%06ld)",
		   r.tv_sec, r.tv_usec);
		OK(r.tv_usec >= 0 && r.tv_usec < 1000000,
		   "y los microsegundos normalizados");
	}

	printf("\n== las constantes sueltas ==\n");
	OK(SOCK_SEQPACKET != SOCK_STREAM && SOCK_SEQPACKET != SOCK_DGRAM,
	   "SOCK_SEQPACKET (%d) no choca con STREAM (%d) ni DGRAM (%d)",
	   SOCK_SEQPACKET, SOCK_STREAM, SOCK_DGRAM);
	OK(IPPORT_RESERVED > 0, "IPPORT_RESERVED = %d", IPPORT_RESERVED);
	OK(ERESTART < 0, "ERESTART = %d (negativo, no choca con ningun errno)",
	   ERESTART);
	OK(ERESTART != EINTR && ERESTART != EWOULDBLOCK,
	   "y es distinto de EINTR y EWOULDBLOCK, que se comparan al lado");

	printf("\n%s (%d fallos)\n", fallos ? "HAY FALLOS" : "todo bien", fallos);
	return fallos != 0;
}
