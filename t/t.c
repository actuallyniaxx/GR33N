/* Banco de pruebas de las tres funciones de indices que se han escrito
 * hoy. No se puede compilar para PowerPC aqui, pero estas tres no tocan
 * nada de la PS3: son aritmetica pura y es donde se cometen los fallos. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned int u32;
typedef unsigned long long u64;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FALLO: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

/* ---- copiadas literalmente de net_tls.c ---- */

static int find_bytes(const char *hay, u32 hlen, const char *pat, u32 plen)
{
	u32 i;
	if (plen == 0 || hlen < plen) return -1;
	for (i = 0; i <= hlen - plen; i++)
		if (memcmp(hay + i, pat, plen) == 0) return (int)i;
	return -1;
}

static int hdr_has(const char *h, u32 hlen, const char *needle)
{
	u32 nlen = (u32)strlen(needle);
	u32 i, j;
	if (hlen < nlen) return 0;
	for (i = 0; i <= hlen - nlen; i++) {
		for (j = 0; j < nlen; j++) {
			char a = h[i + j], b = needle[j];
			if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
			if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
			if (a != b) break;
		}
		if (j == nlen) return 1;
	}
	return 0;
}

static int dechunk(char *p, u32 len)
{
	u32 in = 0, out = 0;
	for (;;) {
		u32 sz = 0;
		int digits = 0;
		while (in < len) {
			char c = p[in];
			int v;
			if (c >= '0' && c <= '9')      v = c - '0';
			else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
			else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
			else break;
			sz = sz * 16 + (u32)v;
			in++;
			digits++;
		}
		if (!digits) return -1;
		while (in < len && p[in] != '\n') in++;
		if (in >= len) return -1;
		in++;
		if (sz == 0) break;
		if (in > len || len - in < sz) return -1;
		memmove(p + out, p + in, sz);
		out += sz;
		in  += sz;
		while (in < len && p[in] != '\n') in++;
		if (in < len) in++;
	}
	return (int)out;
}

/* ---- copiada literalmente de auth.c ---- */

static int split_url(const char *url, char *host, u32 hmax,
                     char *path, u32 pmax)
{
	const char *p = url;
	const char *slash;
	u32 hlen;

	if (strncmp(p, "https://", 8) == 0)      p += 8;
	else if (strncmp(p, "http://", 7) == 0)  p += 7;
	else return -1;

	slash = strchr(p, '/');
	hlen  = slash ? (u32)(slash - p) : (u32)strlen(p);

	if (hlen == 0 || hlen >= hmax) return -1;
	if (memchr(p, ':', hlen))      return -2;

	memcpy(host, p, hlen);
	host[hlen] = '\0';

	if (!slash) { snprintf(path, pmax, "/"); return 0; }
	if (strlen(slash) >= pmax) return -3;
	snprintf(path, pmax, "%s", slash);
	return 0;
}

/* ---- copiada literalmente de imgdec.c ---- */

static void imgShrink(u32 *dst, u32 dw, u32 dh, const u32 *src, u32 sw, u32 sh)
{
	u32 dy, dx;
	if (!dst || !src || !dw || !dh || !sw || !sh) return;
	for (dy = 0; dy < dh; dy++) {
		u32 y0 = (dy * sh) / dh;
		u32 y1 = ((dy + 1) * sh) / dh;
		if (y1 <= y0) y1 = y0 + 1;
		if (y1 > sh)  y1 = sh;
		for (dx = 0; dx < dw; dx++) {
			u32 x0 = (dx * sw) / dw;
			u32 x1 = ((dx + 1) * sw) / dw;
			u32 a = 0, r = 0, g = 0, b = 0, n = 0, y, x;
			if (x1 <= x0) x1 = x0 + 1;
			if (x1 > sw)  x1 = sw;
			for (y = y0; y < y1; y++) {
				const u32 *row = src + (size_t)y * sw;
				for (x = x0; x < x1; x++) {
					u32 p = row[x];
					a += (p >> 24) & 0xff;
					r += (p >> 16) & 0xff;
					g += (p >>  8) & 0xff;
					b +=  p        & 0xff;
					n++;
				}
			}
			if (!n) n = 1;
			dst[(size_t)dy * dw + dx] =
				((a / n) << 24) | ((r / n) << 16) |
				((g / n) <<  8) |  (b / n);
		}
	}
}

/* --------------------------------------------------------------------- */

static void t_split(void)
{
	char h[96], p[1024];

	CHECK(split_url("https://profile.xboxlive.com/users/me", h, sizeof h, p, sizeof p) == 0, "https simple");
	CHECK(strcmp(h, "profile.xboxlive.com") == 0, "host = %s", h);
	CHECK(strcmp(p, "/users/me") == 0, "path = %s", p);

	/* La forma real que devuelve GameDisplayPicRaw */
	CHECK(split_url("https://images-eds-ssl.xboxlive.com/image?url=abc%2Fdef&format=png&w=64",
	                h, sizeof h, p, sizeof p) == 0, "url con consulta");
	CHECK(strcmp(h, "images-eds-ssl.xboxlive.com") == 0, "host eds = %s", h);
	CHECK(strcmp(p, "/image?url=abc%2Fdef&format=png&w=64") == 0, "path eds = %s", p);

	CHECK(split_url("http://images-eds.xboxlive.com/image?x=1", h, sizeof h, p, sizeof p) == 0, "http");
	CHECK(strcmp(h, "images-eds.xboxlive.com") == 0, "host http = %s", h);

	CHECK(split_url("https://solohost", h, sizeof h, p, sizeof p) == 0, "sin barra");
	CHECK(strcmp(p, "/") == 0, "path por defecto = %s", p);

	CHECK(split_url("ftp://algo/x", h, sizeof h, p, sizeof p) == -1, "esquema desconocido");
	CHECK(split_url("https://host:8443/x", h, sizeof h, p, sizeof p) == -2, "puerto");
	CHECK(split_url("https:///x", h, sizeof h, p, sizeof p) == -1, "host vacio");
	CHECK(split_url("", h, sizeof h, p, sizeof p) == -1, "cadena vacia");

	/* host mas largo que el buffer */
	{
		char big[256];
		memset(big, 'a', sizeof big); big[sizeof big - 1] = 0;
		char url[400];
		snprintf(url, sizeof url, "https://%s/x", big);
		CHECK(split_url(url, h, sizeof h, p, sizeof p) == -1, "host largo rechazado");
	}
	/* path mas largo que el buffer: tiene que fallar, NO cortar */
	{
		char pshort[16];
		CHECK(split_url("https://h/12345678901234567890", h, sizeof h, pshort, sizeof pshort) == -3,
		      "path largo rechazado");
	}
}

static void t_dechunk(void)
{
	char b[512];
	int n;

	strcpy(b, "4\r\nWiki\r\n7\r\npedia i\r\nB\r\nn \r\nchunks.\r\n0\r\n\r\n");
	n = dechunk(b, (u32)strlen(b));
	CHECK(n == 22, "dechunk clasico -> %d", n);
	CHECK(n > 0 && memcmp(b, "Wikipedia in \r\nchunks.", 22) == 0, "contenido dechunk");

	/* un solo trozo con JSON */
	strcpy(b, "10\r\n{\"a\":1,\"b\":2222}\r\n0\r\n\r\n");
	n = dechunk(b, (u32)strlen(b));
	CHECK(n == 16, "un trozo -> %d", n);
	CHECK(n == 16 && memcmp(b, "{\"a\":1,\"b\":2222}", 16) == 0, "json dechunk");

	/* cuerpo vacio */
	strcpy(b, "0\r\n\r\n");
	n = dechunk(b, (u32)strlen(b));
	CHECK(n == 0, "vacio -> %d", n);

	/* extension detras del tamano */
	strcpy(b, "4;algo=x\r\nabcd\r\n0\r\n\r\n");
	n = dechunk(b, (u32)strlen(b));
	CHECK(n == 4 && memcmp(b, "abcd", 4) == 0, "extension -> %d", n);

	/* mal formados: no deben leer fuera ni devolver basura */
	strcpy(b, "zz\r\nabcd\r\n0\r\n\r\n");
	CHECK(dechunk(b, (u32)strlen(b)) == -1, "tamano no hexadecimal");

	strcpy(b, "FF\r\nabcd\r\n0\r\n\r\n");   /* dice 255, hay 4 */
	CHECK(dechunk(b, (u32)strlen(b)) == -1, "tamano mayor que los datos");

	strcpy(b, "4");                        /* cortado */
	CHECK(dechunk(b, (u32)strlen(b)) == -1, "cortado en el tamano");

	CHECK(dechunk(b, 0) == -1, "longitud cero");
}

static void t_hdr(void)
{
	const char *h1 = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nX: 1";
	const char *h2 = "HTTP/1.1 200 OK\r\ntransfer-encoding: CHUNKED\r\n";
	const char *h3 = "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n";

	CHECK(hdr_has(h1, (u32)strlen(h1), "transfer-encoding: chunked"), "chunked normal");
	CHECK(hdr_has(h2, (u32)strlen(h2), "transfer-encoding: chunked"), "chunked mayusculas");
	CHECK(!hdr_has(h3, (u32)strlen(h3), "transfer-encoding: chunked"), "sin chunked");
	CHECK(!hdr_has("ab", 2, "transfer-encoding: chunked"), "cabecera mas corta que el patron");

	CHECK(find_bytes("aaa\r\n\r\nbbb", 10, "\r\n\r\n", 4) == 3, "find_bytes");
	CHECK(find_bytes("aaa", 3, "\r\n\r\n", 4) == -1, "find_bytes sin resultado");
	CHECK(find_bytes("", 0, "x", 1) == -1, "find_bytes vacio");
	/* con ceros dentro: strstr fallaria aqui */
	{
		char blob[12] = { 'a', 0, 'b', 0, '\r', '\n', '\r', '\n', 'x', 0, 'y', 0 };
		CHECK(find_bytes(blob, 12, "\r\n\r\n", 4) == 4, "find_bytes con ceros");
	}
}

static void t_shrink(void)
{
	static u32 src[424 * 424];
	static u32 dst[64 * 64];
	u32 i;

	/* Todo del mismo color: el promedio tiene que dar ese color exacto,
	 * sin desviarse por redondeo. */
	for (i = 0; i < 424 * 424; i++) src[i] = 0xff336699;
	imgShrink(dst, 64, 64, src, 424, 424);
	for (i = 0; i < 64 * 64; i++)
		if (dst[i] != 0xff336699) { CHECK(0, "color plano roto en %u: %08x", i, dst[i]); break; }

	/* Mitad izquierda blanca, mitad derecha negra. La columna 0 tiene que
	 * salir blanca y la ultima negra. Esto es lo que distingue promediar
	 * de coger el pixel mas cercano. */
	for (i = 0; i < 424 * 424; i++)
		src[i] = (i % 424) < 212 ? 0xffffffff : 0xff000000;
	imgShrink(dst, 64, 64, src, 424, 424);
	CHECK(dst[0] == 0xffffffff, "columna izquierda = %08x", dst[0]);
	CHECK(dst[63] == 0xff000000, "columna derecha = %08x", dst[63]);

	/* Ampliar tambien tiene que funcionar sin dividir por cero ni salirse:
	 * un avatar diminuto no debe reventar. */
	for (i = 0; i < 4; i++) src[i] = 0xff112233;
	imgShrink(dst, 64, 64, src, 2, 2);
	CHECK(dst[0] == 0xff112233 && dst[64 * 64 - 1] == 0xff112233, "ampliacion 2x2 -> 64x64");

	/* Casos degenerados: no deben escribir nada ni caerse. */
	imgShrink(dst, 0, 0, src, 4, 4);
	imgShrink(dst, 64, 64, src, 0, 0);
	imgShrink(NULL, 64, 64, src, 4, 4);

	/* 1x1 de origen */
	src[0] = 0xdeadbeef;
	imgShrink(dst, 8, 8, src, 1, 1);
	CHECK(dst[0] == 0xdeadbeef, "1x1 -> 8x8 = %08x", dst[0]);
}

int main(void)
{
	t_split();
	t_dechunk();
	t_hdr();
	t_shrink();

	printf(fails ? "%d FALLOS\n" : "todo correcto (%d fallos)\n", fails);
	return fails ? 1 : 0;
}
