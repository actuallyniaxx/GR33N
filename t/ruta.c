/* Prueba de sobremesa de dos cosas de session.c que son pura logica:
 *
 *   1. el armado de rutas, que le faltaba la barra de delante;
 *   2. la clasificacion del estado que devuelve el servidor, que esperaba
 *      un nombre inventado ("Provisioned") en vez del que manda de verdad
 *      ("ReadyToConnect"), y se quedaba sondeando cinco minutos con la
 *      maquina ya lista al otro lado.
 *
 * El fallo que trae la primera: el servidor devuelve el sessionPath SIN barra
 * inicial -"v5/sessions/cloud/UUID"-, se pegaba tal cual detras del host, y
 * lo que salia por el cable era
 *
 *     GET v5/sessions/cloud/UUID/state HTTP/1.1
 *
 * que no es una peticion valida. El gateway de Azure contestaba un 400 en
 * HTML que no nombraba ningun campo, porque el problema no estaba en ningun
 * campo: estaba en la primera linea.
 *
 * La logica esta copiada de ses_call() a mano. Si alguien la cambia alli y
 * no aqui, esto deja de cuadrar, que es justo lo que se quiere.
 */

#include <stdio.h>
#include <string.h>

static int fails = 0;

static void chk(const char *que, const char *dio, const char *esperado)
{
	if (strcmp(dio, esperado) == 0) {
		printf("  ok   %-46s -> %s\n", que, dio);
	} else {
		printf("  FALLO %-45s -> %s (esperaba %s)\n", que, dio, esperado);
		fails++;
	}
}

/* Copia literal de ses_call(). */
static int armar(char *full, unsigned max, const char *base_path,
                 const char *path)
{
	int n = snprintf(full, max, "%s%s%s",
	                 (base_path[0] && strcmp(base_path, "/") != 0) ? base_path : "",
	                 (path[0] == '/') ? "" : "/",
	                 path);
	return (n < 0 || (unsigned)n >= max) ? -1 : 0;
}

/* Copia literal del bloque de estados de step_state(). */
enum { ESPERA = 0, LISTA = 1, CONECTAR = 2, MUERTA = -1, COLA = 3 };

static int clasificar(const char *s)
{
	if (strcmp(s, "Provisioned") == 0)      return LISTA;
	if (strcmp(s, "ReadyToConnect") == 0)   return CONECTAR;

	if (strcmp(s, "Failed") == 0 ||
	    strcmp(s, "Terminated") == 0 ||
	    strcmp(s, "Canceled") == 0)         return MUERTA;

	if (strstr(s, "Wait") || strstr(s, "Queue")) return COLA;

	return ESPERA;
}

static const char *nombre(int c)
{
	switch (c) {
	case LISTA:    return "LISTA";
	case CONECTAR: return "CONECTAR";
	case MUERTA:   return "MUERTA";
	case COLA:     return "COLA";
	default:       return "ESPERA";
	}
}

/* El bucle de espera de run_session_inner(), reducido a lo que se puede
 * comprobar sin red: que el /connect se manda UNA vez, que no se manda
 * antes de tiempo, y que el bucle termina.
 *
 * Devuelve cuantos /connect se han mandado, o -1 si no termino. */
static int simular(const char *const *estados, int n, int *termino)
{
	int conectados = 0, i;

	*termino = 0;

	for (i = 0; i < n; i++) {
		int c = clasificar(estados[i]);

		if (c == MUERTA) return conectados;
		if (c == LISTA)  { *termino = 1; return conectados; }

		if (c == CONECTAR && !conectados) conectados++;
	}

	return conectados;
}

static void chk_n(const char *que, int dio, int esperado)
{
	if (dio == esperado) {
		printf("  ok   %-46s -> %d\n", que, dio);
	} else {
		printf("  FALLO %-45s -> %d (esperaba %d)\n", que, dio, esperado);
		fails++;
	}
}

static void chk_est(const char *que, const char *estado, int esperado)
{
	int dio = clasificar(estado);

	if (dio == esperado) {
		printf("  ok   %-46s -> %s\n", que, nombre(dio));
	} else {
		printf("  FALLO %-45s -> %s (esperaba %s)\n", que, nombre(dio),
		       nombre(esperado));
		fails++;
	}
}

int main(void)
{
	char f[320];

	printf("armado de rutas\n");

	/* Lo que manda GR33N a pelo: siempre con barra. No debe duplicarla. */
	armar(f, sizeof(f), "/", "/v5/sessions/cloud/play");
	chk("base \"/\" + ruta con barra", f, "/v5/sessions/cloud/play");

	/* EL CASO DEL 400: lo que devuelve el servidor, sin barra. */
	armar(f, sizeof(f), "/", "v5/sessions/cloud/9F8AC269/state");
	chk("base \"/\" + sessionPath sin barra", f,
	    "/v5/sessions/cloud/9F8AC269/state");

	armar(f, sizeof(f), "/", "v5/sessions/cloud/9F8AC269/configuration");
	chk("...y la configuracion", f,
	    "/v5/sessions/cloud/9F8AC269/configuration");

	/* El DELETE va contra la ruta pelada, sin sufijo. */
	armar(f, sizeof(f), "/", "v5/sessions/cloud/9F8AC269");
	chk("...y el borrado", f, "/v5/sessions/cloud/9F8AC269");

	/* Si el baseUri trajera prefijo -hoy no lo trae, pero manda el
	 * servidor y no nosotros- tiene que quedar pegado por delante y una
	 * sola barra entre medias, venga como venga la segunda parte. */
	armar(f, sizeof(f), "/gssv", "v5/sessions/cloud/X/state");
	chk("base con prefijo + ruta sin barra", f, "/gssv/v5/sessions/cloud/X/state");

	armar(f, sizeof(f), "/gssv", "/v5/sessions/cloud/play");
	chk("base con prefijo + ruta con barra", f, "/gssv/v5/sessions/cloud/play");

	/* Base vacia: pasa si tlsSplitUrl devolviera "" en vez de "/". */
	armar(f, sizeof(f), "", "v5/sessions/cloud/X/state");
	chk("base vacia + ruta sin barra", f, "/v5/sessions/cloud/X/state");

	/* Que el corte se detecte y no se mande media ruta a otro sitio. */
	{
		char corto[24];
		int r = armar(corto, sizeof(corto), "/",
		              "v5/sessions/cloud/9F8AC269-097F-4449/state");
		if (r == -1) {
			printf("  ok   %-46s -> -1\n", "ruta que no cabe: se detecta");
		} else {
			printf("  FALLO %-45s -> %d\n", "ruta que no cabe: se detecta", r);
			fails++;
		}
	}

	printf("\nclasificacion del estado\n");

	/* Lo que contesto el servidor de verdad, tal cual salio en el log:
	 *     {"state":"ReadyToConnect","errorDetails":null}
	 * y que NO significa "lista": significa "te toca a ti".             */
	chk_est("ReadyToConnect  (te toca a ti)",   "ReadyToConnect", CONECTAR);
	chk_est("Provisioned     (esta si es lista)", "Provisioned",  LISTA);
	chk_est("New",                                "New",          ESPERA);

	chk_est("Failed",     "Failed",     MUERTA);
	chk_est("Terminated", "Terminated", MUERTA);
	chk_est("Canceled",   "Canceled",   MUERTA);

	chk_est("WaitingForResources", "WaitingForResources", COLA);
	chk_est("Queued",              "Queued",              COLA);

	chk_est("Provisioning", "Provisioning", ESPERA);

	/* Lo importante del else: que NO se confunda con estar lista. Un
	 * nombre que no conocemos sigue sondeando, pero se dice. */
	chk_est("Inventado (no lo conozco)", "EstadoQueNoExiste", ESPERA);

	/* Y que "Ready" a secas no cuele por parecido: si el servicio cambia
	 * el nombre, se entera uno por el log, no por un cuelgue. */
	chk_est("Ready (parecido pero no)", "Ready", ESPERA);

	printf("\nsecuencia de la sesion\n");

	{
		/* El caso real de RPCS3: lista en el primer sondeo. */
		static const char *const rapido[] = {
			"ReadyToConnect", "ReadyToConnect", "Provisioning", "Provisioned"
		};
		/* Con cola por delante. */
		static const char *const con_cola[] = {
			"New", "WaitingForResources", "WaitingForResources",
			"ReadyToConnect", "Provisioning", "Provisioned"
		};
		/* Y el que se muere despues de autenticar. */
		static const char *const muerto[] = {
			"ReadyToConnect", "Provisioning", "Failed", "Provisioned"
		};
		int t, c;

		c = simular(rapido, 4, &t);
		chk_n("camino rapido: un solo /connect", c, 1);
		chk_n("camino rapido: termina",          t, 1);

		c = simular(con_cola, 6, &t);
		chk_n("con cola: un solo /connect", c, 1);
		chk_n("con cola: termina",          t, 1);

		/* LO QUE IMPORTA DE VERDAD: que un "Failed" despues del connect
		 * corte el bucle y no se coma el "Provisioned" de detras. Una
		 * sesion muerta que sigue adelante es una maquina reservada que
		 * nadie suelta. */
		c = simular(muerto, 4, &t);
		chk_n("sesion muerta: no termina bien", t, 0);
		chk_n("sesion muerta: ya habia conectado", c, 1);
	}

	printf("\n%s\n", fails ? "HAY FALLOS" : "todo bien");
	return fails ? 1 : 0;
}
