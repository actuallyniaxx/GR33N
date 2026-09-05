/* Prueba de i18n.c: los dos idiomas y la lista de locales de xCloud.
 *
 * i18n.c se compila DE VERDAD, sin nada de la consola: no usa una sola
 * funcion de PSL1GHT, que es justo lo que lo hace probable aqui entero.
 *
 * Lo que se comprueba no es "traduce bien" --eso lo dice quien lee la
 * pantalla-- sino las tres cosas que pueden romperse solas: que un codigo
 * guardado sobreviva a que cambie el orden de la lista, que un codigo
 * desconocido no deje nada en blanco, y que la lista de locales no tenga
 * filas repetidas ni descolocadas.
 */
#include <stdio.h>
#include <string.h>

#include "../include/i18n.h"

static int fallos = 0;
static void ok(const char *q, int c)
{
	printf("   %-54s %s\n", q, c ? "ok" : "FALLA");
	if (!c) fallos++;
}

int main(void)
{
	int i, n;

	printf("1. el idioma de la interfaz\n");
	i18nSet(IDIOMA_ES);
	ok("por defecto castellano", i18nGet() == IDIOMA_ES);
	ok("tr devuelve el castellano",
	   strcmp(tr("Ajustes", "Settings"), "Ajustes") == 0);

	i18nSet(IDIOMA_EN);
	ok("en ingles devuelve el ingles",
	   strcmp(tr("Ajustes", "Settings"), "Settings") == 0);

	/* LO QUE NO SE TRADUCE SALE EN CASTELLANO, NO EN BLANCO. Una pantalla
	 * a medio traducir se lee raro; una con huecos no se lee. */
	ok("sin traduccion (NULL) cae al castellano",
	   strcmp(tr("Ajustes", NULL), "Ajustes") == 0);
	ok("y con cadena vacia tambien",
	   strcmp(tr("Ajustes", ""), "Ajustes") == 0);

	printf("\n2. se guarda el CODIGO, no el numero\n");
	ok("es -> IDIOMA_ES", i18nPorCodigo("es") == IDIOMA_ES);
	ok("en -> IDIOMA_EN", i18nPorCodigo("en") == IDIOMA_EN);
	/* Un fichero de una version futura, o editado a mano, no puede dejar
	 * la interfaz muda. */
	ok("un codigo desconocido cae al castellano",
	   i18nPorCodigo("kl") == IDIOMA_ES);
	ok("y una cadena vacia tambien", i18nPorCodigo("") == IDIOMA_ES);
	ok("y NULL", i18nPorCodigo(NULL) == IDIOMA_ES);

	ok("ida y vuelta es", i18nPorCodigo(i18nCodigo(IDIOMA_ES)) == IDIOMA_ES);
	ok("ida y vuelta en", i18nPorCodigo(i18nCodigo(IDIOMA_EN)) == IDIOMA_EN);

	printf("\n3. el nombre de cada idioma va EN SU IDIOMA\n");
	/* Quien tiene la interfaz en uno que no entiende necesita reconocer
	 * el suyo en la lista, y para eso el unico nombre que sirve es el
	 * propio. Asi que NO puede depender de i18nGet(). */
	i18nSet(IDIOMA_ES);
	{
		const char *es_desde_es = i18nNombre(IDIOMA_ES);
		const char *en_desde_es = i18nNombre(IDIOMA_EN);

		i18nSet(IDIOMA_EN);
		ok("Espanol se llama igual mire quien mire",
		   strcmp(es_desde_es, i18nNombre(IDIOMA_ES)) == 0);
		ok("English tambien",
		   strcmp(en_desde_es, i18nNombre(IDIOMA_EN)) == 0);
		ok("y son distintos entre si",
		   strcmp(i18nNombre(IDIOMA_ES), i18nNombre(IDIOMA_EN)) != 0);
	}

	printf("\n4. la fuente es ASCII 32..122: nada puede salirse\n");
	{
		/* Si alguien mete una tilde en un nombre de idioma o de locale,
		 * en la consola sale un hueco. Aqui se ve antes. */
		int malos = 0;

		for (i = 0; i < IDIOMA_N; i++) {
			const char *p = i18nNombre((gr33nIdioma)i);
			for (; *p; p++)
				if ((unsigned char)*p < 32 || (unsigned char)*p > 122) {
					printf("      fuera de rango en idioma %d: 0x%02x\n",
					       i, (unsigned char)*p);
					malos++;
				}
		}

		i18nSet(IDIOMA_ES);
		for (i = 0; i < xclocN(); i++) {
			const char *p = xclocNombre(i);
			for (; *p; p++)
				if ((unsigned char)*p < 32 || (unsigned char)*p > 122) {
					printf("      fuera de rango en %s (es): 0x%02x\n",
					       xclocCodigo(i), (unsigned char)*p);
					malos++;
				}
		}

		i18nSet(IDIOMA_EN);
		for (i = 0; i < xclocN(); i++) {
			const char *p = xclocNombre(i);
			for (; *p; p++)
				if ((unsigned char)*p < 32 || (unsigned char)*p > 122) {
					printf("      fuera de rango en %s (en): 0x%02x\n",
					       xclocCodigo(i), (unsigned char)*p);
					malos++;
				}
		}

		ok("ningun nombre lleva un caracter sin glifo", malos == 0);
	}

	printf("\n5. la lista de idiomas de xCloud\n");
	n = xclocN();
	printf("      %d locales\n", n);
	ok("hay lista", n > 0);

	{
		int repes = 0, j;
		for (i = 0; i < n; i++)
			for (j = i + 1; j < n; j++)
				if (strcmp(xclocCodigo(i), xclocCodigo(j)) == 0) repes++;
		ok("sin codigos repetidos", repes == 0);
	}

	{
		/* El nombre y el codigo salen de la MISMA fila de XCLOC_LISTA, asi
		 * que no pueden descolocarse. Se comprueba igual: es la propiedad
		 * de la que depende que el ajuste no mienta. */
		int mal = 0;
		for (i = 0; i < n; i++)
			if (xclocPorCodigo(xclocCodigo(i)) != i) mal++;
		ok("cada codigo vuelve a su propia fila", mal == 0);
	}

	ok("un codigo que no esta da -1", xclocPorCodigo("xx-XX") == -1);
	ok("y una cadena vacia tambien", xclocPorCodigo("") == -1);
	ok("y NULL", xclocPorCodigo(NULL) == -1);

	printf("\n6. el que esta puesto\n");
	/* XCLOC_DEFECTO paso a en-US cuando se fijaron los ajustes de fabrica
	 * (interfaz en ingles, juegos en English US). Esta linea decia es-ES y
	 * seguia diciendolo despues del cambio: una prueba que se quedo atras
	 * no avisa de nada, solo mete ruido rojo. */
	ok("por defecto en-US", strcmp(xclocActual(), "en-US") == 0);

	{
		int jp = xclocPorCodigo("ja-JP");
		ok("ja-JP existe", jp >= 0);
		xclocSet(jp);
		ok("y al ponerlo, es el que sale",
		   strcmp(xclocActual(), "ja-JP") == 0);
	}

	/* Un indice fuera de rango NO cambia nada ni lee fuera: quien lo pasa
	 * puede venir de un fichero de ajustes de otra version. */
	{
		const char *antes = xclocActual();
		char guardado[16];

		snprintf(guardado, sizeof(guardado), "%s", antes);
		xclocSet(-1);
		ok("un indice negativo no cambia nada",
		   strcmp(xclocActual(), guardado) == 0);
		xclocSet(9999);
		ok("y uno pasado de largo tampoco",
		   strcmp(xclocActual(), guardado) == 0);
	}

	ok("xclocCodigo fuera de rango no lee fuera",
	   xclocCodigo(-1) != NULL && xclocCodigo(9999) != NULL);
	ok("xclocNombre tampoco",
	   xclocNombre(-1) != NULL && xclocNombre(9999) != NULL);

	printf("\n%s (%d fallos)\n", fallos ? "HAY FALLOS" : "todo bien", fallos);
	return fallos != 0;
}
