#include <stdio.h>
#include <string.h>
#include <stdlib.h>
typedef unsigned int u32;
static int fails=0;
#define CHECK(c,...) do{ if(!(c)){printf("FALLO: ");printf(__VA_ARGS__);printf("\n");fails++;} }while(0)

static const char *find_header(const char *hdrs, const char *name)
{
	u32 hlen = (u32)strlen(hdrs);
	u32 nlen = (u32)strlen(name);
	u32 i, j;
	if (hlen < nlen) return NULL;
	for (i = 0; i <= hlen - nlen; i++) {
		if (i && hdrs[i - 1] != '\n') continue;
		for (j = 0; j < nlen; j++) {
			char a = hdrs[i + j], b = name[j];
			if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
			if (a != b) break;
		}
		if (j == nlen) {
			const char *v = hdrs + i + nlen;
			while (*v == ' ' || *v == '\t') v++;
			return v;
		}
	}
	return NULL;
}
int main(void)
{
	const char *h1 = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 2347891\r\nX: 1";
	const char *h2 = "HTTP/1.1 200 OK\r\ncontent-length:42\r\n";
	const char *h3 = "HTTP/1.1 200 OK\r\nCONTENT-LENGTH:   7\r\n";
	const char *h4 = "HTTP/1.1 200 OK\r\nX-Content-Length: 999\r\nContent-Length: 5\r\n";
	const char *h5 = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n";
	const char *v;

	v = find_header(h1, "content-length:");
	CHECK(v && strtoul(v,NULL,10)==2347891, "normal -> %s", v?v:"(null)");
	v = find_header(h2, "content-length:");
	CHECK(v && strtoul(v,NULL,10)==42, "minusculas sin espacio -> %s", v?v:"(null)");
	v = find_header(h3, "content-length:");
	CHECK(v && strtoul(v,NULL,10)==7, "mayusculas con espacios -> %s", v?v:"(null)");

	/* EL CASO QUE IMPORTA: no debe cazar X-Content-Length */
	v = find_header(h4, "content-length:");
	CHECK(v && strtoul(v,NULL,10)==5, "prefijo X- ignorado -> %lu", v?strtoul(v,NULL,10):0);

	CHECK(find_header(h5, "content-length:")==NULL, "sin cabecera");
	CHECK(find_header("", "content-length:")==NULL, "vacio");
	CHECK(find_header("ab", "content-length:")==NULL, "mas corto que el patron");
	/* la cabecera justo al final, sin valor */
	v = find_header("HTTP/1.1 200 OK\r\ncontent-length:", "content-length:");
	CHECK(v && *v=='\0', "cabecera sin valor no se sale");

	printf(fails?"%d FALLOS\n":"find_header: todo correcto (%d fallos)\n", fails);
	return fails?1:0;
}
