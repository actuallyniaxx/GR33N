/* Prueba de sobremesa de source/teredo.c. Todo esto es aritmetica sobre
 * cadenas, asi que el PC contesta igual que la consola. */
#include <stdio.h>
#include <string.h>
#include "../include/teredo.h"

static int fallos = 0;
static void ok(const char *q, int c) { printf("   %-56s %s\n", q, c?"ok":"FALLA"); if(!c) fallos++; }

int main(void)
{
	char ip[16], cand[192], dir[64];
	int puerto;

	printf("1. Teredo, el ejemplo canonico de la RFC 4380\n");
	/* 2001:0:4136:e378:8000:63bf:3fff:fdd2 -> 192.0.2.45 puerto 40000 */
	ok("decodifica", teredoDecode("2001:0:4136:e378:8000:63bf:3fff:fdd2", ip, sizeof(ip), &puerto));
	printf("      -> %s : %d\n", ip, puerto);
	ok("IPv4 = 192.0.2.45", strcmp(ip, "192.0.2.45") == 0);
	ok("puerto = 40000", puerto == 40000);

	printf("\n2. grupos de cuatro digitos (forma larga)\n");
	ok("decodifica", teredoDecode("2001:0000:4136:e378:8000:63bf:3fff:fdd2", ip, sizeof(ip), &puerto));
	ok("mismo resultado", strcmp(ip, "192.0.2.45") == 0 && puerto == 40000);

	printf("\n3. lo que NO es Teredo se rechaza\n");
	ok("2002: (6to4)",        !teredoDecode("2002:0:4136:e378:8000:63bf:3fff:fdd2", ip, sizeof(ip), &puerto));
	ok("2001:db8: (no es el prefijo)", !teredoDecode("2001:db8:4136:e378:8000:63bf:3fff:fdd2", ip, sizeof(ip), &puerto));
	ok("comprimida con ::", !teredoDecode("2001::63bf:3fff:fdd2", ip, sizeof(ip), &puerto));
	ok("siete grupos",       !teredoDecode("2001:0:4136:e378:8000:63bf:3fff", ip, sizeof(ip), &puerto));
	ok("nueve grupos",       !teredoDecode("2001:0:4136:e378:8000:63bf:3fff:fdd2:1", ip, sizeof(ip), &puerto));
	ok("un digito que no es hex", !teredoDecode("2001:0:4136:e378:8000:63bg:3fff:fdd2", ip, sizeof(ip), &puerto));
	ok("grupo de cinco digitos",  !teredoDecode("2001:0:41366:e378:8000:63bf:3fff:fdd2", ip, sizeof(ip), &puerto));
	ok("grupo vacio",        !teredoDecode("2001::4136:e378:8000:63bf:3fff:fdd2", ip, sizeof(ip), &puerto));
	ok("una IPv4 a secas",   !teredoDecode("192.168.1.150", ip, sizeof(ip), &puerto));
	ok("cadena vacia",       !teredoDecode("", ip, sizeof(ip), &puerto));
	ok("buffer corto",       !teredoDecode("2001:0:4136:e378:8000:63bf:3fff:fdd2", ip, 8, &puerto));

	printf("\n4. normalizar lineas de candidato\n");
	ok("con a= delante", candNormaliza("a=candidate:0 1 UDP 2130 1.2.3.4 5 typ host", cand, sizeof(cand))
	   && strcmp(cand, "candidate:0 1 UDP 2130 1.2.3.4 5 typ host") == 0);
	ok("sin a=",          candNormaliza("candidate:1 1 UDP 100 13.104.1.2 9002 typ host", cand, sizeof(cand)));
	ok("con \\r\\n al final", candNormaliza("a=candidate:0 1 UDP 2130 1.2.3.4 5 typ host\r\n", cand, sizeof(cand))
	   && cand[strlen(cand)-1] == 't');
	ok("con espacios delante", candNormaliza("   candidate:0 1 UDP 1 1.2.3.4 5 typ host", cand, sizeof(cand)));
	ok("end-of-candidates NO es candidato", !candNormaliza("a=end-of-candidates", cand, sizeof(cand)));
	ok("una linea cualquiera",  !candNormaliza("a=ice-ufrag:WNmk", cand, sizeof(cand)));
	ok("NULL",                  !candNormaliza(NULL, cand, sizeof(cand)));
	ok("no cabe -> falla, no recorta",
	   !candNormaliza("candidate:0 1 UDP 2130 1.2.3.4 5 typ host", cand, 20));

	printf("\n5. direccion, puerto y prioridad\n");
	candNormaliza("a=candidate:2 1 UDP 1694373887 90.173.77.130 65047 typ srflx raddr 0.0.0.0 rport 0", cand, sizeof(cand));
	ok("direccion", candDireccion(cand, dir, sizeof(dir), &puerto) && strcmp(dir, "90.173.77.130") == 0);
	ok("puerto 65047", puerto == 65047);
	ok("prioridad 1694373887", candPrioridad(cand) == 1694373887UL);

	candNormaliza("candidate:1 1 UDP 100 13.104.118.6 1063 typ host", cand, sizeof(cand));
	ok("el relleno tiene prioridad 100", candPrioridad(cand) == 100);
	ok("y se distingue del bueno (>1000)", candPrioridad(cand) <= 1000);

	ok("candidato con IPv6 dentro", candDireccion("candidate:5 1 UDP 1 2001:0:4136:e378:8000:63bf:3fff:fdd2 9002 typ host", dir, sizeof(dir), &puerto)
	   && strcmp(dir, "2001:0:4136:e378:8000:63bf:3fff:fdd2") == 0);
	ok("linea corta -> 0", !candDireccion("candidate:0 1 UDP", dir, sizeof(dir), &puerto));
	ok("puerto no numerico -> 0", !candDireccion("candidate:0 1 UDP 1 1.2.3.4 abc typ host", dir, sizeof(dir), &puerto));
	ok("puerto fuera de rango -> 0", !candDireccion("candidate:0 1 UDP 1 1.2.3.4 70000 typ host", dir, sizeof(dir), &puerto));
	ok("prioridad no numerica -> 0", candPrioridad("candidate:0 1 UDP xx 1.2.3.4 5 typ host") == 0);

	printf("\n6. la cadena entera, como llegara de xCloud\n");
	{
		const char *crudo = "a=candidate:5 1 UDP 1 2001:0:4136:e378:8000:63bf:3fff:fdd2 9002 typ host\r\n";
		ok("normaliza", candNormaliza(crudo, cand, sizeof(cand)));
		ok("saca la direccion", candDireccion(cand, dir, sizeof(dir), &puerto));
		ok("es IPv6 (lleva ':')", strchr(dir, ':') != NULL);
		ok("y es Teredo", teredoDecode(dir, ip, sizeof(ip), &puerto));
		printf("      -> host %s, puerto Teredo %d (y ademas el 9002)\n", ip, puerto);
		ok("IPv4 buena", strcmp(ip, "192.0.2.45") == 0);
	}

	printf("\n%s (%d fallos)\n", fallos ? "HAY FALLOS" : "todo bien", fallos);
	return fallos != 0;
}
