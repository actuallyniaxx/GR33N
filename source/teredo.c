/* GR33N - Teredo y lineas de candidato. Ver include/teredo.h.
 *
 * Nada de aqui toca la red ni PSL1GHT: se compila y se ejecuta igual en
 * el PC, que es donde se prueba (t/teredo.c).
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "teredo.h"

/* --------------------------------------------------------------------- */

/* Un grupo hexadecimal de la direccion, sin usar strtoul.
 *
 * strtoul se traga "0x", el signo y los espacios de delante, y para hasta
 * donde entiende. Aqui eso no vale: "12g4" tiene que ser un fallo, no un
 * 0x12 con la cola tirada. Un analizador que acepta de mas es la forma
 * mas comoda de acabar mandando comprobaciones de conectividad a una
 * direccion inventada. */
static int hex_grupo(const char *s, size_t n, unsigned *fuera)
{
	unsigned v = 0;
	size_t i;

	if (n == 0 || n > 4) return 0;

	for (i = 0; i < n; i++) {
		unsigned d;
		char c = s[i];

		if      (c >= '0' && c <= '9') d = (unsigned)(c - '0');
		else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
		else return 0;

		v = (v << 4) | d;
	}

	*fuera = v;
	return 1;
}

int teredoDecode(const char *addr6, char *ipv4, size_t ipv4_n, int *port)
{
	const char *g[8];
	size_t len[8];
	unsigned val[8];
	const char *p;
	int i, n = 0;

	if (addr6 == NULL || ipv4 == NULL || port == NULL || ipv4_n < 16)
		return 0;

	/* Ocho grupos separados por ':', en forma EXPANDIDA.
	 *
	 * La forma comprimida con "::" se rechaza a proposito: expandirla
	 * bien es otro analizador entero, y xCloud manda la expandida. Si
	 * algun dia manda la otra, esto devuelve 0 y quien llama lo escribe
	 * en el log, que es como queremos enterarnos -- y no con una
	 * direccion medio adivinada. */
	p = addr6;
	for (;;) {
		const char *c = strchr(p, ':');

		if (n >= 8) return 0;
		g[n]   = p;
		len[n] = c ? (size_t)(c - p) : strlen(p);
		n++;

		if (!c) break;
		p = c + 1;
	}
	if (n != 8) return 0;

	for (i = 0; i < 8; i++)
		if (!hex_grupo(g[i], len[i], &val[i])) return 0;

	/* El prefijo de Teredo es 2001:0000::/32: LOS DOS grupos.
	 *
	 * green-nx solo mira el primero. Comprobar tambien el segundo rechaza
	 * direcciones que NO son Teredo y que, decodificadas de todas formas,
	 * darian una IPv4 cualquiera -- y a esa le mandariamos comprobaciones
	 * de conectividad durante quince segundos antes de rendirnos. Es mas
	 * estricto que la referencia, y a proposito. */
	if (val[0] != 0x2001 || val[1] != 0x0000) return 0;

	/* El puerto y la IPv4 del cliente van con todos los bits invertidos.
	 * No es cifrado: es para que no aparezcan literales que algun NAT
	 * quisiera reescribir por su cuenta. */
	*port = (int)((~val[5]) & 0xFFFFu);

	{
		unsigned a = (~(val[6] >> 8))   & 0xFFu;
		unsigned b = (~(val[6]))        & 0xFFu;
		unsigned c = (~(val[7] >> 8))   & 0xFFu;
		unsigned d = (~(val[7]))        & 0xFFu;

		snprintf(ipv4, ipv4_n, "%u.%u.%u.%u", a, b, c, d);
	}

	return 1;
}

/* --------------------------------------------------------------------- */

int candNormaliza(const char *linea, char *fuera, size_t fuera_n)
{
	size_t n;

	if (linea == NULL || fuera == NULL || fuera_n == 0) return 0;

	while (*linea == ' ' || *linea == '\t') linea++;

	if (strncmp(linea, "a=", 2) == 0) linea += 2;

	if (strncmp(linea, "candidate:", 10) != 0) return 0;

	n = strlen(linea);
	while (n > 0 && (linea[n - 1] == ' '  || linea[n - 1] == '\r' ||
	                 linea[n - 1] == '\n' || linea[n - 1] == '\t'))
		n--;

	/* Que no quepa es un FALLO, no un motivo para recortar. Un candidato
	 * a medias tiene forma de candidato y apunta a otro sitio. */
	if (n == 0 || n >= fuera_n) return 0;

	memcpy(fuera, linea, n);
	fuera[n] = '\0';
	return 1;
}

/* Devuelve el campo `idx` (contando desde 0) de una linea separada por
 * espacios, o NULL. */
static const char *campo(const char *s, int idx, size_t *len)
{
	int i;

	for (i = 0; ; i++) {
		const char *fin;

		while (*s == ' ') s++;
		if (*s == '\0') return NULL;

		fin = s;
		while (*fin && *fin != ' ') fin++;

		if (i == idx) {
			*len = (size_t)(fin - s);
			return s;
		}
		s = fin;
	}
}

int candDireccion(const char *cand, char *dir, size_t dir_n, int *puerto)
{
	const char *c;
	size_t n;

	if (cand == NULL || dir == NULL || puerto == NULL) return 0;

	c = campo(cand, 4, &n);
	if (c == NULL || n == 0 || n >= dir_n) return 0;
	memcpy(dir, c, n);
	dir[n] = '\0';

	c = campo(cand, 5, &n);
	if (c == NULL || n == 0) return 0;
	{
		unsigned long v = 0;
		size_t i;

		for (i = 0; i < n; i++) {
			if (c[i] < '0' || c[i] > '9') return 0;
			v = v * 10 + (unsigned long)(c[i] - '0');
			if (v > 65535) return 0;
		}
		*puerto = (int)v;
	}

	return 1;
}

unsigned long candPrioridad(const char *cand)
{
	const char *c;
	size_t n, i;
	unsigned long v = 0;

	if (cand == NULL) return 0;

	c = campo(cand, 3, &n);
	if (c == NULL || n == 0) return 0;

	for (i = 0; i < n; i++) {
		if (c[i] < '0' || c[i] > '9') return 0;
		v = v * 10 + (unsigned long)(c[i] - '0');
	}

	return v;
}
