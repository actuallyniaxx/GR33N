/* Prueba de sobremesa del armado de la peticion HTTP de net_tls.c.
 *
 * El fallo: snprintf devuelve LO QUE HABRIA ESCRITO, no lo que escribio, y
 * las cuatro llamadas se encadenaban con
 *
 *     n += snprintf(req + n, sizeof(req) - (size_t)n, ...);
 *
 * dejando la comprobacion de que cabia DETRAS de las cuatro. En cuanto la
 * primera se pasaba, `n` era mayor que el buffer, `req + n` apuntaba fuera,
 * y `sizeof(req) - n` daba la vuelta y se convertia en un numero enorme.
 * Escritura libre en memoria estatica, con un mensaje de error educado
 * detras explicando que no cabia.
 *
 * Era alcanzable: auth.c monta una cabecera de 20 KB con el XSTS dentro.
 *
 * UN MATIZ QUE CASI ME COMO ENTERO. Esta prueba, con el codigo viejo,
 * PASA en el PC. No porque el codigo estuviera bien, sino porque glibc
 * rechaza cualquier snprintf con un tamaño mayor que INT_MAX: devuelve -1
 * sin escribir nada, y como `sizeof(req) - n` con n pasado de rosca siempre
 * da un numero gigante, glibc tapaba el fallo ella sola.
 *
 * La PS3 no lleva glibc, lleva newlib. Con un snprintf que no haga esa
 * comprobacion -que es lo que hace una implementacion ingenua- el codigo
 * viejo escribe fuera: comprobado aparte con ASan, "heap-buffer-overflow,
 * WRITE of size 2, 37 bytes after 512-byte region".
 *
 * O sea: no es "esto petaba en la consola", es "esto dependia de una
 * costumbre de la biblioteca de C que nadie prometio". Que para una
 * escritura sin limites en memoria estatica es exactamente igual de malo.
 *
 * Por eso esta prueba NO intenta reproducir el desbordamiento: en el PC no
 * puede. Lo que comprueba es que la version nueva se planta en el borde
 * exacto, en los dos lados, y que ningun camino devuelve un tamaño mayor
 * que el buffer. La logica esta copiada de do_request() a mano.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef unsigned u32;

static int fails = 0;

/* Como el de verdad, pero pequeño para poder desbordarlo a mano. Los
 * centinelas de alrededor son lo que delataria una escritura fuera si ASan
 * se distrajera. */
#define REQ_MAX 512

static struct {
	char antes[64];
	char req[REQ_MAX];
	char despues[64];
} m;

/* Copia literal del bloque de do_request(), con el buffer parametrizado. */
static int armar(const char *method, const char *path, const char *host,
                 const char *ctype, const char *hdrs, const char *body,
                 int *fallo)
{
	char *req = m.req;
	size_t blen = body ? strlen(body) : 0;
	int n;

	*fallo = 0;

	#define PON(...) do { \
		int _k = snprintf(req + n, REQ_MAX - (size_t)n, __VA_ARGS__); \
		if (_k < 0 || (size_t)_k >= REQ_MAX - (size_t)n) { \
			*fallo = 1; \
			return -1; \
		} \
		n += _k; \
	} while (0)

	n = 0;

	PON("%s %s HTTP/1.1\r\n"
	    "Host: %s\r\n"
	    "User-Agent: GR33N/x (PlayStation 3)\r\n"
	    "Accept: */*\r\n"
	    "Connection: close\r\n",
	    method, path, host);

	if (ctype && blen)
		PON("Content-Type: %s\r\n"
		    "Content-Length: %u\r\n",
		    ctype, (unsigned)blen);

	if (hdrs && hdrs[0])
		PON("%s", hdrs);

	PON("\r\n");

	#undef PON

	if ((size_t)n + blen >= REQ_MAX) { *fallo = 1; return -1; }

	if (blen) memcpy(req + n, body, blen);

	return n + (int)blen;
}

static void centinelas(const char *que)
{
	u32 i;

	for (i = 0; i < sizeof(m.antes); i++)
		if (m.antes[i] != 0x5a) {
			printf("  FALLO %-45s -> pisado ANTES del buffer\n", que);
			fails++;
			return;
		}

	for (i = 0; i < sizeof(m.despues); i++)
		if (m.despues[i] != 0x5a) {
			printf("  FALLO %-45s -> pisado DESPUES del buffer\n", que);
			fails++;
			return;
		}
}

static void limpiar(void)
{
	memset(m.antes,   0x5a, sizeof(m.antes));
	memset(m.req,     0,    sizeof(m.req));
	memset(m.despues, 0x5a, sizeof(m.despues));
}

static void caso(const char *que, const char *hdrs, const char *body,
                 int debe_caber)
{
	int fallo, n;

	limpiar();
	n = armar("GET", "/v5/sessions/cloud/play", "uks.core.gssv.xboxlive.com",
	          body ? "application/json" : NULL, hdrs, body, &fallo);

	centinelas(que);

	if (debe_caber && fallo) {
		printf("  FALLO %-45s -> dijo que no cabe y si cabia\n", que);
		fails++;
	} else if (!debe_caber && !fallo) {
		printf("  FALLO %-45s -> dijo que cabe (%d) y NO cabia\n", que, n);
		fails++;
	} else if (!fallo && (n < 0 || n >= REQ_MAX)) {
		printf("  FALLO %-45s -> devolvio %d con buffer de %d\n",
		       que, n, REQ_MAX);
		fails++;
	} else {
		printf("  ok   %-46s -> %s\n", que,
		       fallo ? "no cabe, y lo dice" : "cabe");
	}
}

int main(void)
{
	char *grande;

	printf("armado de la peticion HTTP\n");

	caso("peticion normal, sin cabeceras extra", NULL, NULL, 1);

	caso("con Authorization corriente",
	     "Authorization: Bearer aaaabbbbccccdddd\r\n"
	     "x-gssv-client: XboxComBrowser\r\n", NULL, 1);

	caso("con cuerpo corto",
	     "Authorization: Bearer aaaa\r\n", "{\"titleId\":\"CRASH\"}", 1);

	/* EL CASO DEL DESBORDAMIENTO: una cabecera mas larga que el buffer
	 * entero. La version vieja escribia fuera y luego se quejaba. */
	grande = malloc(REQ_MAX * 3);
	if (!grande) return 2;
	memset(grande, 'A', REQ_MAX * 3 - 3);
	memcpy(grande + REQ_MAX * 3 - 3, "\r\n", 3);

	caso("cabecera mas larga que el buffer entero", grande, NULL, 0);

	/* EL BORDE, medido y no estimado.
	 *
	 * Primero se pregunta cuanto ocupa la peticion pelada, y a partir de
	 * ahi se calcula la cabecera que deja el buffer exactamente al ras.
	 * Poner un numero a ojo aqui es como se escriben pruebas que pasan por
	 * el motivo equivocado: la primera version decia 155 "porque las
	 * fijas ocupan unos 150", fallaba por diez bytes, y el que estaba mal
	 * era el numero. */
	{
		int fallo, base;
		char *justo;
		int i;

		limpiar();
		base = armar("GET", "/v5/sessions/cloud/play",
		             "uks.core.gssv.xboxlive.com", NULL, NULL, NULL, &fallo);

		if (fallo || base <= 0) {
			printf("  FALLO no se pudo medir la peticion pelada\n");
			fails++;
			free(grande);
			return 1;
		}

		printf("  (la peticion pelada ocupa %d de %d)\n", base, REQ_MAX);

		justo = malloc(REQ_MAX);
		if (!justo) { free(grande); return 2; }

		/* Uno menos del limite: tiene que caber justo. */
		for (i = 0; i < REQ_MAX - base - 1; i++) justo[i] = 'B';
		justo[REQ_MAX - base - 1] = '\0';
		caso("cabecera de un byte menos del limite", justo, NULL, 1);

		/* Y el limite exacto: aqui el que no cabe es el "\r\n" final, que
		 * es el trozo que la version vieja escribia ya fuera. */
		for (i = 0; i < REQ_MAX - base; i++) justo[i] = 'B';
		justo[REQ_MAX - base] = '\0';
		caso("cabecera justo en el limite", justo, NULL, 0);

		free(justo);
	}

	/* Un cuerpo que solo se pasa por los ultimos bytes: la cabecera cabe,
	 * el conjunto no. Es el caso que la comprobacion final tiene que
	 * cazar, porque el memcpy del cuerpo no pasa por PON. */
	{
		char *cuerpo = malloc(REQ_MAX);
		int i;

		if (!cuerpo) { free(grande); return 2; }
		for (i = 0; i < REQ_MAX - 100; i++) cuerpo[i] = 'C';
		cuerpo[REQ_MAX - 100] = '\0';

		caso("cabecera cabe pero cabecera+cuerpo no", NULL, cuerpo, 0);
		free(cuerpo);
	}

	free(grande);

	printf("%s\n", fails ? "HAY FALLOS" : "todo bien");
	return fails ? 1 : 0;
}
