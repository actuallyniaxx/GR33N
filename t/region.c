/* Prueba del medidor de latencia y de la aritmetica del selector.
 *
 * ping.c se compila DE VERDAD, no una copia: contra una PS3 de mentira
 * (t/falso) y una red guionizada que tarda lo que le digamos. Asi se
 * comprueba lo unico que se puede comprobar sin consola -- que el minimo
 * es el minimo, que un fallo se distingue de un "todavia no", y que los
 * candados no se pisan -- y NO se comprueba, porque no se puede, si
 * PSL1GHT llama SO_NBIO a lo que aqui se llama SO_NBIO. Eso lo dira el
 * log de la consola.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "falso/ppu-types.h"
#include "../include/auth.h"
#include "../include/ping.h"
#include "guion.h"

static int fallos = 0;
static void ok(const char *q, int c)
{
	printf("   %-52s %s\n", q, c ? "ok" : "FALLA");
	if (!c) fallos++;
}

/* --------------------------------------------------------------------- */
/* auth de mentira: las regiones que le pasemos                          */
/* --------------------------------------------------------------------- */

static xcRegion regs[XC_REGIONS_MAX];
static u32      regs_n = 0;

u32 authRegionsN(void) { return regs_n; }

int authRegionCopy(int i, xcRegion *out)
{
	if (!out) return -1;
	if (i < 0 || (u32)i >= regs_n) { memset(out, 0, sizeof(*out)); return -1; }
	*out = regs[i];
	return 0;
}

int authRegionDefault(void)
{
	u32 i;
	for (i = 0; i < regs_n; i++) if (regs[i].is_default) return (int)i;
	return 0;
}

int authRegionPorNombre(const char *n)
{
	u32 i;
	if (!n || !n[0]) return -1;
	for (i = 0; i < regs_n; i++) if (strcmp(regs[i].name, n) == 0) return (int)i;
	return -1;
}

static void region(const char *nombre, const char *host, int def,
                   int resuelve, int acepta, int a, int b, int c)
{
	int i = (int)regs_n;

	snprintf(regs[i].name, sizeof(regs[i].name), "%s", nombre);
	snprintf(regs[i].host, sizeof(regs[i].host), "%s", host);
	snprintf(regs[i].base_uri, sizeof(regs[i].base_uri), "https://%s", host);
	regs[i].is_default = def;
	regs_n++;

	snprintf(guion[guion_n].host, sizeof(guion[0].host), "%s", host);
	guion[guion_n].resuelve = resuelve;
	guion[guion_n].acepta   = acepta;
	guion[guion_n].us[0] = a;
	guion[guion_n].us[1] = b;
	guion[guion_n].us[2] = c;
	guion[guion_n].us[3] = c;
	guion[guion_n].intentos = 0;
	guion_n++;
}

static void esperar_medida(void)
{
	int i;
	for (i = 0; i < 2000; i++) {
		if (pingEstado() == PING_HECHO) return;
		usleep(5000);
	}
	printf("   !! se agoto la espera del medidor\n");
	fallos++;
}

/* --------------------------------------------------------------------- */
/* La aritmetica del selector, copiada tal cual de ui.c                  */
/* --------------------------------------------------------------------- */

static int idx_region(int set, int n_regiones, int por_defecto)
{
	if (n_regiones <= 0) return -1;
	if (set > 0 && set <= n_regiones) return set - 1;
	if (por_defecto >= 0 && por_defecto < n_regiones) return por_defecto;
	return 0;
}

int main(int argc, char **argv)
{
	/* Sin buffer: si algo revienta a mitad, lo impreso hasta ahi se ve.
	 * Con buffer, LeakSanitizer se lleva por delante la salida entera al
	 * salir y la prueba parece no haber hecho nada. */
	setvbuf(stdout, NULL, _IONBF, 0);

	if (argc > 1 && strcmp(argv[1], "-v") == 0) guion_ruidoso = 1;

	printf("1. la aritmetica del selector (0 = automatico)\n");
	ok("sin regiones no hay indice",     idx_region(0, 0, 0) == -1);
	ok("automatico -> la de Microsoft",  idx_region(0, 5, 3) == 3);
	ok("la primera de la lista",         idx_region(1, 5, 3) == 0);
	ok("la ultima de la lista",          idx_region(5, 5, 3) == 4);
	/* El caso que importa: la lista encoge (otra cuenta, otro login) y el
	 * indice guardado se sale. Ni se lee fuera ni se elige a ciegas. */
	ok("indice pasado de largo -> la de Microsoft",
	   idx_region(9, 5, 3) == 3);
	ok("y si esa tampoco vale, la primera", idx_region(9, 5, 99) == 0);
	ok("negativo -> la de Microsoft",    idx_region(-4, 5, 2) == 2);

	printf("\n2. eleccion guardada POR NOMBRE, con la lista reordenada\n");
	{
		/* El login de hoy */
		snprintf(regs[0].name, sizeof(regs[0].name), "%s", "WestEurope");
		snprintf(regs[1].name, sizeof(regs[1].name), "%s", "UKSouth");
		snprintf(regs[2].name, sizeof(regs[2].name), "%s", "EastUS");
		regs_n = 3;
		ok("UKSouth es la 1 hoy", authRegionPorNombre("UKSouth") == 1);

		/* El de manana, con las mismas tres en otro orden */
		snprintf(regs[0].name, sizeof(regs[0].name), "%s", "EastUS");
		snprintf(regs[1].name, sizeof(regs[1].name), "%s", "WestEurope");
		snprintf(regs[2].name, sizeof(regs[2].name), "%s", "UKSouth");
		ok("y la 2 manana, sin dejar de ser UKSouth",
		   authRegionPorNombre("UKSouth") == 2);
		ok("una que ya no esta se sabe que no esta",
		   authRegionPorNombre("JapanEast") == -1);
		ok("cadena vacia no cuela como region",
		   authRegionPorNombre("") == -1);

		regs_n = 0;
		memset(regs, 0, sizeof(regs));
	}

	printf("\n3. el medidor, contra una red guionizada\n");
	/*        nombre         host          def res acep   us de cada intento */
	region("WestEurope",  "weu.falso", 1, 1, 1,  40000, 18000, 60000);
	region("UKSouth",     "uks.falso", 0, 1, 1,   9000,  9500,  9200);
	region("EastUS",      "eus.falso", 0, 1, 0,      0,     0,     0);
	region("JapanEast",   "jpe.falso", 0, 0, 1,   1000,  1000,  1000);
	region("Microscopica","mic.falso", 0, 1, 1,    300,   300,   300);

	if (pingInit() != 0) { printf("   !! pingInit fallo\n"); return 1; }

	ok("antes de medir, no hay nada", pingEstado() == PING_NADA);
	ok("y ningun milisegundo",        pingMs(0) == 0 && pingMs(1) == 0);

	pingMedir();
	esperar_medida();

	{
		int h = 0, t = 0;
		pingProgreso(&h, &t);
		ok("se han medido las cinco", h == 5 && t == 5);
	}

	/* EL MINIMO, NO LA MEDIA. Con 40, 18 y 60 ms la media son 39 y el
	 * minimo 18: si esto diera 39, el ordenador estaria comparando
	 * regiones por lo ocupada que estaba la red al medirlas. */
	ok("se queda el MINIMO de los tres (40/18/60 -> 18)",
	   pingMs(0) >= 17 && pingMs(0) <= 21);
	ok("los tres intentos se hacen de verdad", guion[0].intentos == 3);

	ok("una region estable sale por su valor (9 ms)",
	   pingMs(1) >= 8 && pingMs(1) <= 12);

	printf("      medidas: %u %u %u %u %u ms\n",
	       (unsigned)pingMs(0), (unsigned)pingMs(1), (unsigned)pingMs(2),
	       (unsigned)pingMs(3), (unsigned)pingMs(4));

	/* Las dos maneras de no tener numero, que NO son la misma cosa. */
	ok("la que rechaza: 0 ms y marcada como fallo",
	   pingMs(2) == 0 && pingFallo(2) == 1);
	ok("la que no resuelve: igual, 0 y fallo",
	   pingMs(3) == 0 && pingFallo(3) == 1);
	ok("y a la que no resuelve no se le intenta conectar",
	   guion[3].intentos == 0);
	ok("las que si contestaron NO estan marcadas como fallo",
	   pingFallo(0) == 0 && pingFallo(1) == 0 && pingFallo(4) == 0);

	/* Cero significa "sin medir" en toda la capa, asi que una medida real
	 * no puede valer cero: 300 us existen y tienen que salir como 1 ms. */
	ok("300 us salen como 1 ms, no como 0 (0 = sin medir)",
	   pingMs(4) == 1 && pingFallo(4) == 0);

	ok("la mas rapida es la de 300 us", pingMejor() == 4);

	/* Indices imposibles: nadie deberia pedirlos, pero el que pinta lee
	 * con un indice que viene de una lista que puede haber cambiado. */
	ok("fuera de rango no lee fuera",
	   pingMs(-1) == 0 && pingMs(99) == 0 &&
	   pingFallo(-1) == 0 && pingFallo(99) == 0);

	printf("\n4. volver a medir\n");
	{
		int i;
		for (i = 0; i < guion_n; i++) guion[i].intentos = 0;

		/* Ahora la que rechazaba acepta: una medida nueva tiene que
		 * enterarse, no quedarse con el fallo de la vez anterior. */
		guion[2].acepta = 1;
		guion[2].us[0] = guion[2].us[1] = guion[2].us[2] = 25000;

		pingMedir();
		usleep(50000);
		pingMedir();   /* pedirlo dos veces no lanza dos medidas */
		esperar_medida();

		ok("la que fallaba ahora da numero",
		   pingMs(2) >= 23 && pingMs(2) <= 28 && pingFallo(2) == 0);
		ok("y sin repetir la ronda por pedirlo dos veces",
		   guion[2].intentos == 3);
	}

	pingShutdown();

	printf("\n%s (%d fallos)\n", fallos ? "HAY FALLOS" : "todo bien", fallos);
	return fallos != 0;
}
