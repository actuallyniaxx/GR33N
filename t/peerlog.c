/* Prueba de peer_log: que recorte la ruta y no desborde. */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
static char ultimo[400];
void linkLog(const char *fmt, ...) {
	va_list ap; va_start(ap, fmt);
	vsnprintf(ultimo, sizeof(ultimo), fmt, ap); va_end(ap);
}
void peer_log(char *lvl, const char *f, int l, const char *fmt, ...);
static int fallos;
static void ok(int c, const char *q) {
	printf(c ? "   bien  %s\n" : "   MAL   %s\n", q); if (!c) fallos++;
}
int main(void) {
	char largo[500]; memset(largo, 'x', sizeof(largo)-1); largo[sizeof(largo)-1] = 0;

	peer_log("INFO", "/home/nia/.gr33n-deps/libpeer-src/src/agent.c", 94, "hola %d", 7);
	printf("   -> %s\n", ultimo);
	ok(strstr(ultimo, "agent.c:94") != NULL, "recorta la ruta y deja fichero:linea");
	ok(strstr(ultimo, "/home/nia") == NULL, "y no cuela la ruta entera");
	ok(strstr(ultimo, "hola 7") != NULL, "el mensaje formateado esta");

	peer_log("ERROR", "C:\\ps3dev\\src\\sctp.c", 12, "x");
	printf("   -> %s\n", ultimo);
	ok(strstr(ultimo, "sctp.c:12") != NULL, "tambien con contrabarras");

	peer_log("DEBUG", NULL, 0, "sin fichero");
	ok(strstr(ultimo, "?:0") != NULL, "un file_name nulo no revienta");

    peer_log("WARN", "src/x.c", 1, "%s", largo);
	ok(strlen(ultimo) < 400, "un mensaje enorme se recorta y no desborda");
	printf("   (la linea salio con %d letras)\n", (int)strlen(ultimo));

	printf("\n%s (%d fallos)\n", fallos ? "HAY FALLOS" : "todo bien", fallos);
	return fallos != 0;
}
