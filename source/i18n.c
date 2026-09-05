/* GR33N - ver i18n.h para el porque de que esto sea tan corto. */

#include <string.h>

#include "i18n.h"

static gr33nIdioma actual = IDIOMA_ES;

/* Una sola tabla, con el codigo y el nombre juntos. No hay dos listas que
 * puedan descolocarse porque no hay dos listas. */
static const struct {
	const char *codigo;
	const char *nombre;
} idiomas[IDIOMA_N] = {
	{ "es", "Espanol" },   /* sin tilde: la fuente no la tiene */
	{ "en", "English"  }
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

const char *tr(const char *es, const char *en)
{
	if (actual == IDIOMA_EN && en != NULL && en[0] != '\0') return en;
	return es;
}

/* --------------------------------------------------------------------- */
/* El idioma de xCloud                                                   */
/* --------------------------------------------------------------------- */

/* Las tres columnas de XCLOC_LISTA, generadas de la MISMA lista. No hay
 * tres arrays que mantener: hay uno de estructuras, y cada fila sale
 * entera de su linea. */
static const struct {
	const char *codigo;
	const char *es;
	const char *en;
} xclocs[] = {
#define X(c, e, i) { c, e, i },
	XCLOC_LISTA
#undef X
};

#define XCLOC_TOTAL ((int)(sizeof(xclocs) / sizeof(xclocs[0])))

/* Por defecto es-ES, que es donde estaba clavado hasta hoy: cambiar el
 * comportamiento por defecto al meter el ajuste seria colar un cambio
 * dentro de otro.
 *
 * PERO NO SE ESCRIBE EL 2. Habia puesto un 2 a pelo con un comentario al
 * lado diciendo "la fila de es-ES", y es exactamente el indice magico de
 * siempre: el dia que alguien meta un idioma delante, el 2 pasa a ser
 * es-MX y el comentario sigue jurando que es es-ES.
 *
 * Se resuelve por codigo la primera vez que se pregunta, que es la unica
 * forma de que no pueda mentir. */
/* en-US, igual que el idioma de fabrica de la interfaz.
 *
 * En la practica aplicar_idioma() de ui.c lo deriva del idioma elegido
 * mientras nadie haya tocado el ajuste de juego, asi que este valor solo
 * se ve si alguien pregunta antes de que la interfaz arranque. Aun asi
 * tiene que decir lo mismo: dos sitios que responden distinto a la misma
 * pregunta acaban discrepando el dia menos pensado. */
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
	return tr(xclocs[i].es, xclocs[i].en);
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
