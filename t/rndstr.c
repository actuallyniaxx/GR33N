/* Prueba de sobremesa de la utils_random_string nueva.
 * El generador de la consola se sustituye por uno controlable para poder
 * comprobar el rechazo, el reparto y los casos raros. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int modo = 0;           /* 0 = pseudoaleatorio, 1 = todo 0xFF */
static unsigned semilla = 1;

#define LOGE(fmt, ...) do { printf("   [LOGE] " fmt "\n", ##__VA_ARGS__); } while (0)

#define MBEDTLS_ENTROPY_BLOCK_SIZE 32
typedef int mbedtls_entropy_context;
static void mbedtls_entropy_init(mbedtls_entropy_context *c) { *c = 0; }
static void mbedtls_entropy_free(mbedtls_entropy_context *c) { *c = -1; }
static int mbedtls_entropy_func(void *d, unsigned char *out, size_t len)
{
	size_t i;
	(void)d;
	if (len > MBEDTLS_ENTROPY_BLOCK_SIZE) return -1;   /* como el de verdad */
	for (i = 0; i < len; i++) {
		if (modo == 1) { out[i] = 0xFF; continue; }
		semilla = semilla * 1103515245u + 12345u;
		out[i] = (unsigned char)(semilla >> 16);
	}
	return 0;
}

static void utils_random_string(char* s, const int len) {
  int i;
  static const char alphanum[] =
      "0123456789"
      "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
      "abcdefghijklmnopqrstuvwxyz";
  const int base = (int)(sizeof(alphanum) - 1);
  const unsigned char corte = (unsigned char)(256 - (256 % 62));
  int recargas = 0;
  mbedtls_entropy_context ent;

  mbedtls_entropy_init(&ent);
  i = 0;
  while (i < len) {
    unsigned char crudo[MBEDTLS_ENTROPY_BLOCK_SIZE];
    size_t k;

    if (++recargas > 32) { LOGE("32 recargas sin completar %d", len); break; }

    memset(crudo, 0, sizeof(crudo));
    if (mbedtls_entropy_func(&ent, crudo, sizeof(crudo)) != 0) {
      LOGE("sin entropia"); break;
    }
    for (k = 0; k < sizeof(crudo) && i < len; k++) {
      if (crudo[k] >= corte) continue;
      s[i++] = alphanum[crudo[k] % base];
    }
  }
  s[i] = '\0';
  mbedtls_entropy_free(&ent);
}

static int fallos = 0;
static void ok(const char *q, int cond) { printf("   %-52s %s\n", q, cond ? "ok" : "FALLA"); if (!cond) fallos++; }

int main(void)
{
	char a[257], b[257];
	int i, cuenta[62]; long total = 0;

	printf("corte = %d (deberia ser 248)\n", 256 - (256 % 62));
	printf("\n1. ufrag y pwd seguidos, como en agent.c:297-298\n");
	utils_random_string(a, 4);
	utils_random_string(b, 24);
	printf("   ufrag %s\n   pwd   %s\n", a, b);
	ok("la pwd NO empieza por el ufrag", strncmp(a, b, 4) != 0);
	ok("longitudes correctas", strlen(a) == 4 && strlen(b) == 24);

	printf("\n2. longitudes\n");
	utils_random_string(a, 256);
	ok("len=256 (ICE_UPWD_LENGTH) se completa", strlen(a) == 256);
	utils_random_string(a, 1);
	ok("len=1", strlen(a) == 1);
	utils_random_string(a, 0);
	ok("len=0 deja cadena vacia", strlen(a) == 0);

	printf("\n3. solo salen caracteres del alfabeto\n");
	utils_random_string(a, 256);
	{ int malo = 0;
	  for (i = 0; i < 256; i++)
		  if (!((a[i]>='0'&&a[i]<='9')||(a[i]>='A'&&a[i]<='Z')||(a[i]>='a'&&a[i]<='z'))) malo = 1;
	  ok("256 caracteres, todos del alfabeto", !malo); }

	printf("\n4. reparto (rechazo bien hecho = sin sesgo grande)\n");
	memset(cuenta, 0, sizeof(cuenta));
	for (i = 0; i < 4000; i++) {
		int k; utils_random_string(a, 62);
		for (k = 0; k < 62; k++) {
			const char *p = strchr("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz", a[k]);
			if (p) { cuenta[p - "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"]++; total++; }
		}
	}
	{ int min = cuenta[0], max = cuenta[0];
	  for (i = 1; i < 62; i++) { if (cuenta[i] < min) min = cuenta[i]; if (cuenta[i] > max) max = cuenta[i]; }
	  printf("   %ld muestras, min %d max %d, esperado %ld\n", total, min, max, total/62);
	  ok("ningun caracter se desvia mas del 15%", (max - min) * 100 / (total/62) < 15); }

	printf("\n5. generador estropeado (siempre 0xFF, todo se rechaza)\n");
	modo = 1;
	utils_random_string(a, 24);
	ok("no se cuelga y deja cadena terminada", strlen(a) == 0);

	printf("\n%s (%d fallos)\n", fallos ? "HAY FALLOS" : "todo bien", fallos);
	return fallos != 0;
}
