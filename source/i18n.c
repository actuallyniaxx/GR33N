/* GR33N - ver i18n.h para el porque de que esto sea tan corto. */

#include <string.h>

#include "i18n.h"

static gr33nIdioma actual = IDIOMA_ES;

/* Una sola tabla, generada de IDIOMA_LISTA. El codigo, el nombre y el
 * idioma de juego de un idioma salen todos de su MISMA linea, asi que no
 * hay dos listas que puedan descolocarse porque no hay dos listas. */
static const struct {
	const char *codigo;
	const char *nombre;
	const char *juego;
} idiomas[IDIOMA_N] = {
#define X(e, c, n, l) { c, n, l },
	IDIOMA_LISTA
#undef X
};

void i18nSet(gr33nIdioma i)
{
	if (i >= 0 && i < IDIOMA_N) actual = i;
}

gr33nIdioma i18nGet(void) { return actual; }

const char *i18nCodigo(gr33nIdioma i)
{
	if (i < 0 || i >= IDIOMA_N) return idiomas[IDIOMA_ES].codigo;
	return idiomas[i].codigo;
}

const char *i18nNombre(gr33nIdioma i)
{
	if (i < 0 || i >= IDIOMA_N) return idiomas[IDIOMA_ES].nombre;
	return idiomas[i].nombre;
}

const char *i18nJuegoPorDefecto(gr33nIdioma i)
{
	if (i < 0 || i >= IDIOMA_N) return idiomas[IDIOMA_ES].juego;
	return idiomas[i].juego;
}

gr33nIdioma i18nPorCodigo(const char *cod)
{
	int i;

	if (cod == NULL || cod[0] == '\0') return IDIOMA_ES;

	for (i = 0; i < IDIOMA_N; i++)
		if (strcmp(idiomas[i].codigo, cod) == 0) return (gr33nIdioma)i;

	/* Un codigo que no conocemos --de una version futura, o de un fichero
	 * editado a mano-- no deja la interfaz muda: se cae al castellano. */
	return IDIOMA_ES;
}

/* La casilla del idioma actual, y si esta vacia, el castellano.
 *
 * Los dos NULL que se comprueban aqui no son paranoia de firma: v entero
 * es NULL cuando una entrada del catalogo no trae texto nuestro (los
 * nombres los pone Microsoft y YA vienen traducidos), y v[actual] es NULL
 * en cada cadena que todavia no se ha traducido -- los paneles de Pruebas
 * llevan los cuatro a NULL a proposito. Las dos cosas pasan ahora mismo,
 * en varios sitios, a proposito. */
const char *trPick(const char *const *v)
{
	if (v == NULL) return "";

	if (actual >= 0 && actual < IDIOMA_N &&
	    v[actual] != NULL && v[actual][0] != '\0')
		return v[actual];

	return (v[IDIOMA_ES] != NULL) ? v[IDIOMA_ES] : "";
}

/* --------------------------------------------------------------------- */
/* El idioma de xCloud                                                   */
/* --------------------------------------------------------------------- */

/* Las seis columnas de XCLOC_LISTA, generadas de la MISMA lista. No hay
 * seis arrays que mantener: hay uno de estructuras, y cada fila sale
 * entera de su linea.
 *
 * Los cinco nombres van en un array de IDIOMA_N y no en cinco campos con
 * nombre, para que sea el MISMO array que come trPick() en todas partes. */
static const struct {
	const char *codigo;
	const char *n[IDIOMA_N];
} xclocs[] = {
#define X(c, es, en, fr, it, pt) { c, { es, en, fr, it, pt } },
	XCLOC_LISTA
#undef X
};

#define XCLOC_TOTAL ((int)(sizeof(xclocs) / sizeof(xclocs[0])))

/* Por defecto en-US.
 *
 * NO SE ESCRIBE EL INDICE. Habia puesto un 2 a pelo con un comentario al
 * lado diciendo "la fila de es-ES", y es exactamente el indice magico de
 * siempre: el dia que alguien meta un idioma delante, el 2 pasa a ser
 * es-MX y el comentario sigue jurando que es es-ES.
 *
 * Se resuelve por codigo la primera vez que se pregunta, que es la unica
 * forma de que no pueda mentir.
 *
 * En la practica aplicar_idioma() de ui.c lo deriva del idioma elegido
 * --con i18nJuegoPorDefecto()-- mientras nadie haya tocado el ajuste de
 * juego, asi que este valor solo se ve si alguien pregunta antes de que
 * la interfaz arranque. */
#define XCLOC_DEFECTO "en-US"

static int xcloc_actual = -1;   /* sin resolver todavia */

static int actual_idx(void)
{
	if (xcloc_actual < 0) {
		int i = xclocPorCodigo(XCLOC_DEFECTO);
		xcloc_actual = (i >= 0) ? i : 0;
	}
	return xcloc_actual;
}

int xclocN(void) { return XCLOC_TOTAL; }

const char *xclocCodigo(int i)
{
	if (i < 0 || i >= XCLOC_TOTAL) return "es-ES";
	return xclocs[i].codigo;
}

const char *xclocNombre(int i)
{
	if (i < 0 || i >= XCLOC_TOTAL) return "?";
	return trPick(xclocs[i].n);
}

int xclocPorCodigo(const char *cod)
{
	int i;

	if (cod == NULL || cod[0] == '\0') return -1;

	for (i = 0; i < XCLOC_TOTAL; i++)
		if (strcmp(xclocs[i].codigo, cod) == 0) return i;

	return -1;
}

void xclocSet(int i)
{
	if (i >= 0 && i < XCLOC_TOTAL) xcloc_actual = i;
}

int xclocGet(void) { return actual_idx(); }

const char *xclocActual(void) { return xclocs[actual_idx()].codigo; }
