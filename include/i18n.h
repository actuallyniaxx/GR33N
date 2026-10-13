/* GR33N - cinco idiomas, sin tabla de identificadores
 *
 * ------------------------------------------------------------------
 * POR QUE NO HAY UN enum DE CADENAS
 * ------------------------------------------------------------------
 *
 * Lo normal seria un identificador por texto (T_AJUSTES, T_CUENTA...) y
 * cinco arrays, uno por idioma. Aqui NO, y por una razon concreta: eso
 * son cinco listas paralelas mantenidas a mano, que es el fallo que este
 * proyecto lleva meses cometiendo en todas sus formas -- la lista de
 * regiones inventada al lado de la de verdad, el combo_name[] junto al
 * combo_btn[], el config.h de opus con las exclusiones a mano. Cada vez
 * que hay dos listas que tienen que ir en el mismo orden, tarde o
 * temprano una se queda atras, y aqui el sintoma seria el peor de todos:
 * un texto que dice algo distinto de lo que hace el boton.
 *
 * Asi que cada cadena lleva sus traducciones AL LADO, en el mismo sitio.
 * No hay indice que pueda apuntar mal porque no hay indice.
 *
 * Sale mas caro en binario (las cinco cadenas viajan siempre) y mas
 * repetitivo de leer. Se acepta: son unos kilobytes de los 256 MB de la
 * consola, y a cambio es imposible que un texto se descoloque.
 *
 * ------------------------------------------------------------------
 * COMO SE PASO DE DOS IDIOMAS A CINCO SIN PODER EQUIVOCARSE
 * ------------------------------------------------------------------
 *
 * Con dos, esto era tr(es, en): dos parametros y ya. Con cinco, la
 * tentacion es tr(es, en, fr, it, pt) -- cinco parametros posicionales, y
 * el dia que alguien se salte una coma, el italiano sale en frances y
 * NADIE SE ENTERA, porque el compilador ve cinco const char * y le
 * parecen todos bien.
 *
 * Por eso TR() construye un ARRAY de tamano fijo, con CINCO PARAMETROS
 * CON NOMBRE:
 *
 *     #define TR(es_, en_, fr_, it_, pt_) \
 *         trPick((const char *const[IDIOMA_N]){ es_, en_, fr_, it_, pt_ })
 *
 * y asi las dos formas de equivocarse son error de compilacion:
 *
 *     TR("a","b","c","d")           -> requires 5 arguments, but only 4 given
 *     TR("a","b","c","d","e","f")   -> passed 6 arguments, but takes just 5
 *
 * OJO CON LA VERSION QUE NO SE USO. Lo primero que escribi fue TR(...)
 * con __VA_ARGS__, apoyandome en que sobrarse da "excess elements in
 * array initializer". Lo comprobe antes de subirlo y resulta que eso es
 * un AVISO, no un error, y este Makefile no lleva -Werror: habria
 * compilado, habria pasado los tests, y se habria subido con la mitad de
 * una fila corrida. Con parametros con nombre lo para el preprocesador,
 * que no tiene avisos: o estan los cinco o no hay binario.
 *
 * La contrapartida es que una cadena sin traducir hay que escribirla NULL
 * a mano. Es mejor asi: un NULL dice "se que esto no esta traducido", y
 * una coma que falta no dice nada.
 *
 * Las tablas usan el MISMO array, declarado como const char *n[IDIOMA_N],
 * y el mismo trPick(). Una sola forma de decir "esto es texto traducido",
 * en el codigo suelto y en las tablas.
 *
 * ------------------------------------------------------------------
 * SIN TRADUCIR SALE EN CASTELLANO, NO EN BLANCO
 * ------------------------------------------------------------------
 *
 * trPick() con NULL o cadena vacia en la casilla del idioma actual
 * devuelve el castellano. Una pantalla a medio traducir se lee raro; una
 * con huecos no se lee.
 *
 * ------------------------------------------------------------------
 * Y POR QUE ESTOS CINCO
 * ------------------------------------------------------------------
 *
 * Por la fuente. text.c lleva un mapa de 5x7 pixeles: ASCII 32..122 mas
 * una tabla font_extra indexada por punto de codigo. Ahi caben las
 * vocales acentuadas de las lenguas latinas -- a, e, i, o, u con tilde,
 * grave y circunflejo, mas la n y la o con virgulilla y la c con
 * cedilla. Son unos treinta dibujos de cinco columnas.
 *
 * Ruso o japones no: eso es dibujar un alfabeto, no traducir. Aleman
 * pediria la ss y cabria, pero nadie lo ha pedido.
 *
 * (En la version de dos idiomas ponia aqui que la fuente no tenia
 * tildes. Era falso: lo saque del comentario de text.h, que se quedo sin
 * actualizar cuando se anadio font_extra. La sonda de glifos lo heredo y
 * me hizo quitar cuatro "n con virgulilla" de textos que se dibujaban
 * perfectamente. Ahora la cobertura la lee t/i18n-fuente.py del propio
 * text.c, que es el unico sitio que no puede mentir.)
 */

#ifndef GR33N_I18N_H
#define GR33N_I18N_H

/* --------------------------------------------------------------------- */
/* LOS IDIOMAS, EN UNA SOLA LISTA                                        */
/*                                                                       */
/* Cuatro columnas por fila, y de esta lista salen TODAS las demas: el    */
/* enum, la tabla de codigos, los nombres y el idioma de juego de         */
/* partida. No hay un segundo sitio que actualizar al anadir uno.         */
/*                                                                       */
/* El nombre va EN SU PROPIO IDIOMA. "Espanol" y "English", no "Espanol"  */
/* e "Ingles". Quien tiene la interfaz en un idioma que no entiende       */
/* necesita reconocer el suyo en la lista, y para eso el unico nombre que */
/* sirve es el propio.                                                   */
/*                                                                       */
/* La ultima columna es el idioma de JUEGO al que se cae mientras nadie   */
/* haya tocado ese ajuste. Vive aqui, pegada a su idioma, y no en un if   */
/* de ui.c: ahi habia un "set_idioma == IDIOMA_EN ? en-US : es-ES" que    */
/* con cinco idiomas habria mandado a franceses, italianos y brasilenos   */
/* a jugar en castellano sin decir nada.                                  */
/*                                                                       */
/* Y VAN CON SUS ACENTOS, que para eso se han dibujado los glifos. Esta   */
/* tabla ponia "Espanol" con un comentario al lado: "sin tilde: la fuente */
/* no la tiene". Era mentira, y de la misma familia que la de arriba:     */
/* font_extra lleva la n con virgulilla desde que se anadio.              */
/*                                                                       */
/* Si alguna de estas cinco sale con un hueco en la pantalla de Ajustes,  */
/* es que falta un glifo en text.c. Es el sitio donde antes se nota.      */
/* --------------------------------------------------------------------- */

/*        ENUM  cod   nombre en su idioma   idioma de juego */
#define IDIOMA_LISTA \
	X(ES,  "es",  "Español",     "es-ES") \
	X(EN,  "en",  "English",     "en-US") \
	X(FR,  "fr",  "Français",    "fr-FR") \
	X(IT,  "it",  "Italiano",    "it-IT") \
	X(PT,  "pt",  "Português",   "pt-BR")

typedef enum {
#define X(e, c, n, l) IDIOMA_##e,
	IDIOMA_LISTA
#undef X
	IDIOMA_N
} gr33nIdioma;

void        i18nSet(gr33nIdioma i);
gr33nIdioma i18nGet(void);

/* El codigo corto que se guarda en disco: "es", "en", "fr", "it", "pt".
 *
 * Se guarda el CODIGO y no el numero, por lo mismo que la region: un
 * indice depende del orden de una lista, y el dia que se meta un idioma
 * en medio, el ajuste guardado de todo el mundo pasa a decir otra cosa.
 *
 * Esto ya ha valido la pena una vez: al pasar de dos idiomas a cinco, el
 * frances entro DETRAS del ingles y los ajustes guardados de todo el
 * mundo siguieron diciendo lo mismo. Con un indice, cualquiera que
 * tuviera el ingles guardado habria abierto GR33N en frances. */
const char *i18nCodigo(gr33nIdioma i);
gr33nIdioma i18nPorCodigo(const char *cod);

/* Como se llama el idioma, EN SU PROPIO IDIOMA. */
const char *i18nNombre(gr33nIdioma i);

/* El idioma de xCloud al que se cae mientras nadie toque ese ajuste. */
const char *i18nJuegoPorDefecto(gr33nIdioma i);

/* --------------------------------------------------------------------- */
/* ELEGIR                                                                */
/* --------------------------------------------------------------------- */

/* v son IDIOMA_N punteros en el orden del enum. Una casilla NULL o vacia
 * cae al castellano; v entero NULL devuelve "". */
const char *trPick(const char *const *v);

/* En el codigo suelto, SIEMPRE las cinco:
 *
 *     TR("Ajustes", "Settings", "Réglages", "Impostazioni", "Configurações")
 *
 * Sobran o faltan -> no compila. Lo que todavia no este traducido va
 * NULL, escrito a mano, y sale en castellano:
 *
 *     TR("Sin juegos", "No games", NULL, NULL, NULL)
 *
 * En las tablas, el mismo array:
 *
 *     static const struct { const char *n[IDIOMA_N]; } filas[] = {
 *         { { "Cuenta", "Account", "Compte", "Account", "Conta" } },
 *     };
 *     ... trPick(filas[i].n) ...
 *
 * Ahi el compilador solo avisa si sobran, asi que las tablas las repasa
 * t/i18n-fuente.py: cuenta las columnas de cada fila y falla si alguna no
 * tiene las cinco, o si alguna esta vacia. Una tabla no se puede proteger
 * con el preprocesador, pero si con algo que la recorra entera.
 */
#define TR(es_, en_, fr_, it_, pt_) \
	trPick((const char *const[IDIOMA_N]){ es_, en_, fr_, it_, pt_ })

/* --------------------------------------------------------------------- */
/* El idioma de xCloud: el de LOS JUEGOS                                 */
/*                                                                       */
/* Es un ajuste DISTINTO del de la interfaz, y no por complicar: la       */
/* interfaz tiene cinco idiomas porque son los que caben en la fuente, y  */
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
/* UNA SOLA LISTA, seis campos por fila: el codigo y sus cinco nombres    */
/* van juntos y no hay manera de que se descoloquen.                      */
/* --------------------------------------------------------------------- */

/*        codigo   castellano              ingles                  frances                 italiano                 portugues (BR)          */
#define XCLOC_LISTA \
	X("en-US", "Inglés (EEUU)",         "English (US)",         "Anglais (É-U)",        "Inglese (USA)",         "Inglês (EUA)")            \
	X("en-GB", "Inglés (Reino Unido)",  "English (UK)",         "Anglais (R-U)",        "Inglese (Regno Unito)", "Inglês (Reino Unido)")    \
	X("es-ES", "Español (España)",      "Spanish (Spain)",      "Espagnol (Espagne)",   "Spagnolo (Spagna)",     "Espanhol (Espanha)")      \
	X("es-MX", "Español (México)",      "Spanish (Mexico)",     "Espagnol (Mexique)",   "Spagnolo (Messico)",    "Espanhol (México)")       \
	X("fr-FR", "Francés",               "French",               "Français",             "Francese",              "Francês")                 \
	X("de-DE", "Alemán",                "German",               "Allemand",             "Tedesco",               "Alemão")                  \
	X("it-IT", "Italiano",              "Italian",              "Italien",              "Italiano",              "Italiano")                \
	X("pt-BR", "Portugués (Brasil)",    "Portuguese (Brazil)",  "Portugais (Brésil)",   "Portoghese (Brasile)",  "Português (Brasil)")      \
	X("nl-NL", "Neerlandés",            "Dutch",                "Néerlandais",          "Olandese",              "Holandês")                \
	X("pl-PL", "Polaco",                "Polish",               "Polonais",             "Polacco",               "Polonês")                 \
	X("ru-RU", "Ruso",                  "Russian",              "Russe",                "Russo",                 "Russo")                   \
	X("tr-TR", "Turco",                 "Turkish",              "Turc",                 "Turco",                 "Turco")                   \
	X("ja-JP", "Japonés",               "Japanese",             "Japonais",             "Giapponese",            "Japonês")                 \
	X("ko-KR", "Coreano",               "Korean",               "Coréen",               "Coreano",               "Coreano")                 \
	X("zh-CN", "Chino simplificado",    "Chinese (Simplified)", "Chinois simplifié",    "Cinese semplificato",   "Chinês simplificado")

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
