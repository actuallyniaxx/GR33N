#include <stdio.h>
#include <string.h>
#include <stdlib.h>
typedef unsigned int u32;
static int dechunk(char *p, u32 len)
{
	u32 in = 0, out = 0;
	for (;;) {
		u32 sz = 0; int digits = 0;
		while (in < len) {
			char c = p[in]; int v;
			if (c >= '0' && c <= '9') v = c - '0';
			else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
			else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
			else break;
			sz = sz * 16 + (u32)v; in++; digits++;
		}
		if (!digits) return -1;
		while (in < len && p[in] != '\n') in++;
		if (in >= len) return -1;
		in++;
		if (sz == 0) break;
		if (in > len || len - in < sz) return -1;
		memmove(p + out, p + in, sz); out += sz; in += sz;
		while (in < len && p[in] != '\n') in++;
		if (in < len) in++;
	}
	return (int)out;
}
int main(void)
{
	const char *alpha = "0123456789abcdefABCDEF\r\n;xyz \t";
	int alen = (int)strlen(alpha);
	srand(1234);
	for (long it = 0; it < 400000; it++) {
		u32 len = (u32)(rand() % 64);
		char *b = malloc(len ? len : 1);   /* justo, para que ASan vea el borde */
		for (u32 i = 0; i < len; i++) b[i] = alpha[rand() % alen];
		int r = dechunk(b, len);
		if (r > (int)len) { printf("SALIDA MAYOR QUE ENTRADA: %d > %u\n", r, len); return 1; }
		free(b);
	}
	/* y con tamanos enormes declarados, que es el ataque obvio */
	for (long it = 0; it < 2000; it++) {
		const char *s = "ffffffff\r\nabc\r\n0\r\n\r\n";
		u32 len = (u32)strlen(s);
		char *b = malloc(len); memcpy(b, s, len);
		if (dechunk(b, len) != -1) { printf("acepto un tamano imposible\n"); return 1; }
		free(b);
	}
	printf("fuzz de dechunk: sin desbordes en 400k casos\n");
	return 0;
}
