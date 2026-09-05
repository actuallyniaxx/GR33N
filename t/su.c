#include <stdio.h>
#include <string.h>
typedef unsigned int u32;
static int fails=0;
#define CHECK(c,...) do{ if(!(c)){printf("FALLO: ");printf(__VA_ARGS__);printf("\n");fails++;} }while(0)
static int tlsSplitUrl(const char *url, char *host, u32 hmax, char *path, u32 pmax)
{
	const char *p = url; const char *slash; u32 hlen;
	if (!url || !host || !path || hmax < 2 || pmax < 2) return -1;
	if (strncmp(p, "https://", 8) == 0)      p += 8;
	else if (strncmp(p, "http://", 7) == 0)  p += 7;
	else if (strncmp(p, "//", 2) == 0)       p += 2;
	else return -1;
	slash = strchr(p, '/');
	hlen  = slash ? (u32)(slash - p) : (u32)strlen(p);
	if (hlen == 0 || hlen >= hmax) return -1;
	if (memchr(p, ':', hlen))      return -2;
	memcpy(host, p, hlen); host[hlen] = '\0';
	if (!slash) { snprintf(path, pmax, "/"); return 0; }
	if (strlen(slash) >= pmax) return -3;
	snprintf(path, pmax, "%s", slash);
	return 0;
}
int main(void)
{
	char h[128], p[512];
	/* la forma REAL que manda la tienda, con el https: que le pegamos delante */
	const char *real = "https://store-images.s-microsoft.com/image/apps.41143."
	                   "14287567326939893.d87aadd1-a515-4a5b-b76e-830000daa2d9."
	                   "5566f70a-d2f7-4341-a8dd-e25f0d3e2f3d?w=256&h=256";
	CHECK(tlsSplitUrl(real, h, sizeof h, p, sizeof p) == 0, "url real de la tienda");
	CHECK(strcmp(h, "store-images.s-microsoft.com") == 0, "host = %s", h);
	CHECK(strstr(p, "?w=256&h=256") != NULL, "los parametros sobreviven: %s", p);
	CHECK(p[0] == '/', "la ruta empieza por barra: %s", p);

	/* sin esquema, tal cual viene en el JSON */
	CHECK(tlsSplitUrl("//store-images.s-microsoft.com/image/x", h, sizeof h, p, sizeof p) == 0,
	      "url sin esquema");
	CHECK(strcmp(h, "store-images.s-microsoft.com") == 0, "host sin esquema = %s", h);

	CHECK(tlsSplitUrl("ftp://a/b", h, sizeof h, p, sizeof p) == -1, "esquema raro");
	CHECK(tlsSplitUrl("https://a:8443/b", h, sizeof h, p, sizeof p) == -2, "puerto");
	CHECK(tlsSplitUrl("https://solohost", h, sizeof h, p, sizeof p) == 0, "sin barra");
	CHECK(strcmp(p, "/") == 0, "ruta por defecto");
	{ char sp[8]; CHECK(tlsSplitUrl("https://h/123456789", h, sizeof h, sp, sizeof sp) == -3,
	                    "ruta larga se RECHAZA, no se recorta"); }
	printf(fails?"%d FALLOS\n":"tlsSplitUrl: todo correcto (%d fallos)\n", fails);
	return fails?1:0;
}
