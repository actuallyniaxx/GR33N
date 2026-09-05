/* GR33N - dos idiomas, sin tabla de identificadores
 *
 * ------------------------------------------------------------------
 * POR QUE NO HAY UN enum DE CADENAS
 * ------------------------------------------------------------------
 *
 * Lo normal seria un identificador por texto (T_AJUSTES, T_CUENTA...) y
 * dos arrays, uno por idioma. Aqui NO, y por una razon concreta: eso son
 * dos listas paralelas mantenidas a mano, que es el fallo que este
 * proyecto lleva meses cometiendo en todas sus formas -- la lista de
 * regiones inventada al lado de la de verdad, el combo_name[] junto al
 * combo_btn[], el config.h de opus con las exclusiones a mano. Cada vez
 * que hay dos listas que tienen que ir en el mismo orden, tarde o
 * temprano una se queda atras, y aqui el sintoma seria el peor de todos:
 * un texto que dice algo distinto de lo que hace el boton.
 *
 * Asi que cada cadena lleva su traduccion AL LADO. En las tablas, un
 * campo _en junto al castellano. En el codigo suelto, TR("es", "en").
 * No hay indice que pueda apuntar mal porque no hay indice.
 *
 * Sale mas caro en binario (las dos cadenas viajan siempre) y mas
 * repetitivo de leer. Se acepta: son unos kilobytes de los 256 MB de la
 * consola, y a cambio es imposible que un texto se descoloque.
 *
 * ------------------------------------------------------------------
 * SIN TRADUCIR SALE EN CASTELLANO, NO EN BLANCO
 * ------------------------------------------------------------------
 *
 * tr() con NULL o cadena vacia en ingles devuelve el castellano. Una
 * pantalla a medio traducir se lee raro; una con huecos no se lee.
 *
 * ------------------------------------------------------------------
 * Y POR QUE SOLO ESTOS DOS
 * ------------------------------------------------------------------
 *
 * Por la fuente. text.c lleva un mapa de 5x7 pixeles: ASCII 32..122 mas
 * una tabla font_extra con tildes, dieresis, ñ, ¿ y ¡. Eso cubre
 * castellano e ingles enteros y nada mas.
 *
 * (Aqui ponia que no habia tildes. Era falso: lo saque del comentario de
 * text.h, que se quedo sin actualizar cuando se anadio font_extra. La
 * sonda de glifos lo heredo y me hizo quitar cuatro "ñ" de textos que se
 * dibujaban perfectamente. Ahora la sonda lee la cobertura del propio
 * text.c, que es el unico sitio que no puede mentir.)
 *
 * Frances o aleman pedirian ocho o diez glifos mas -- ç, ä, ö, ü, ß, à,
 * è, ê -- y caben en la misma tabla. Ruso o japones no: eso es dibujar un
 * alfabeto, no traducir.
 */

#ifndef GR33N_I18N_H
#define GR33N_I18N_H

typedef enum {
	IDIOMA_ES = 0,
	IDIOMA_EN,
	IDIOMA_N
} gr33nIdioma;

void        i18nSet(gr33nIdioma i);
gr33nIdioma i18nGet(void);

/* El codigo corto que se guarda en disco: "es", "en".
 *
 * Se guarda el CODIGO y no el numero, por lo mismo que la region: un
 * indice depende del orden de una lista, y el dia que se meta un idioma
 * en medio, el ajuste guardado de todo el mundo pasa a decir otra cosa. */
const char *i18nCodigo(gr33nIdioma i);
gr33nIdioma i18nPorCodigo(const char *cod);

/* Como se llama el idioma, EN SU PROPIO IDIOMA.
 *
 * "Espanol" y "English", no "Espanol" e "Ingles". Quien tiene la interfaz
 * en un idioma que no entiende necesita reconocer el suyo en la lista, y
 * para eso el unico nombre que sirve es el propio. Es lo que hacen todos
 * los sistemas que se toman esto en serio. */
const char *i18nNombre(gr33nIdioma i);

/* Elige. Con en NULL o vacio devuelve es. */
const char *tr(const char *es, const char *en);

#define TR(es_, en_)  tr((es_), (en_))

/* --------------------------------------------------------------------- */
/* El idioma de xCloud: el de LOS JUEGOS                                 */
/*                                                                       */
/* Es un ajuste DISTINTO del de la interfaz, y no por complicar: la       */
/* interfaz tiene dos idiomas porque solo dos caben en la fuente, y       */
/* xCloud tiene veinte. Alguien que juega en japones no quiere que GR33N  */
/* le hable en japones -- entre otras cosas porque no podria: no hay      */
/* glifos.                                                               */
/*                                                                       */
/* Este locale va a DOS sitios y conviene saberlo:                       */
/*                                                                       */
/*   1. settings.locale del POST /play. Es el idioma con el que arranca   */
/*      el juego: voces, textos, teclado.                                 */
/*   2. El parametro language de catalog.gamepass.com, o sea el idioma de */
/*      los nombres y descripciones de la biblioteca.                     */
/*                                                                       */
/* AVISO HONESTO: que se lo pidamos no obliga a nadie. Un juego que no    */
/* este doblado al idioma que pides sale en el que tenga, normalmente     */
/* ingles. xCloud no avisa de eso y nosotros tampoco podemos.            */
/*                                                                       */
/* UNA SOLA LISTA, tres campos por fila. El codigo y sus dos nombres van  */
/* juntos y no hay manera de que se descoloquen. Es la misma forma que    */
/* COMBO_LISTA en ui.c, y por el mismo motivo.                            */
/* --------------------------------------------------------------------- */

/*        codigo    en castellano              en ingles                  */
#define XCLOC_LISTA \
	X("en-US", "Ingles (EEUU)",        "English (US)")            \
	X("en-GB", "Ingles (Reino Unido)", "English (UK)")            \
	X("es-ES", "Espanol (Espana)",     "Spanish (Spain)")         \
	X("es-MX", "Espanol (Mexico)",     "Spanish (Mexico)")        \
	X("fr-FR", "Frances",              "French")                  \
	X("de-DE", "Aleman",               "German")                  \
	X("it-IT", "Italiano",             "Italian")                 \
	X("pt-BR", "Portugues (Brasil)",   "Portuguese (Brazil)")     \
	X("nl-NL", "Neerlandes",           "Dutch")                   \
	X("pl-PL", "Polaco",               "Polish")                  \
	X("ru-RU", "Ruso",                 "Russian")                 \
	X("tr-TR", "Turco",                "Turkish")                 \
	X("ja-JP", "Japones",              "Japanese")                \
	X("ko-KR", "Coreano",              "Korean")                  \
	X("zh-CN", "Chino simplificado",   "Chinese (Simplified)")

int         xclocN(void);
const char *xclocCodigo(int i);   /* "es-ES"                    */
const char *xclocNombre(int i);   /* ya traducido al de la interfaz */
int         xclocPorCodigo(const char *cod);   /* -1 si no esta */

/* El que esta puesto ahora. Lo leen session.c (el /play) y catalog.c (la
 * tienda), asi que vive aqui y no en ui.c: un ajuste que tres modulos
 * necesitan no puede ser privado del que lo dibuja. */
void        xclocSet(int i);
int         xclocGet(void);
const char *xclocActual(void);    /* el codigo, listo para meter en JSON */

#endif /* GR33N_I18N_H */
