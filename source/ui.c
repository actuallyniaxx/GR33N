/* GR33N - interfaz */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ppu-types.h>

#include "gr33n.h"
#include "video.h"
#include "input.h"
#include "link.h"
#include "text.h"
#include "ui.h"
#include "store.h"
#include "catalog.h"
#include "session.h"
#include "auth.h"
#include "ping.h"
#include "i18n.h"

/* --------------------------------------------------------------------- */
/* Paleta                                                                */
/* --------------------------------------------------------------------- */

#define COL_BG        GR33N_RGB( 10,  12,  11)
#define COL_BAR       GR33N_RGB(  6,   8,   7)
#define COL_PANEL     GR33N_RGB( 18,  22,  20)
#define COL_PANEL_HI  GR33N_RGB( 30,  38,  34)
#define COL_LINE      GR33N_RGB( 40,  50,  45)
#define COL_GREEN     GR33N_RGB(  0, 255,  64)
#define COL_GREEN_DIM GR33N_RGB(  0, 110,  40)
#define COL_TEXT      GR33N_RGB(232, 240, 235)
#define COL_TEXT_DIM  GR33N_RGB(128, 145, 136)
#define COL_WARN      GR33N_RGB(255, 180,   0)
#define COL_BLACK     GR33N_RGB(  0,   0,   0)

/* --------------------------------------------------------------------- */
/* Medidas. La superficie es siempre 1280x720 y el RSX la escala a lo que
 * haya, asi que se puede maquetar contra un tamano fijo sin miedo.      */
/* --------------------------------------------------------------------- */

#define W            GR33N_SURFACE_W
#define H            GR33N_SURFACE_H

#define TOPBAR_H     64
#define BOTBAR_H     48
#define CONTENT_Y    (TOPBAR_H + 24)
#define CONTENT_H    (H - CONTENT_Y - BOTBAR_H - 24)

#define LIST_X       32
#define LIST_W       400
#define DET_X        456
#define DET_W        792

#define ROW_H        56
#define MAX_ROWS     (CONTENT_H / ROW_H)

/* Las filas de Ajustes son mas bajas que las de la biblioteca, y no por
 * gusto: ocho filas de 56 se comen 448 de los 560 de contenido y dejan dos
 * lineas para la descripcion, que en estos ajustes no da ni para la
 * primera frase. A 46 quedan cinco. */
#define SET_ROW_H    46

/* EL HUECO DE LA DESCRIPCION SE RESERVA, NO SE PIDE DE LO QUE SOBRE.
 *
 * Antes las filas ocupaban todo lo que querian y el texto se conformaba
 * con el resto: lines = avail / 28. Con diez ajustes salia bien; al meter
 * los dos de idioma quedo sitio para UNA linea y la descripcion se cortaba
 * a mitad de frase. Se veia en la pantalla: "...There are only" y ahi
 * acababa.
 *
 * Ahora es al reves: el texto tiene su sitio fijo y la lista se desplaza,
 * como la biblioteca. Asi el numero de ajustes puede crecer sin que la
 * ayuda se encoja hasta desaparecer -- que es la parte que explica QUE
 * hace cada cosa, o sea justo lo que no puede faltar. */
#define SET_DESC_LINEAS 5
#define SET_DESC_H      (24 + SET_DESC_LINEAS * 28 + 30)  /* raya + texto + ejes */
#define SET_ROWS        ((CONTENT_H - SET_DESC_H) / SET_ROW_H)

#define HINT_FRAMES  240   /* ~4 s de aviso al entrar en un titulo */

/* Repeticion del pad al mantener una direccion */
/* Rejilla de caratulas.
 *
 * UNA linea de nombre y caratulas mas grandes, porque el nombre largo se
 * resuelve desplazandolo en vez de partiendolo en dos.
 *
 * 560 px de contenido dan para tres filas de 186 (160 de caratula + 26 de
 * nombre) con 2 px de sobra. Con 160 de lado caben siete columnas con 16
 * de separacion: 1216 px justos. 21 juegos por pantalla.
 *
 * A escala 2 cada caracter ocupa 12 px, o sea 13 por linea. Casi ningun
 * juego cabe en 13 caracteres, y por eso el seleccionado se desplaza.
 *
 * GRID_CELL es CAT_ART_PX, y no por casualidad: si la caratula guardada
 * midiera otra cosa, gfxBlit tendria que remuestrear en cada fotograma y
 * se veria. */
#define GRID_COLS    7
#define GRID_ROWS    3
#define GRID_CELL    CAT_ART_PX
#define GRID_GAP     16
#define GRID_X       32

/* El nombre empieza 6 px bajo la caratula y mide 14 (siete de glifo por
 * dos de escala), o sea que acaba en +20. Con GRID_NAME_H en 22 el marco
 * del seleccionado -tres pixeles de grosor pegados al borde de abajo-
 * empezaba justo en +19 y se comia la ultima fila del texto. */
#define GRID_NAME_Y  6
#define GRID_NAME_H  26

/* Margen del marco alrededor de la celda, y su altura. Contado desde la
 * cima de la caratula, con el marco empezando en -4:
 *
 *     +166..+179   el nombre (14 px a escala 2)
 *     +180         un pixel de aire
 *     +181..+183   el borde de abajo del marco (grosor 3)
 *     +186         la caratula de la fila siguiente
 *
 * Lo comprueba t/focus.c, que es donde estan los numeros de verdad. */
#define GRID_SEL_PAD 4
#define GRID_SEL_H   (GRID_CELL + GRID_NAME_H + 2)

#define GRID_STEP_X  (GRID_CELL + GRID_GAP)
#define GRID_STEP_Y  (GRID_CELL + GRID_NAME_H)
#define GRID_PAGE    (GRID_COLS * GRID_ROWS)

#define REPEAT_DELAY 22
#define REPEAT_RATE   5

/* --------------------------------------------------------------------- */
/* Catalogo                                                              */
/* --------------------------------------------------------------------- */

/* LAS CINCO PANTALLAS DE PRUEBA SE QUEDAN EN CASTELLANO, y es una
 * decision, no un olvido.
 *
 * Son texto de desarrollo: solo aparecen con Debug puesto, hablan de
 * cellVdec, de generacion de claves RSA y de judder, y quien las lee esta
 * depurando, no jugando. Traducir sesenta lineas de prosa tecnica para una
 * pantalla que un usuario normal no ve nunca es trabajo que rinde en otro
 * sitio.
 *
 * Los tres NULL de cada una son los campos _en, y tr() con NULL devuelve
 * el castellano. O sea que esto no deja huecos en pantalla: deja
 * castellano. Si algun dia se traducen, se rellenan y ya esta. */
static const uiTitle title_video_debug = {
	"VIDEO_DEBUG",
	"VIDEO_DEBUG",
	"GR33N - generado en la consola",
	"Carta de ajuste local. Barras SMPTE para verificar el formato de "
	"textura y el remap de canales, degradado en movimiento para el frame "
	"pacing, barra vertical que barre como detector de judder, y un borde "
	"de 3 pixeles que se come el overscan de la tele si lo hay. No toca la "
	"red: sirve para saber si un problema es de video o de enlace.",
	/* Sin traducir a proposito: ver la nota de abajo. */
	NULL, NULL, NULL,
	TITLE_TESTCARD,
	GR33N_RGB(0, 255, 64)
};

static const uiTitle title_h264_debug = {
	"H264_DEBUG",
	"H264_DEBUG",
	"GR33N - cellVdec, decodificacion por hardware",
	"Clip H.264 empotrado en el binario, decodificado por el hardware de "
	"la consola con cellVdec y volcado en la superficie. Sin red y sin "
	"WebRTC: lo unico que se prueba es el decodificador. El numero que "
	"importa es la latencia de decodificacion, que en el cliente final se "
	"lleva casi todo el presupuesto. Si esto va, el camino a xCloud esta "
	"abierto; si no va, no hay proyecto.",
	/* Sin traducir a proposito: ver la nota de abajo. */
	NULL, NULL, NULL,
	TITLE_H264,
	GR33N_RGB(255, 140, 0)
};

static const uiTitle title_tls_debug = {
	"TLS_DEBUG",
	"TLS_DEBUG",
	"GR33N - mbedTLS contra Microsoft",
	"Una peticion HTTPS de verdad al servidor de identidad de Microsoft, "
	"con verificacion de certificado obligatoria contra las raices "
	"empotradas. Resuelve el nombre, abre el socket, negocia TLS y pide el "
	"documento de descubrimiento de OpenID. Ese documento es el que dice "
	"donde esta el endpoint de codigo de dispositivo, asi que esto no es "
	"solo una prueba de transporte: es el primer paso real del inicio de "
	"sesion. Si el apreton de manos sale, el camino a xCloud esta abierto.",
	/* Sin traducir a proposito: ver la nota de abajo. */
	NULL, NULL, NULL,
	TITLE_TLS,
	GR33N_RGB(120, 160, 255)
};

static const uiTitle title_webrtc_debug = {
	"WEBRTC_DEBUG",
	"WEBRTC_DEBUG",
	"GR33N - despertar libpeer",
	"Arranca la pila entera de WebRTC en la consola y cronometra cada paso. "
	"NO se conecta a nada: no manda un paquete ni negocia nada. Lo que mide "
	"son tres cosas que duelen si se descubren tarde. Cuanto tarda la "
	"generacion de la clave RSA del certificado DTLS, que en un PPU de 2006 "
	"puede ser un rato muy largo. Cuanto dura de verdad una espera de un "
	"milisegundo, porque los contadores heredados cuentan vueltas y no "
	"tiempo. Y si la oferta SDP que sale lleva exactamente lo que xCloud "
	"exige, comprobado sobre el texto generado y no sobre el codigo fuente. "
	"La oferta entera sale por el log remoto, linea a linea.",
	/* Sin traducir a proposito: ver la nota de abajo. */
	NULL, NULL, NULL,
	TITLE_WEBRTC,
	GR33N_RGB(0, 200, 120)
};

/* No esta en el catalogo: se abre desde Ajustes > Cuenta. Sigue siendo un
 * uiTitle porque asi reutiliza la maquina de estados de pantalla completa
 * en vez de inventar un modo nuevo para lo mismo. */
static const uiTitle title_auth = {
	"INICIAR_SESION",
	"Iniciar sesion",
	"Cuenta de Microsoft",
	"Inicio de sesion por codigo de dispositivo. La consola pide un codigo "
	"corto, lo enseña en la tele, y tu lo escribes en el movil o en el PC. "
	"La contrasena no pasa por la consola en ningun momento, que es "
	"exactamente el motivo por el que este flujo existe. Lo que se guarda "
	"aqui es un token de refresco: caduca, se puede revocar desde tu "
	"cuenta, y no sirve para nada mas.",

	"Sign in",
	"Microsoft account",
	"Device code sign-in. The console asks for a short code, shows it on "
	"the TV, and you type it on your phone or PC. Your password never "
	"passes through the console at any point, which is exactly why this "
	"flow exists. What gets stored here is a refresh token: it expires, "
	"you can revoke it from your account, and it is good for nothing "
	"else.",

	TITLE_AUTH,
	GR33N_RGB(0, 220, 160)
};

static const uiTitle title_loop_test = {
	"LOOP_TEST",
	"LOOP_TEST",
	"GR33N - servidor de pruebas",
	"El lazo completo contra el PC. La consola sube el estado del mando a "
	"60 Hz y el servidor devuelve un frame. La cruz blanca la pinta la PS3 "
	"con el input local, sin latencia; el cursor de la rejilla lo pinta el "
	"PC. La distancia entre los dos cuando mueves el stick es la latencia "
	"de punta a punta, en directo.",
	/* Sin traducir a proposito: ver la nota de abajo. */
	NULL, NULL, NULL,
	TITLE_REMOTE,
	GR33N_RGB(0, 200, 255)
};

static const uiTitle *catalog[8];
static int catalog_n = 0;

/* --------------------------------------------------------------------- */
/* Ajustes                                                               */
/* --------------------------------------------------------------------- */

/* Tres formas de ajuste. Antes solo habia casillas y se notaba: la zona
 * muerta no es un si/no y el servidor tampoco. */
typedef enum {
	SET_TOGGLE = 0,   /* casilla                          */
	SET_RANGE,        /* numero con minimo, maximo y paso */
	SET_CHOICE,       /* una opcion de una lista          */
	SET_ACTION        /* no guarda nada: abre una pantalla */
} uiSetKind;

typedef struct {
	/* CADA TEXTO CON SU TRADUCCION AL LADO, no un identificador que
	 * apunte a otra tabla. Ver el comentario largo de i18n.h: dos listas
	 * paralelas en el mismo orden es como se acaba con una fila que dice
	 * una cosa y hace otra. */
	const char *name;
	const char *name_en;
	const char *desc;
	const char *desc_en;
	uiSetKind   kind;
	int        *value;

	int min, max, step;                  /* SET_RANGE  */
	const char *const *options;          /* SET_CHOICE */
	int n_options;

	/* SET_ACTION: que poner a la derecha de la fila. Es una funcion y no
	 * un texto fijo porque lo que hay que enseñar cambia solo — hoy si
	 * hay sesion o no, manana el gamertag. */
	const char *(*status)(void);

	/* 1 = solo se ve con Debug puesto. El tamano y la esquina del panel de
	 * depuracion no le dicen nada a quien no lo tiene encendido. */
	int debug_only;
} uiSetting;

/* --------------------------------------------------------------------- */
/* LOS VALORES DE FABRICA                                                */
/*                                                                       */
/* Esto es lo que se encuentra alguien que arranca GR33N por primera vez */
/* y no toca nada, asi que no son "los que me van bien a mi": son los    */
/* que menos sorprenden a quien no ha leido ni una linea de esto.        */
/*                                                                       */
/* Ingles porque el proyecto se publica y la mayoria de quien lo pruebe  */
/* no habla castellano. Debug apagado porque un panel de estadisticas    */
/* encima del juego, sin haberlo pedido, parece un fallo. Y salir con    */
/* SELECT + Atras porque es lo que hace la propia consola.               */
/* --------------------------------------------------------------------- */

static int set_debug    = 0;
static int set_deadzone = 10;
static int set_swap_ok  = 0;
/* set_server esta mas abajo, junto a la lista de regiones que indexa. */
static int set_hud_scale = 1;   /* 1..3 */
static int set_hud_pos   = 1;   /* arriba a la derecha */
/* De 2531 titulos solo 586 arrancan. Por defecto se ven solo esos: una
 * lista donde cuatro de cada cinco entradas no hacen nada no es una
 * biblioteca. Quien quiera ojear el catalogo entero, que lo encienda. */
static int set_show_all  = 0;
static int set_exit_combo = 0;  /* como se sale de un stream */
/* APAGADO. Estaba encendido, y encendido choca con el valor de fabrica de
 * "Salir de un juego con": si alguien lo cambiara a SELECT + START, salir
 * de una partida le cerraria GR33N entero. Que los dos valores de fabrica
 * no puedan pisarse entre si es lo minimo. */
static int set_quit_app   = 0;

/* Un juego en marcha NO es el menu con una imagen encima.
 *
 * La primera version pintaba el video sobre el menu y dejaba a uiUpdate
 * haciendo lo de siempre: la ficha del juego seguia debajo, asi que Atras
 * la cerraba -- y cerrarla suelta la sesion. Se salia del juego con un
 * boton suelto, en mitad de la partida.
 *
 * Con esto la interfaz sabe que hay un juego delante y se comporta como
 * en cualquier stream: los botones sueltos son del juego y solo sale la
 * combinacion. */
static int live_on = 0;
static int live_exit = 0;

/* Las cuatro esquinas donde se puede pegar el panel de depuracion. Nada
 * mas: este comentario tenia encima el de la lista de regiones, copiado de
 * abajo, que es exactamente el mismo descuido que hacia salir un ping al
 * lado de este ajuste. */
#define HUD_POS_LISTA \
	X("Arriba izq", "Top left")     \
	X("Arriba der", "Top right")    \
	X("Abajo izq",  "Bottom left")  \
	X("Abajo der",  "Bottom right")

static const struct { const char *es, *en; } hud_pos_txt[] = {
#define X(e, i) { e, i },
	HUD_POS_LISTA
#undef X
};
#define HUD_POS_N ((int)(sizeof(hud_pos_txt)/sizeof(hud_pos_txt[0])))

/* Las opciones son punteros que la tabla de ajustes guarda una vez, asi
 * que hay que rehacerlas cuando cambia el idioma. Igual que xcloc_opt. */
static const char *hud_pos_name[HUD_POS_N];

/* ESTE BLOQUE ESTABA 300 LINEAS MAS ABAJO, y rebuild_idiomas() --que
 * necesita combo_txt para traducir los nombres-- esta aqui arriba. El
 * compilador lo dijo con tres errores seguidos: COMBOS_N sin declarar,
 * combo_name sin declarar, combo_txt sin declarar.
 *
 * Tercera vez esta semana que meto un uso por encima de su declaracion.
 * Se sube el bloque entero en vez de declarar cosas a medias, y se deja
 * al lado de las esquinas del panel, que es su vecino natural: las dos
 * son listas de opciones con texto que hay que rehacer al cambiar de
 * idioma. */

/* LAS COMBINACIONES DE SALIDA.
 *
 * Cada una es "mantener esto y pulsar aquello". El modificador se
 * MANTIENE y el otro se PULSA, no al reves: un boton que el juego usa no
 * puede disparar la salida por mantenerlo.
 *
 * Y ninguna usa un boton suelto, por lo mismo. En un stream de xCloud
 * cualquier boton es del juego.
 *
 * UNA SOLA LISTA, DOS ARRAYS. El ajuste necesita los nombres como array
 * de cadenas (asi es el campo `options`) y el codigo necesita las
 * mascaras, y son cosas distintas. Escribirlas por separado es la lista
 * copiada a mano que se desincroniza -- y aqui desincronizarse significa
 * "el cartel dice una combinacion y sale con otra", o peor, una entrada
 * sin mascara que se dispara con un boton suelto.
 *
 * Con la lista en un macro no hay dos sitios que mantener: los dos arrays
 * se generan del mismo texto y no pueden diferir ni en orden ni en
 * numero. */
/* "Atras" es el boton de cancelar, que depende del ajuste de intercambiar
 * X y O -- por eso se traduce y no se pone "O" a secas. Los demas son
 * nombres de botones fisicos y no se traducen: L1 es L1 en todas partes. */
#define COMBO_LISTA \
	X("SELECT + Atras",  "SELECT + Back",     GR33N_BTN_SELECT,            0)                \
	X("SELECT + START",  "SELECT + START",    GR33N_BTN_SELECT,            GR33N_BTN_START)  \
	X("L1 + R1 + START", "L1 + R1 + START",   GR33N_BTN_L1 | GR33N_BTN_R1, GR33N_BTN_START)  \
	X("L3 + R3",         "L3 + R3",           GR33N_BTN_L3,                GR33N_BTN_R3)

static const struct { const char *es, *en; } combo_txt[] = {
#define X(e, i, h, p) { e, i },
	COMBO_LISTA
#undef X
};

static const char *combo_name[4];

/* pulsar == 0 quiere decir "el boton de Atras", que depende del ajuste de
 * intercambiar X y O. */
static const struct { u32 mantener, pulsar; } combo_btn[] = {
#define X(e, i, h, p) { (h), (p) },
	COMBO_LISTA
#undef X
};

#define COMBOS_N ((int)(sizeof(combo_txt)/sizeof(combo_txt[0])))

/* LAS REGIONES.
 *
 * Aqui habia una lista inventada con siete nombres bonitos y una latencia
 * de relleno, esperando "a que hubiera con que rellenarla". Y habia con
 * que desde el primer login: Microsoft manda TODAS las regiones en la
 * respuesta, auth.c las escribia en el log y las tiraba. Una lista falsa
 * al lado de la de verdad, que es la version de este proyecto de las dos
 * listas paralelas mantenidas a mano.
 *
 * Ahora la lista es la del servidor y no hay otra. Mientras no haya
 * sesion iniciada solo esta "Automatico", que es la respuesta honesta:
 * sin autenticarse no se sabe ni que regiones existen ni cual te toca. */
#define SERVERS_MAX (1 + XC_REGIONS_MAX)

static const char *server_opt[SERVERS_MAX];
static char        server_txt[SERVERS_MAX][72];
static int         server_opt_n = 1;

/* Lo que se guarda en disco es el NOMBRE, no el indice.
 *
 * Un indice depende del orden en que Microsoft devuelva las regiones, y
 * ese orden no es un contrato: el dia que cambie, un "3" guardado elige
 * otra region distinta sin avisar. Cadena vacia = automatico. */
static char set_server_nom[64] = "";

/* Nombres en castellano para las regiones que se conocen. Es SOLO
 * cosmetica: la que no este en esta tabla sale con el nombre que dice
 * Microsoft, que es preferible a inventarse una traduccion. El log
 * siempre lleva el nombre original. */
static const struct { const char *ms; const char *es; } region_es[] = {
	{ "WestEurope",         "Europa Oeste"        },
	{ "NorthEurope",        "Europa Norte"        },
	{ "UKSouth",            "Reino Unido Sur"     },
	{ "UKWest",             "Reino Unido Oeste"   },
	{ "FranceCentral",      "Francia Centro"      },
	{ "GermanyWestCentral", "Alemania Oeste"      },
	{ "SwedenCentral",      "Suecia Centro"       },
	{ "ItalyNorth",         "Italia Norte"        },
	{ "SpainCentral",       "Espana Centro"       },
	{ "PolandCentral",      "Polonia Centro"      },
	{ "EastUS",             "EEUU Este"           },
	{ "EastUS2",            "EEUU Este 2"         },
	{ "WestUS",             "EEUU Oeste"          },
	{ "WestUS2",            "EEUU Oeste 2"        },
	{ "SouthCentralUS",     "EEUU Centro Sur"     },
	{ "NorthCentralUS",     "EEUU Centro Norte"   },
	{ "WestCentralUS",      "EEUU Centro Oeste"   },
	{ "CanadaCentral",      "Canada Centro"       },
	{ "CanadaEast",         "Canada Este"         },
	{ "BrazilSouth",        "Brasil Sur"          },
	{ "MexicoCentral",      "Mexico Centro"       },
	{ "JapanEast",          "Japon Este"          },
	{ "JapanWest",          "Japon Oeste"         },
	{ "KoreaCentral",       "Corea Centro"        },
	{ "EastAsia",           "Asia Este"           },
	{ "SoutheastAsia",      "Asia Sudeste"        },
	{ "CentralIndia",       "India Centro"        },
	{ "AustraliaEast",      "Australia Este"      },
	{ "AustraliaSouthEast", "Australia Sudeste"   },
	{ "UAENorth",           "Emiratos Norte"      },
	{ "SouthAfricaNorth",   "Sudafrica Norte"     }
};
#define REGION_ES_N ((int)(sizeof(region_es)/sizeof(region_es[0])))

/* SIN MIRAR MAYUSCULAS NI MINUSCULAS.
 *
 * La tabla dice "SwedenCentral", que es como aparece en la documentacion
 * de Azure. Lo que manda el login de xCloud es "SWEDENCENTRAL", en
 * mayusculas. strcmp no encontraba ni una y todas las regiones salian con
 * el nombre crudo -- se veia en la pantalla: "best: SWEDENCENTRAL".
 *
 * Comparar sin caja no cuesta nada y quita la dependencia de acertar
 * exactamente como lo escribe Microsoft hoy. */
static int igual_sin_caja(const char *a, const char *b)
{
	for (; *a && *b; a++, b++) {
		char x = *a, y = *b;
		if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
		if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
		if (x != y) return 0;
	}
	return *a == *b;
}

static const char *region_bonito(const char *ms)
{
	int i;

	for (i = 0; i < REGION_ES_N; i++)
		if (igual_sin_caja(region_es[i].ms, ms)) return region_es[i].es;

	return ms;
}

/* set_server: 0 = automatico, 1..n = la region n-1 de auth. */
static int set_server   = 0;

/* --------------------------------------------------------------------- */
/* Los dos idiomas                                                       */
/* --------------------------------------------------------------------- */

/* El de la interfaz. Se guarda el CODIGO ("es"/"en"), no el indice, por lo
 * mismo que la region: el dia que se meta un idioma en medio, un numero
 * guardado pasa a significar otro. */
static int  set_idioma = IDIOMA_EN;

/* El de los juegos. Indice dentro de XCLOC_LISTA; tambien se guarda por
 * codigo ("es-ES"). */
static int  set_juego_loc = 0;
static char set_juego_cod[16] = "";

/* Techo para el campo n_options de la tabla, que tiene que ser constante
 * al compilar. El numero de verdad lo da xclocN() a traves de opts_n(). */
#define XCLOC_MAX 32

static const char *idioma_opt[IDIOMA_N];
static const char *xcloc_opt[XCLOC_MAX];
static char        xcloc_txt[XCLOC_MAX][40];

/* Los nombres de los idiomas de xCloud estan traducidos, asi que cambian
 * cuando cambia el idioma de la interfaz. Se rehacen ahi mismo. */
static void rebuild_idiomas(void)
{
	int i, n = xclocN();

	for (i = 0; i < IDIOMA_N; i++)
		idioma_opt[i] = i18nNombre((gr33nIdioma)i);

	if (n > XCLOC_MAX) n = XCLOC_MAX;

	for (i = 0; i < n; i++) {
		snprintf(xcloc_txt[i], sizeof(xcloc_txt[0]), "%s", xclocNombre(i));
		xcloc_opt[i] = xcloc_txt[i];
	}

	/* Las de mas, si algun dia hubiera, apuntan a algo valido en vez de a
	 * NULL: quien pinta lee con un indice que puede venir de un fichero
	 * viejo. */
	for (; i < XCLOC_MAX; i++) xcloc_opt[i] = xcloc_txt[0];

	/* Y las otras dos listas de opciones, que tambien llevan texto. Van
	 * aqui y no en su sitio para que no se pueda traducir la interfaz y
	 * dejarse una lista a medias: un solo sitio que las rehace todas. */
	for (i = 0; i < HUD_POS_N; i++)
		hud_pos_name[i] = tr(hud_pos_txt[i].es, hud_pos_txt[i].en);

	for (i = 0; i < COMBOS_N; i++)
		combo_name[i] = tr(combo_txt[i].es, combo_txt[i].en);
}

/* Aplica los dos ajustes de idioma a quien corresponda. */
static void aplicar_idioma(void)
{
	i18nSet((gr33nIdioma)set_idioma);

	/* MIENTRAS NADIE HAYA ELEGIDO, EL JUEGO SIGUE A LA INTERFAZ.
	 *
	 * Quien pone la interfaz en ingles casi seguro quiere los juegos en
	 * ingles, y hacerle cambiar dos ajustes para lo mismo es tonto. En
	 * cuanto toca el de los juegos una vez, deja de seguir: a partir de
	 * ahi manda el suyo, que es lo que ha dicho que quiere.
	 *
	 * set_juego_cod vacio = nunca lo ha tocado. */
	if (set_juego_cod[0] == '\0') {
		int i = xclocPorCodigo(set_idioma == IDIOMA_EN ? "en-US" : "es-ES");
		if (i >= 0) set_juego_loc = i;
	}

	xclocSet(set_juego_loc);
	rebuild_idiomas();
}

/* LA UNICA ARITMETICA DE ESTE AJUSTE, en un sitio.
 *
 * set_server cuenta desde 1 porque el 0 es "Automatico"; auth cuenta
 * desde 0 porque todas sus entradas son regiones. Ese desfase de uno
 * estaba escrito en dos sitios --el que aplica y el que pinta el ping--
 * y dos copias de la misma cuenta es como empiezan las que no cuadran.
 *
 * Es pura y no toca nada: se prueba en el PC, en t/region.c.
 * Devuelve -1 si no hay ninguna region. */
static int idx_region(int set, int n_regiones, int por_defecto)
{
	if (n_regiones <= 0) return -1;

	/* Una eleccion explicita y valida manda. */
	if (set > 0 && set <= n_regiones) return set - 1;

	/* Automatico, o un indice que se ha quedado fuera porque la lista
	 * ha encogido: la que diga Microsoft. */
	if (por_defecto >= 0 && por_defecto < n_regiones) return por_defecto;

	return 0;
}

/* Le dice a auth.c donde se juega. Se llama al cambiar el ajuste y al
 * cargarlo, no en cada fotograma: escribe estado compartido. */
static void aplicar_region(void)
{
	int idx = idx_region(set_server, (int)authRegionsN(),
	                     authRegionDefault());

	if (idx >= 0) authSetRegion(idx);
}

/* Rehace la lista de opciones a partir de la que tiene auth. */
static void rebuild_regiones(void)
{
	u32 n = authRegionsN();
	u32 i;

	snprintf(server_txt[0], sizeof(server_txt[0]), "%s",
	         TR("Automatico", "Automatic"));
	server_opt[0] = server_txt[0];
	server_opt_n = 1;

	for (i = 0; i < n && server_opt_n < SERVERS_MAX; i++) {
		xcRegion r;

		if (authRegionCopy((int)i, &r) != 0) continue;

		snprintf(server_txt[server_opt_n], sizeof(server_txt[0]),
		         "%s", region_bonito(r.name));
		server_opt[server_opt_n] = server_txt[server_opt_n];
		server_opt_n++;
	}

	/* Y ahora se resuelve lo que habia guardado. Esto es lo que no podia
	 * hacerse al cargar los ajustes: en ese momento no habia lista. */
	if (set_server_nom[0]) {
		int idx = authRegionPorNombre(set_server_nom);

		if (idx >= 0) {
			set_server = idx + 1;
		} else if (n > 0) {
			/* La region guardada ya no existe, o es de otra cuenta.
			 * Se dice y se vuelve a automatico: dejar el indice viejo
			 * seria elegir a ciegas otra region. */
			linkLog("[ui] la region guardada (%s) ya no esta: automatico",
			        set_server_nom);
			set_server_nom[0] = '\0';
			set_server = 0;
		}
	}

	if (set_server >= server_opt_n) set_server = 0;

	aplicar_region();
}

/* Se rehace cuando cambia la lista de auth, no en cada fotograma. Se mira
 * el numero Y el primer nombre: cerrar sesion y entrar con otra cuenta
 * puede dar la misma cantidad de regiones en otro orden, y entonces el
 * indice apunta a otro sitio sin que el numero se entere.
 *
 * Lo del nombre cuesta un candado y copiar 380 bytes, asi que no se hace
 * sesenta veces por segundo: el numero se mira siempre --es lo que cambia
 * al iniciar sesion, que es el caso de verdad-- y el nombre dos veces por
 * segundo. Una lista de regiones no cambia mas a menudo que eso ni
 * queriendo. */
static void regiones_al_dia(void)
{
	static u32  ultimas_n = 0xffffffffu;
	static char ultima_0[64] = "\1";
	static int  cuenta = 0;
	xcRegion r;
	u32 n = authRegionsN();

	if (n != ultimas_n) goto rehacer;

	if (++cuenta < 30) return;
	cuenta = 0;

	memset(&r, 0, sizeof(r));
	if (n > 0) authRegionCopy(0, &r);
	if (strcmp(r.name, ultima_0) == 0) return;

rehacer:
	memset(&r, 0, sizeof(r));
	if (n > 0) authRegionCopy(0, &r);

	ultimas_n = n;
	snprintf(ultima_0, sizeof(ultima_0), "%s", r.name);

	rebuild_regiones();
}

/* Lo que se ve a la derecha de la fila de Cuenta. Cuando exista el
 * gamertag ira aqui, y no habra que tocar nada mas. */
static const char *account_status(void)
{
	const authInfo *a = authStatus();

	if (a->state == AUTH_WAITING || a->state == AUTH_REQUESTING)
		return TR("entrando...", "signing in...");

	return authHaveToken() ? TR("sesion iniciada", "signed in")
	                       : TR("sin iniciar", "not signed in");
}

static int set_dummy = 0;   /* SET_ACTION no guarda valor, pero el campo existe */

/* El segundo SET_ACTION. El campo `value` no guarda nada en las acciones,
 * pero SI sirve para saber cual es cual: la fila se identifica por la
 * direccion de su variable, igual que ya se hace con set_server para la
 * latencia. Un segundo boton apuntando a set_dummy abriria la pantalla de
 * la cuenta. */
static int set_ping = 0;

static const char *ping_status(void)
{
	static char txt[40];
	int h = 0, t = 0;

	switch (pingEstado()) {
	case PING_MIDIENDO:
		pingProgreso(&h, &t);
		snprintf(txt, sizeof(txt), "%s %d/%d",
		         TR("midiendo", "measuring"), h, t);
		return txt;

	case PING_HECHO: {
		int mej = pingMejor();
		xcRegion r;

		if (mej >= 0 && authRegionCopy(mej, &r) == 0) {
			snprintf(txt, sizeof(txt), "%s %s",
			         TR("mejor:", "best:"), region_bonito(r.name));
			return txt;
		}
		return TR("ninguna contesto", "none answered");
	}

	default:
		return authRegionsN() ? TR("medir", "measure")
		                      : TR("hace falta cuenta", "sign in first");
	}
}


/* 1 si el mando esta haciendo la combinacion elegida ahora mismo. */
static int combo_hecho(const gr33nPad *pad)
{
	int i = (set_exit_combo >= 0 && set_exit_combo < COMBOS_N)
	        ? set_exit_combo : 0;
	u32 mantener = combo_btn[i].mantener;
	u32 pulsar   = combo_btn[i].pulsar ? combo_btn[i].pulsar : uiBtnBack();

	/* Sin modificador no hay combinacion. Esto no deberia pasar nunca --
	 * las cuatro lo tienen-- pero si alguna vez pasara, lo de arriba
	 * seria "(held & 0) == 0", que es SIEMPRE cierto: saldria del juego
	 * con un boton suelto. Un fallo asi no se ve leyendo la tabla. */
	if (mantener == 0) return 0;

	return (pad->held & mantener) == mantener &&
	       (pad->pressed & pulsar) != 0;
}

static const uiSetting settings[] = {
	{
		"Cuenta",
		"Account",
		"Entra con tu cuenta de Microsoft. La consola te da un codigo "
		"corto y tu lo escribes en el movil: la contrasena no pasa por "
		"aqui en ningun momento, que es justo para lo que sirve este "
		"metodo. Lo que se guarda en la PS3 es un permiso que caduca y "
		"que puedes retirar cuando quieras desde tu cuenta.",
		"Sign in with your Microsoft account. The console gives you a "
		"short code and you type it on your phone: your password never "
		"passes through here, which is exactly what this method is for. "
		"What stays on the PS3 is a permission that expires, and that you "
		"can revoke from your account whenever you like.",
		SET_ACTION, &set_dummy, 0, 0, 0, NULL, 0, account_status, 0
	},
	{
		"Idioma",
		"Language",
		"El idioma de GR33N. El de los juegos es el ajuste de abajo. "
		"Solo hay dos porque la fuente esta dibujada a mano, pixel a "
		"pixel: llega para el castellano y el ingles, y anadir ruso o "
		"japones seria dibujar un alfabeto entero.",
		"The language GR33N speaks. Your games get their own setting, "
		"just below. There are only two because the font is drawn by "
		"hand, pixel by pixel: it covers Spanish and English, and adding "
		"Russian or Japanese would mean drawing a whole alphabet.",
		SET_CHOICE, &set_idioma, 0, IDIOMA_N - 1, 1,
		idioma_opt, IDIOMA_N, NULL, 0
	},
	{
		"Idioma de los juegos",
		"Game language",
		"En que idioma arrancan los juegos: voces, textos y teclado. "
		"Tambien decide el idioma de los nombres y descripciones de la "
		"biblioteca. Ojo, que pedirlo no lo garantiza: un juego que no "
		"este doblado saldra en el idioma que tenga, normalmente ingles. "
		"Se aplica a la proxima partida, no a la que ya este en marcha.",
		"What language your games start in: voices, text and keyboard. "
		"It also sets the language of the names and descriptions in your "
		"library. Careful though, asking is not the same as getting: a "
		"game with no dub will fall back to whatever it has, usually "
		"English. It applies to your next session, not one already "
		"running.",
		SET_CHOICE, &set_juego_loc, 0, XCLOC_MAX - 1, 1,
		xcloc_opt, XCLOC_MAX, NULL, 0
	},
	{
		"Zona muerta",
		"Stick deadzone",
		"Cuanto hay que mover el stick antes de que cuente. Los mandos "
		"con anos encima no vuelven del todo al centro: si el cursor se "
		"va solo, sube esto; si los movimientos suaves no responden, "
		"bajalo. Abajo tienes los cuatro ejes en vivo -- suelta el mando "
		"y sube el valor hasta que se queden todos en verde.",
		"How far the stick has to move before it counts. Pads with a few "
		"years on them do not quite return to centre: if the cursor "
		"drifts on its own, raise this; if gentle movements stop "
		"responding, lower it. The four axes are shown live below -- put "
		"the pad down and raise the value until they all sit green.",
		SET_RANGE, &set_deadzone, 0, 48, 2, NULL, 0, NULL, 0
	},
	{
		"Intercambiar X y O",
		"Swap X and O",
		"En PS3 se acepta con X y se cancela con O. Si vienes de una "
		"consola donde es al reves, o de un mando de Xbox, esto lo cambia "
		"en todo el menu. A los juegos les sigue llegando lo que pulses "
		"de verdad.",
		"On PS3 you confirm with X and cancel with O. If you are coming "
		"from a console where it is the other way round, or from an Xbox "
		"pad, this flips it across the whole menu. Your games still "
		"receive whatever you actually pressed.",
		SET_TOGGLE, &set_swap_ok, 0, 0, 0, NULL, 0, NULL, 0
	},
	{
		"Servidor",
		"Server",
		"A que centro de datos de xCloud te conectas. Elegir el que te "
		"pilla lejos puede costarte 80 ms antes de empezar a jugar. La "
		"lista te la da Microsoft al iniciar sesion, asi que sin cuenta "
		"aqui solo veras Automatico. Los milisegundos son lo que tarda en "
		"contestarte la region: no son la latencia jugando, pero son "
		"buena parte de ella.",
		"Which xCloud data centre you connect to. Picking a far one can "
		"cost you 80 ms before you even start playing. Microsoft gives us "
		"the list when you sign in, so with no account you will only see "
		"Automatic here. The milliseconds are how long the region takes "
		"to answer: not the latency you feel in game, but a good chunk "
		"of it.",
		SET_CHOICE, &set_server, 0, SERVERS_MAX - 1, 1,
		server_opt, SERVERS_MAX, NULL, 0
	},
	{
		"Medir latencia",
		"Measure latency",
		"Vuelve a medir todas las regiones. Se hace solo al iniciar "
		"sesion; esto es para repetirlo cuando cambie la red, cuando "
		"alguien se ponga a descargar algo, o simplemente por curiosidad. "
		"Tres medidas por region y se queda la mejor, porque el ruido de "
		"red solo suma.",
		"Measures every region again. It runs on its own when you sign "
		"in; this is for repeating it when your network changes, when "
		"somebody starts a download, or just out of curiosity. Three "
		"measurements per region and the best one wins, because network "
		"noise only ever adds.",
		SET_ACTION, &set_ping, 0, 0, 0, NULL, 0, ping_status, 0
	},
	{
		"Mostrar juegos no disponibles",
		"Show unavailable games",
		"Por defecto la biblioteca solo ensena lo que puedes jugar: todo "
		"lo que veas, arranca. Con esto puesto salen tambien los que no "
		"entran en tu suscripcion, con un candado. Va bien para ojear el "
		"catalogo entero, pero hay muchos mas de los que puedes jugar.",
		"By default your library only shows what you can play: "
		"everything you see will start. Turn this on and the ones "
		"outside your subscription appear too, with a padlock. Handy for "
		"browsing the whole catalogue, though there are a lot more of "
		"them than there are games you can play.",
		SET_TOGGLE, &set_show_all, 0, 0, 0, NULL, 0, NULL, 0
	},
	{
		"Salir de un juego con",
		"Quit a game with",
		"Que combinacion cierra un juego. Nunca es un boton suelto, y con "
		"razon: mientras juegas, cualquier boton es del juego, asi que "
		"uno solo te sacaria de la partida cada vez que lo pulsaras. Se "
		"mantiene el primero y se pulsa el segundo. Si eliges SELECT + "
		"START, desactiva el ajuste de abajo o se pisaran.",
		"Which combination closes a game. Never a single button, and for "
		"good reason: while you play, every button belongs to the game, "
		"so one on its own would end your session every time you pressed "
		"it. Hold the first, press the second. If you pick SELECT + "
		"START, turn off the setting below or the two will collide.",
		SET_CHOICE, &set_exit_combo, 0, COMBOS_N - 1, 1,
		combo_name, COMBOS_N, NULL, 0
	},
	{
		"Cerrar GR33N con START + SELECT",
		"Quit GR33N with START + SELECT",
		"El atajo para volver al XMB desde cualquier sitio. Quitalo si te "
		"estorba, y quitalo seguro si has puesto SELECT + START para "
		"salir de los juegos: si no, salir de una partida te cerraria "
		"GR33N entero. Con esto apagado se sale por el XMB, como con "
		"cualquier juego.",
		"The shortcut back to the XMB from anywhere. Turn it off if it "
		"gets in your way, and definitely turn it off if you set SELECT "
		"+ START as your game exit: otherwise leaving a game would close "
		"GR33N as well. With this off you quit through the XMB, like any "
		"other game.",
		SET_TOGGLE, &set_quit_app, 0, 0, 0, NULL, 0, NULL, 0
	},
	{
		"Debug",
		"Debug",
		"Enseña un panel con fps, estado de la red y rendimiento del "
		"decodificador, y manda lo mismo al servidor de depuracion del "
		"PC, que guarda cada sesion en un fichero. Tambien anade las "
		"pantallas de prueba a la biblioteca. Si algo va mal, esto es lo "
		"que deja rastro.",
		"Shows a panel with fps, network state and decoder performance, "
		"and sends the same data to the PC debug server, which keeps "
		"every session in a file. It also adds the test screens to your "
		"library. When something goes wrong, this is what leaves a "
		"trail.",
		SET_TOGGLE, &set_debug, 0, 0, 0, NULL, 0, NULL, 0
	},

	/* Los dos de abajo son del panel de depuracion, asi que van DEBAJO de
	 * Debug y solo se ven con Debug puesto. Estaban en medio de los
	 * ajustes normales, con nombres que no decian de que panel hablaban, y
	 * ofreciendo mover una cosa que quien lee eso ni siquiera ve. */
	{
		"Tamaño del panel de depuracion",
		"Debug panel size",
		"Como de grande sale el panel de estadisticas. A 1 cabe todo pero "
		"hay que acercarse a la tele; a 3 se lee desde el sofa pero tapa "
		"media pantalla. Solo afecta a ese panel.",
		"How big the statistics panel is. At 1 everything fits but you "
		"have to walk up to the TV; at 3 you can read it from the sofa "
		"but it covers half the screen. It only affects that panel.",
		SET_RANGE, &set_hud_scale, 1, 3, 1, NULL, 0, NULL, 1
	},
	{
		"Esquina del panel de depuracion",
		"Debug panel corner",
		"En que esquina se pega el panel de estadisticas. Util cuando lo "
		"que quieres mirar esta justo debajo: mueves el panel en vez de "
		"la cabeza.",
		"Which corner the statistics panel sticks to. Useful when the "
		"thing you want to look at is right underneath it: move the "
		"panel instead of your head.",
		SET_CHOICE, &set_hud_pos, 0, HUD_POS_N - 1, 1,
		hud_pos_name, HUD_POS_N, NULL, 1
	}
};
#define SETTINGS_N ((int)(sizeof(settings)/sizeof(settings[0])))

/* CUANTAS OPCIONES TIENE DE VERDAD UNA FILA DE ELECCION.
 *
 * settings[] es const y se queda const: es una tabla de descripcion, no
 * de estado, y hacerla escribible para un solo campo abre la puerta a que
 * cualquiera la toque. Pero la lista de regiones cambia al iniciar
 * sesion, asi que para esa fila el n_options de la tabla es un MAXIMO y
 * el numero de verdad esta en server_opt_n.
 *
 * Se pregunta aqui, en un sitio, y no en los tres que lo necesitan: la
 * ultima vez que este proyecto tuvo dos cuentas del mismo conjunto
 * mantenidas por separado, una de las dos se quedo atras. */
static int opts_n(const uiSetting *st)
{
	if (st->value == &set_server)     return server_opt_n;
	if (st->value == &set_juego_loc)  return xclocN();
	return st->n_options;
}

/* Que filas se ven ahora mismo, en orden.
 *
 * set_sel es un indice de ESTA lista, no de settings[]. Con un indice
 * absoluto, apagar Debug dejaria el cursor apuntando a una fila que ya no
 * se pinta: el marco se quedaria en un sitio y las flechas cambiarian otra
 * cosa. Es la misma clase de estado sin invalidar que hacia que la
 * biblioteca se quedara vacia. */
static int settings_vis[SETTINGS_N];
static int settings_vis_n = 0;
static int set_top = 0;      /* primera fila que se ve */

static void rebuild_settings(void)
{
	int i;

	settings_vis_n = 0;

	for (i = 0; i < SETTINGS_N; i++)
		if (!settings[i].debug_only || set_debug)
			settings_vis[settings_vis_n++] = i;
}

/* --------------------------------------------------------------------- */
/* Estado                                                                */
/* --------------------------------------------------------------------- */

/* La pestana de pruebas solo existe con Debug puesto. Al meter la rejilla
 * de xCloud los titulos de prueba se quedaron sin sitio desde donde
 * lanzarse - y eso es justo lo que se usa para probar el decodificador y
 * la pantalla de cuenta. Aqui vuelven, en su propia pestana, y quien no
 * tenga Debug no la ve. */
typedef enum {
	TAB_LIBRARY = 0, TAB_FAVS, TAB_SETTINGS, TAB_DEBUG, TAB_COUNT
} uiTab;

static int set_debug_fwd(void);
#define TAB_N  (set_debug_fwd() ? TAB_COUNT : TAB_COUNT - 1)

static uiTab  tab = TAB_LIBRARY;

/* Vista de la biblioteca. Empieza en la rejilla de xCloud en cuanto haya
 * catalogo; con X se entra en la ficha y con Atras se vuelve. */
static int grid_sel = 0;
static int grid_top = 0;      /* primera FILA visible */
static int in_ficha = 0;
static int desc_scroll = 0;
static int desc_max = 0;      /* lo calcula draw_ficha al pintar */
static int desc_hold = 0;
static int ficha_msg = 0;

/* Filtro. Por defecto solo lo que se puede jugar ahora: de 2531 titulos
 * solo 586 arrancan, y una lista donde cuatro de cada cinco entradas no
 * hacen nada no es una biblioteca, es un catalogo de la tienda. */
#define show_all set_show_all

/* Indices de titulos que pasan el filtro. Se reconstruye cuando cambia
 * algo, no en cada fotograma: recorrer 2531 sesenta veces por segundo
 * seria el mismo error que el fopen de la barra superior. */
static u32 *view = NULL;
static u32  view_n = 0;
static int  view_dirty = 1;

/* Lo que el catalogo tenia la ultima vez que se reconstruyo la vista. Si
 * ha cambiado, lo que hay en pantalla es de antes. */
static u32  view_gen = 0;
static u32  view_det = 0;

/* Filtro por genero. La lista se construye del propio catalogo segun van
 * llegando los datos; el indice 0 es siempre "todos". */
#define GENRES_MAX 32
static char genres[GENRES_MAX][64] = { "Todos" };
static int  genres_n = 1;          /* [0] = todos */
static int  genre_sel = 0;
static uiTab last_tab = TAB_LIBRARY;
static int favs_loaded = 0;
static u32 favs_gen = 0;

static int set_debug_fwd(void) { return set_debug; }

/* Favoritos: guardados como una lista de identificadores en su propio
 * fichero. No van en config.cfg porque eso son ajustes de un puñado de
 * lineas y esto puede ser una lista larga. */
#define FAVS_FILE  "favoritos.txt"

/* Definidas junto a la rejilla, mas abajo. */
static void rebuild_view(void);
static void rebuild_genres(void);
static void toggle_fav(void);
static int    lib_sel = 0;
static int    lib_top = 0;
static int    set_sel = 0;
static uiMode mode = UI_MENU;
static const uiTitle *active = NULL;

static u32 frame_counter = 0;
static u32 stream_entered = 0;

/* Perfil de Xbox Live. Vacio hasta que alguien llame a uiSetProfile: se
 * dibuja solo si hay algo que dibujar, que es mejor que un hueco con un
 * interrogante. */
static char prof_tag[24] = "";
static u32  prof_score = 0;
static int  prof_have = 0;

/* Avatar ya decodificado a ARGB, o NULL. Lo pone uiSetProfilePic cuando
 * cellPngDec lo haya masticado. */
static const u32 *prof_pic = NULL;
static u32 prof_pic_w = 0, prof_pic_h = 0;

static u32 repeat_hold = 0;   /* mascara de la direccion mantenida */
static u32 repeat_ticks = 0;

/* Ultima lectura de los sticks, para poder pintarla mientras se calibra
 * la zona muerta. Calibrar a ciegas es adivinar. */
static s8 pad_lx = 0, pad_ly = 0, pad_rx = 0, pad_ry = 0;

/* Los ajustes se escriben a disco cuando se sale de la pestana o de la
 * aplicacion, no en cada pulsacion: mover un slider no deberia castigar
 * al disco duro veinte veces. */
static int set_dirty = 0;

/* 1 si no habia fichero de ajustes, o sea si es la primera vez que se
 * arranca en esta consola. */
static int first_run = 1;

/* --------------------------------------------------------------------- */

/* Mantiene la seleccion dentro de la ventana visible.
 *
 * Ojo con la forma de escribirlo: la version anterior comparaba
 * "sel >= top + rows" y ppu-gcc (7.2) suelta un -Wstrict-overflow por el
 * patron (X + c) < X. Con la resta no se queja, y encima se lee mejor.
 * GCC 15 no avisaba de esto: las heuristicas cambiaron entre medias. */
static void clamp_scroll(int sel, int count, int rows, int *top)
{
	if (count <= rows) { *top = 0; return; }

	if (sel < *top)              *top = sel;
	else if (sel - *top >= rows) *top = sel - rows + 1;

	if (*top > count - rows) *top = count - rows;
	if (*top < 0)            *top = 0;
}

static void rebuild_catalog(void)
{
	const linkInfo *lk = linkStatus();

	catalog_n = 0;

	if (set_debug) {
		catalog[catalog_n++] = &title_video_debug;
		catalog[catalog_n++] = &title_h264_debug;
		catalog[catalog_n++] = &title_tls_debug;
		catalog[catalog_n++] = &title_webrtc_debug;
	}

	if (lk->state == LINK_UP)
		catalog[catalog_n++] = &title_loop_test;

	if (lib_sel >= catalog_n) lib_sel = catalog_n - 1;
	if (lib_sel < 0) lib_sel = 0;

	clamp_scroll(lib_sel, catalog_n, MAX_ROWS, &lib_top);
}

void uiInit(void)
{
	/* Antes de nada: la rejilla y las pestanas llevan texto, y uiInit las
	 * arma. Sin esto el primer fotograma sale en el idioma de la variable
	 * sin aplicar. */
	aplicar_idioma();

	tab = TAB_LIBRARY;
	lib_sel = lib_top = set_sel = 0;
	mode = UI_MENU;
	active = NULL;
	frame_counter = 0;
	rebuild_catalog();

	/* Si se apaga Debug estando en Pruebas, la pestana desaparece bajo
	 * los pies. Se sale a la biblioteca en vez de quedarse en una
	 * pestana que ya no existe. */
	if (tab >= (uiTab)TAB_N) tab = TAB_LIBRARY;
}

/* Devuelve 1 si la direccion cuenta como "pulsada ahora", con
 * repeticion al mantenerla. */
static int nav(const gr33nPad *pad, u32 bit)
{
	if (pad->pressed & bit) {
		repeat_hold = bit;
		repeat_ticks = 0;
		return 1;
	}

	if ((pad->held & bit) && repeat_hold == bit) {
		repeat_ticks++;
		if (repeat_ticks >= REPEAT_DELAY &&
		    ((repeat_ticks - REPEAT_DELAY) % REPEAT_RATE) == 0)
			return 1;
		return 0;
	}

	if (repeat_hold == bit && !(pad->held & bit))
		repeat_hold = 0;

	return 0;
}

u32 uiBtnOk(void)   { return set_swap_ok ? GR33N_BTN_CIRCLE : GR33N_BTN_CROSS; }
u32 uiBtnBack(void) { return set_swap_ok ? GR33N_BTN_CROSS  : GR33N_BTN_CIRCLE; }

static const char *ok_glyph(void)   { return set_swap_ok ? "O" : "X"; }
static const char *back_glyph(void) { return set_swap_ok ? "X" : "O"; }

void uiUpdate(const gr33nPad *pad)
{
	frame_counter++;

	pad_lx = pad->lx; pad_ly = pad->ly;
	pad_rx = pad->rx; pad_ry = pad->ry;

	rebuild_catalog();

	/* JUEGO EN VIVO: antes que nada, y se sale de aqui.
	 *
	 * Va delante del resto a proposito. Debajo sigue estando la ficha
	 * del juego, y si se dejara pasar el mando la cerraria con Atras --
	 * que es lo que hacia la primera version: un boton suelto sacaba de
	 * la partida. */
	if (live_on) {
		if (combo_hecho(pad)) live_exit = 1;
		return;
	}

	/* LA LISTA DE REGIONES, AQUI Y NO EN LA PESTANA DE AJUSTES.
	 *
	 * Estaba dentro de la rama de Ajustes, que parecia el sitio logico y
	 * era el equivocado: quien guarda una region y no vuelve a abrir
	 * Ajustes nunca la aplica. El login termina cuando termina, y este es
	 * el unico punto por el que se pasa siempre. Cuesta una comparacion
	 * de enteros por fotograma. */
	regiones_al_dia();

	if (mode == UI_STREAM) {
		/* La pantalla de cuenta no es un juego: es un ajuste a pantalla
		 * completa, y de un ajuste se sale con Atras y ya. Pedir
		 * SELECT+Atras ahi era tratar como stream algo que no lo es. */
		if (active && active->kind == TITLE_AUTH) {
			if (pad->pressed & uiBtnBack()) {
				mode = UI_MENU;
				active = NULL;
			}
			return;
		}

		/* En un stream de verdad, la combinacion elegida en Ajustes:
		 * los botones sueltos son del juego. */
		if (combo_hecho(pad)) {
			mode = UI_MENU;
			active = NULL;
		}
		return;
	}

	if (pad->pressed & (GR33N_BTN_R1 | GR33N_BTN_L1)) {
		uiTab before = tab;

		if (pad->pressed & GR33N_BTN_R1) tab = (uiTab)((tab + 1) % TAB_N);
		else tab = (uiTab)((tab + TAB_N - 1) % TAB_N);

		/* Cambiar de pestana CIERRA la ficha, y aqui, que es donde se
		 * cambia de pestana.
		 *
		 * Estaba mas abajo, dentro de la rama de la biblioteca, y por eso
		 * solo funcionaba la mitad de las veces: desde Juegos, L1 lleva a
		 * Pruebas o a Ajustes, que no entran en esa rama. La ficha se
		 * quedaba abierta por debajo y volvias a ella con R1 sin haber
		 * pulsado nada. */
		if (in_ficha) {
			in_ficha = 0;
			desc_scroll = 0;
			ficha_msg = 0;
		}

		/* Salir de Ajustes es el momento natural de escribir a disco:
		 * el usuario ya ha terminado de toquetear. */
		if (before == TAB_SETTINGS && tab != TAB_SETTINGS) uiSaveSettings();
	}

	if (tab == TAB_DEBUG) {
		if (catalog_n > 0) {
			if (nav(pad, GR33N_BTN_UP)   && lib_sel > 0)             lib_sel--;
			if (nav(pad, GR33N_BTN_DOWN) && lib_sel < catalog_n - 1) lib_sel++;

			clamp_scroll(lib_sel, catalog_n, MAX_ROWS, &lib_top);

			if (pad->pressed & uiBtnOk()) {
				active = catalog[lib_sel];
				mode = UI_STREAM;
				stream_entered = frame_counter;
			}
		}
	} else if (tab == TAB_LIBRARY || tab == TAB_FAVS) {
		int n;

		if (tab != last_tab) { view_dirty = 1; grid_sel = 0; grid_top = 0; }
		rebuild_view();
		n = (int)view_n;

		if (in_ficha) {
			/* El stick derecho recorre la descripcion POR LINEAS.
			 *
			 * La primera version movia un indice de BYTES: el texto
			 * saltaba caracter a caracter, empezaba a mitad de palabra
			 * y podia partir una secuencia UTF-8. Un texto se recorre
			 * por lineas, que es la unidad que ve quien lee. */
			desc_hold = pad->ry > uiDeadzone() ? desc_hold + 1
			          : pad->ry < -uiDeadzone() ? desc_hold + 1 : 0;

			if (desc_hold == 1 || (desc_hold > 12 && (desc_hold % 3) == 0)) {
				if (pad->ry > 0) desc_scroll++;
				else             desc_scroll--;
			}

			if (desc_scroll > desc_max) desc_scroll = desc_max;
			if (desc_scroll < 0) desc_scroll = 0;

			if (pad->pressed & GR33N_BTN_TRIANGLE) toggle_fav();

			/* X SOBRE LA FICHA: pide una maquina de verdad.
			 *
			 * Todavia no reproduce nada -eso es WebRTC- pero abre la
			 * sesion contra el servidor de la region y cuenta al log lo
			 * que contesta. De ahi salen las dos respuestas que faltan:
			 * si el gateway exige SISU, y que pinta tiene la oferta SDP.
			 *
			 * Solo con derecho: pedir una maquina para un juego que no
			 * puedes jugar es gastar cuota para que te digan que no. */
			if (pad->pressed & uiBtnOk()) {
				const catTitle *t2 = catTitles(NULL);
				const catTitle *g2 = (t2 && view && grid_sel >= 0 &&
				                      grid_sel < (int)view_n)
				                   ? &t2[view[grid_sel]] : NULL;

				if (g2 && g2->entitled && g2->title_id[0])
					sesStart(g2->title_id);

				ficha_msg = 180;
			}
			if (ficha_msg) ficha_msg--;

			/* Y con Atras se suelta. Una maquina reservada que nadie
			 * mira sigue contando contra tu cuota. */
			if (pad->pressed & uiBtnBack()) sesStop();

			if (pad->pressed & uiBtnBack()) {
				in_ficha = 0;
				desc_scroll = 0;
				ficha_msg = 0;   /* que no lo herede la siguiente ficha */
			}
			return;
		}

		if (n > 0) {
			if (nav(pad, GR33N_BTN_LEFT)  && grid_sel > 0) grid_sel--;
			if (nav(pad, GR33N_BTN_RIGHT) && grid_sel < n - 1) grid_sel++;
			/* Restar en vez de sumar: (X + c) < X es justo el patron
			 * que hace saltar -Wstrict-overflow en ppu-gcc, y ya nos ha
			 * costado cuatro avisos en este proyecto. */
			if (nav(pad, GR33N_BTN_UP)   && grid_sel >= GRID_COLS)
				grid_sel -= GRID_COLS;
			if (nav(pad, GR33N_BTN_DOWN) && n - grid_sel > GRID_COLS)
				grid_sel += GRID_COLS;

			{
				int row = grid_sel / GRID_COLS;
				if (row < grid_top) grid_top = row;
				if (row - grid_top >= GRID_ROWS) grid_top = row - GRID_ROWS + 1;
				if (grid_top < 0) grid_top = 0;
			}

			if (pad->pressed & GR33N_BTN_TRIANGLE) toggle_fav();

			if (pad->pressed & uiBtnOk()) {
				in_ficha = 1;
				desc_scroll = 0;
				ficha_msg = 0;
			}
		}

		/* EL FILTRO VA FUERA del "si hay juegos".
		 *
		 * Estaba dentro, y bastaba elegir un genero sin resultados para
		 * que la lista quedara vacia... y con la lista vacia ya no se
		 * atendia el cuadrado. Te quedabas encerrado en el filtro que
		 * acababas de elegir. */
		if (pad->pressed & GR33N_BTN_SQUARE) {
			rebuild_genres();
			genre_sel = genres_n ? (genre_sel + 1) % genres_n : 0;
			view_dirty = 1;
			grid_sel = 0;
			grid_top = 0;
		}

		last_tab = tab;
	} else {
		const uiSetting *st;
		int *v, before;

		rebuild_settings();
		if (set_sel >= settings_vis_n) set_sel = settings_vis_n - 1;
		if (set_sel < 0) set_sel = 0;

		if (nav(pad, GR33N_BTN_UP)   && set_sel > 0) set_sel--;
		if (nav(pad, GR33N_BTN_DOWN) && set_sel < settings_vis_n - 1) set_sel++;

		clamp_scroll(set_sel, settings_vis_n, SET_ROWS, &set_top);

		st = &settings[settings_vis[set_sel]];
		v  = st->value;
		before = *v;

		switch (st->kind) {
		case SET_ACTION:
			/* Cada accion se reconoce por la variable a la que apunta.
			 * Antes aqui habia un solo destino cableado, asi que
			 * cualquier fila de accion nueva abria la pantalla de la
			 * cuenta -- una fila que no hace lo que dice su nombre. */
			if (pad->pressed & uiBtnOk()) {
				if (v == &set_ping) {
					pingMedir();
				} else {
					active = &title_auth;
					mode = UI_STREAM;
					stream_entered = frame_counter;
				}
			}
			break;

		case SET_TOGGLE:
			if ((pad->pressed & uiBtnOk()) ||
			    nav(pad, GR33N_BTN_LEFT) || nav(pad, GR33N_BTN_RIGHT))
				*v = !*v;
			break;

		case SET_RANGE:
			if (nav(pad, GR33N_BTN_LEFT))  *v -= st->step;
			if (nav(pad, GR33N_BTN_RIGHT)) *v += st->step;
			if (*v < st->min) *v = st->min;
			if (*v > st->max) *v = st->max;
			break;

		case SET_CHOICE:
			/* Da la vuelta por los dos lados: con ocho regiones, llegar a
			 * la ultima desde la primera son siete pulsaciones o una. */
			if (nav(pad, GR33N_BTN_LEFT))
				*v = (*v + opts_n(st) - 1) % opts_n(st);
			if (nav(pad, GR33N_BTN_RIGHT))
				*v = (*v + 1) % opts_n(st);
			break;
		}

		if (*v != before) {
			set_dirty = 1;

			/* Si el que ha cambiado es el filtro, la rejilla ya no vale
			 * lo que tenia calculado. */
			if (v == &set_show_all) view_dirty = 1;

			/* Y si es Debug, aparecen o desaparecen filas debajo. Se
			 * rehace aqui mismo para que el dibujado de este mismo
			 * fotograma vea la lista buena. */
			if (v == &set_debug) rebuild_settings();

			/* Y si es la region, se aplica DE VERDAD. Un selector que
			 * no cambia a donde te conectas es un adorno, y este
			 * proyecto ya tuvo uno cinco meses. Se guarda el nombre,
			 * que es lo que sobrevive a que Microsoft reordene la
			 * lista. */
			/* Los dos idiomas se aplican al momento: cambiar "Idioma"
			 * y que el menu siga en el otro hasta reiniciar seria un
			 * ajuste que no parece funcionar. */
			if (v == &set_idioma || v == &set_juego_loc) {
				aplicar_idioma();
				snprintf(set_juego_cod, sizeof(set_juego_cod), "%s",
				         xclocCodigo(set_juego_loc));
			}

			if (v == &set_server) {
				aplicar_region();

				if (set_server > 0) {
					xcRegion r;
					if (authRegionCopy(set_server - 1, &r) == 0)
						snprintf(set_server_nom, sizeof(set_server_nom),
						         "%s", r.name);
				} else {
					set_server_nom[0] = '\0';
				}
			}
		}
	}
}

/* Abre la pantalla de cuenta sin pasar por el menu. La usa main.c en el
 * primer arranque. */
void uiOpenAuth(void)
{
	tab    = TAB_SETTINGS;
	set_sel = 0;
	active = &title_auth;
	mode   = UI_STREAM;
	stream_entered = frame_counter;
}

int uiFirstRun(void) { return first_run; }

/* Marca como favoritos los que estaban guardados. Se llama una vez, en
 * cuanto el catalogo tiene titulos: antes no hay a que aplicarlo. */
static void load_favs(void)
{
	static char buf[8192];
	const catTitle *t;
	u32 n = 0, len = 0, i;
	char *line, *save;

	t = catTitles(&n);
	if (!t || n == 0) return;

	if (storeLoad(FAVS_FILE, buf, sizeof(buf) - 1, &len) != 0 || !len) return;
	buf[len] = '\0';

	for (line = buf; *line; line = save) {
		save = strchr(line, '\n');
		if (save) *save++ = '\0'; else save = line + strlen(line);

		if (!line[0]) continue;

		for (i = 0; i < n; i++) {
			if (strcmp(t[i].title_id, line) != 0) continue;
			((catTitle*)t)[i].favorite = 1;
			break;
		}
	}

	view_dirty = 1;
}

void uiSetProfilePic(const u32 *argb, u32 w, u32 h)
{
	prof_pic   = argb;
	prof_pic_w = w;
	prof_pic_h = h;
}

void uiSetProfile(const char *gamertag, u32 gamerscore)
{
	if (!gamertag || !gamertag[0]) { prof_have = 0; return; }

	snprintf(prof_tag, sizeof(prof_tag), "%s", gamertag);
	prof_score = gamerscore;
	prof_have = 1;
}

uiMode uiGetMode(void)             { return mode; }
const uiTitle *uiActiveTitle(void) { return active; }
int uiDebug(void)                  { return set_debug; }
int uiDeadzone(void)               { return set_deadzone; }
int uiHudScale(void)               { return set_hud_scale; }
int uiHudPos(void)                 { return set_hud_pos; }
const char *uiServerName(void)
{
	if (set_server < 0 || set_server >= server_opt_n) return "Automatico";
	return server_opt[set_server] ? server_opt[set_server] : "Automatico";
}

/* uiSetServerLatency ya no esta.
 *
 * Era un hueco donde otro modulo dejaba los milisegundos y la interfaz
 * los pintaba: dos copias del mismo numero, una de ellas sin nadie que la
 * escribiera. Ahora la interfaz le pregunta a ping.c cuando va a pintar,
 * que es la unica manera de que no se puedan desincronizar. */

/* --------------------------------------------------------------------- */
/* Persistencia                                                          */
/*                                                                       */
/* Texto de clave=valor, una por linea. Podria ser un volcado binario de  */
/* la estructura y ocuparia menos, pero entonces cambiar un ajuste de     */
/* sitio invalidaria el fichero de todo el mundo, y para depurar habria   */
/* que abrir un editor hexadecimal. Aqui se abre con el bloc de notas.    */
/* --------------------------------------------------------------------- */

#define SETTINGS_FILE  "config.cfg"

/* Clave -> variable. Un ajuste que no este aqui simplemente no se guarda,
 * y uno que sobre en el fichero se ignora: asi un fichero viejo nunca
 * impide arrancar. */
static const struct { const char *key; int *value; } persisted[] = {
	{ "mostrar_todo", &set_show_all },
	{ "debug",    &set_debug    },
	{ "deadzone", &set_deadzone },
	{ "swap_ok",  &set_swap_ok  },
	{ "hud_scale", &set_hud_scale },
	{ "hud_pos",   &set_hud_pos   },
	{ "salir_combo", &set_exit_combo },
	{ "salir_app",   &set_quit_app   }
};
#define PERSISTED_N ((int)(sizeof(persisted)/sizeof(persisted[0])))

/* LA REGION SE GUARDA APARTE Y COMO TEXTO.
 *
 * La clave vieja era "server" con un indice dentro de una lista
 * inventada. Ese numero ya no significa nada: un fichero de antes trae un
 * 3 que apuntaba a "Este de EEUU" en una lista que ya no existe. No se
 * migra, se ignora -- adivinar a que region equivale seria inventarse la
 * eleccion del usuario -- y al ignorarlo se queda en Automatico, que es
 * lo que habia antes de que este ajuste hiciera nada.
 *
 * La clave nueva es "region_nom" y lleva el nombre tal cual lo dice
 * Microsoft, no el traducido: lo que se compara al arrancar es contra la
 * respuesta del servidor. */
#define KEY_REGION "region_nom"

/* Y los dos idiomas, por codigo tambien y por lo mismo. */
#define KEY_IDIOMA     "idioma"
#define KEY_JUEGO_LOC  "idioma_juego"

/* Se recorta cada valor a su rango al cargarlo. Un fichero editado a mano
 * o de una version anterior no puede dejar la interfaz inservible. */
static void clamp_settings(void)
{
	int i;

	for (i = 0; i < SETTINGS_N; i++) {
		const uiSetting *st = &settings[i];
		int *v = st->value;

		switch (st->kind) {
		case SET_ACTION:
			break;   /* no guarda valor: nada que recortar */

		case SET_TOGGLE:
			*v = *v ? 1 : 0;
			break;
		case SET_RANGE:
			if (*v < st->min) *v = st->min;
			if (*v > st->max) *v = st->max;
			break;
		case SET_CHOICE:
			if (*v < 0 || *v >= opts_n(st)) *v = 0;
			break;
		}
	}
}

void uiSetLive(int on)
{
	if (!on && live_on) live_exit = 0;   /* al salir, sin arrastres */
	live_on = on ? 1 : 0;
}

int uiLiveWantsExit(void)
{
	int r = live_exit;
	live_exit = 0;
	return r;
}

u32 uiQuitMask(void)
{
	return set_quit_app ? (u32)(GR33N_BTN_START | GR33N_BTN_SELECT) : 0u;
}

void uiLoadSettings(void)
{
	char buf[512];
	u32 len = 0;
	char *line, *save;

	if (storeLoad(SETTINGS_FILE, buf, sizeof(buf) - 1, &len) != 0) {
		linkLog("[ui] sin fichero de ajustes: primer arranque");

		/* Y SE ESCRIBE, con los valores de fabrica dentro.
		 *
		 * Antes no se guardaba nada hasta que alguien cambiaba un ajuste,
		 * asi que en una instalacion recien hecha config.cfg no existia y
		 * no habia forma de ver --ni de editar a mano-- con que estaba
		 * arrancando. Ahora el fichero esta desde el primer cierre y dice
		 * exactamente lo que hay. */
		set_dirty = 1;

		aplicar_idioma();
		return;
	}

	first_run = 0;
	buf[len] = '\0';

	for (line = strtok_r(buf, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		char *eq = strchr(line, '=');
		int i;

		if (!eq) continue;
		*eq = '\0';

		if (strcmp(line, KEY_REGION) == 0) {
			snprintf(set_server_nom, sizeof(set_server_nom), "%s",
			         eq + 1);
			continue;
		}

		if (strcmp(line, KEY_IDIOMA) == 0) {
			set_idioma = (int)i18nPorCodigo(eq + 1);
			continue;
		}

		if (strcmp(line, KEY_JUEGO_LOC) == 0) {
			int i = xclocPorCodigo(eq + 1);

			/* Un codigo que ya no esta en la lista NO deja el ajuste
			 * apuntando a cualquier sitio: se queda el de por defecto y
			 * se dice. */
			if (i >= 0) {
				set_juego_loc = i;
				snprintf(set_juego_cod, sizeof(set_juego_cod), "%s",
				         eq + 1);
			} else {
				linkLog("[ui] idioma de juego guardado (%s) desconocido",
				        eq + 1);
			}
			continue;
		}

		for (i = 0; i < PERSISTED_N; i++) {
			if (strcmp(line, persisted[i].key) == 0) {
				*persisted[i].value = (int)strtol(eq + 1, NULL, 10);
				break;
			}
		}
	}

	clamp_settings();

	/* ANTES de rebuild_catalog: la rejilla lleva texto y el texto ya tiene
	 * que estar en el idioma bueno cuando se arme. */
	aplicar_idioma();

	set_dirty = 0;
	rebuild_catalog();

	/* La region NO se resuelve aqui: todavia no hay lista con que. Lo que
	 * se ha cargado es un nombre, y se convierte en indice en cuanto el
	 * login traiga las regiones (regiones_al_dia). */
	linkLog("[ui] ajustes cargados: debug=%d zona=%d swap=%d region=%s",
	        set_debug, set_deadzone, set_swap_ok,
	        set_server_nom[0] ? set_server_nom : "automatica");

	/* LOS DOS ATAJOS PUEDEN PISARSE, y cuando lo hacen el sintoma no se
	 * parece a la causa: sales de una partida y se te cierra GR33N
	 * entero. Quien lo sufra va a contarlo como "se cierra solo".
	 *
	 * Es una eleccion legitima del usuario y no se le cambia; se anota,
	 * para que el log lo diga en vez de que haya que adivinarlo. Los
	 * valores de fabrica no chocan. */
	if (set_quit_app && combo_btn[set_exit_combo].mantener == GR33N_BTN_SELECT
	                 && combo_btn[set_exit_combo].pulsar == GR33N_BTN_START)
		linkLog("[ui] !! salir del juego y cerrar GR33N son la misma "
		        "combinacion: salir de una partida cerrara la aplicacion");
}

void uiSaveSettings(void)
{
	char buf[512];
	int n = 0, i;

	if (!set_dirty) return;

	for (i = 0; i < PERSISTED_N; i++) {
		int w = snprintf(buf + n, sizeof(buf) - (size_t)n, "%s=%d\n",
		                 persisted[i].key, *persisted[i].value);
		if (w <= 0 || n + w >= (int)sizeof(buf)) break;
		n += w;
	}

	if (set_server_nom[0]) {
		int w = snprintf(buf + n, sizeof(buf) - (size_t)n, "%s=%s\n",
		                 KEY_REGION, set_server_nom);
		if (w > 0 && n + w < (int)sizeof(buf)) n += w;
	}

	/* Los dos idiomas, por codigo y no por indice. Ver KEY_REGION. */
	{
		int w = snprintf(buf + n, sizeof(buf) - (size_t)n,
		                 "%s=%s\n%s=%s\n",
		                 KEY_IDIOMA, i18nCodigo((gr33nIdioma)set_idioma),
		                 KEY_JUEGO_LOC, xclocCodigo(set_juego_loc));
		if (w > 0 && n + w < (int)sizeof(buf)) n += w;
	}

	if (storeSave(SETTINGS_FILE, buf, (u32)n) == 0) set_dirty = 0;
	else linkLog("!! [ui] no se pudieron guardar los ajustes");
}

int uiSettingsDirty(void) { return set_dirty; }

/* --------------------------------------------------------------------- */
/* Dibujo                                                                */
/* --------------------------------------------------------------------- */

static void draw_check(gr33nSurface *s, int x, int y, int size, u32 col)
{
	int i;
	int t = size / 6;
	if (t < 3) t = 3;

	for (i = 0; i < size / 3; i++)
		gfxFillRect(s, x + i, y + size / 2 + i, t, t, col);
	for (i = 0; i < size * 2 / 3; i++)
		gfxFillRect(s, x + size / 3 + i, y + size / 2 + size / 3 - i, t, t, col);
}

static void draw_topbar(gr33nSurface *s)
{
	const char *tab_name[TAB_COUNT] = {
		TR("Juegos",    "Games"),
		TR("Favoritos", "Favourites"),
		TR("Ajustes",   "Settings"),
		TR("Pruebas",   "Tests")
	};
	int i, x = 300, n = TAB_N, sum = 0, gap, prof_left;

	gfxVGradient(s, 0, 0, W, TOPBAR_H, COL_BAR, COL_BG);
	gfxFillRect(s, 0, TOPBAR_H - 2, W, 2, COL_LINE);

	textDraw(s, 32, 18, 4, COL_GREEN, "GR33N");
	textDraw(s, 32 + textWidth(4, "GR33N") + 12, 30, 2, COL_TEXT_DIM, GR33N_VERSION);

	/* EL REPARTO SE CALCULA, no se fija.
	 *
	 * Con separacion fija de 48 y cuatro pestanas, "Pruebas" acababa
	 * debajo del avatar y del gamertag. Y el gamertag mide lo que mida:
	 * no es un numero que yo pueda elegir. Asi que primero se mide lo
	 * que ocupa el perfil, y las pestanas se reparten el hueco que
	 * queda. */
	prof_left = prof_have
	          ? W - 32 - 44 - 16 - textWidth(3, prof_tag) - 24
	          : W - 32 - textWidth(2, "Cargando perfil...") - 24;

	for (i = 0; i < n; i++) sum += textWidth(3, tab_name[i]);

	gap = n > 1 ? (prof_left - x - sum) / (n - 1) : 0;
	if (gap > 48) gap = 48;
	if (gap < 14) gap = 14;

	for (i = 0; i < n; i++) {
		int tw = textWidth(3, tab_name[i]);
		int sel = (i == (int)tab);

		textDraw(s, x, 22, 3, sel ? COL_GREEN : COL_TEXT_DIM, tab_name[i]);
		if (sel) gfxFillRect(s, x, 48, tw, 3, COL_GREEN);

		x += tw + gap;
	}

	/* Derecha de la barra: el PERFIL, y nada mas.
	 *
	 * Aqui salia antes el servidor de depuracion. Eso es informacion
	 * nuestra, no del usuario, y ocupaba el sitio de lo unico que le
	 * importa a quien usa esto. Se ha mudado al panel de estadisticas,
	 * que es donde viven las cosas de depurar. */
	{
		int rx = W - 32;

		if (prof_have) {
			char sc[24];
			int tw;

			/* Avatar pegado al borde, y el texto a su izquierda en dos
			 * lineas. Antes iban los puntos AL LADO del gamertag y eso
			 * son cien pixeles mas de ancho que se comian una pestana. */
			if (prof_pic)
				gfxBlit(s, rx - 44, 10, 44, 44, prof_pic, prof_pic_w, prof_pic_h);
			else
				gfxRect(s, rx - 44, 10, 44, 44, 2, COL_LINE);

			rx -= 44 + 16;

			snprintf(sc, sizeof(sc), "%u G", (unsigned)prof_score);
			tw = textWidth(3, prof_tag);

			textDraw(s, rx - tw, 12, 3, COL_TEXT, prof_tag);
			textDraw(s, rx - textWidth(2, sc), 38, 2, COL_GREEN_DIM, sc);
		} else {
			const char *msg = authHaveToken() ? "Cargando perfil..."
			                                  : "Sin sesion";

			textDraw(s, rx - textWidth(2, msg), 28, 2, COL_TEXT_DIM, msg);
		}
	}
}

static void draw_botbar(gr33nSurface *s, const char *hints)
{
	gfxFillRect(s, 0, H - BOTBAR_H, W, BOTBAR_H, COL_BAR);
	gfxFillRect(s, 0, H - BOTBAR_H, W, 2, COL_LINE);
	textDraw(s, 32, H - BOTBAR_H + 17, 2, COL_TEXT_DIM, hints);
}

/* Portada de mentira: degradado del color del titulo con sus iniciales.
 * Cuando haya caratulas de verdad, esto se cambia por una textura. */
static void draw_cover(gr33nSurface *s, int x, int y, int size,
                       const uiTitle *t)
{
	char ini[3];
	int n = 0;
	const char *p = t->name;
	u32 dark = GR33N_RGB(((t->accent >> 16) & 0xff) / 5,
	                     ((t->accent >> 8) & 0xff) / 5,
	                     (t->accent & 0xff) / 5);

	gfxVGradient(s, x, y, size, size, t->accent, dark);
	gfxRect(s, x, y, size, size, 2, COL_LINE);

	while (*p && n < 2) {
		if (*p != '_' && *p != ' ') ini[n++] = *p;
		while (*p && *p != '_' && *p != ' ') p++;
		while (*p == '_' || *p == ' ') p++;
	}
	ini[n] = '\0';

	textDraw(s, x + (size - textWidth(6, ini)) / 2,
	         y + (size - TEXT_GLYPH_H * 6) / 2, 6, COL_BLACK, ini);
}

static void save_favs(void)
{
	static char buf[8192];
	const catTitle *t;
	u32 n = 0, i, used = 0;

	t = catTitles(&n);
	if (!t) return;

	for (i = 0; i < n; i++) {
		int k;
		if (!t[i].favorite) continue;
		k = snprintf(buf + used, sizeof(buf) - used, "%s\n", t[i].title_id);
		if (k < 0 || (u32)k >= sizeof(buf) - used) break;
		used += (u32)k;
	}

	storeSave(FAVS_FILE, buf, used);
}

static void toggle_fav(void)
{
	const catTitle *t = catTitles(NULL);
	catTitle *g;

	if (!t || !view || grid_sel < 0 || grid_sel >= (int)view_n) return;

	/* catTitles devuelve const porque nadie de fuera deberia tocarlo. El
	 * favorito es la unica excepcion y se marca aqui a proposito, para
	 * que se vea que es deliberado y no un descuido. */
	g = (catTitle*)&t[view[grid_sel]];
	g->favorite = g->favorite ? 0 : 1;

	save_favs();
	if (tab == TAB_FAVS) view_dirty = 1;
}

/* Los generos que hay AHORA mismo en el catalogo. Crece segun el barrido
 * de fondo va completando los datos. */
static void rebuild_genres(void)
{
	const catTitle *t;
	u32 n = 0, i;
	int j;

	t = catTitles(&n);
	if (!t) return;

	genres_n = 1;
	snprintf(genres[0], sizeof(genres[0]), "%s", TR("Todos", "All"));

	for (i = 0; i < n && genres_n < GENRES_MAX; i++) {
		if (!t[i].genre[0]) continue;

		for (j = 1; j < genres_n; j++)
			if (strcmp(genres[j], t[i].genre) == 0) break;

		if (j == genres_n)
			snprintf(genres[genres_n++], sizeof(genres[0]), "%s",
			         t[i].genre);
	}
}

/* Reconstruye la lista de indices que se ven, segun el filtro. */
static void rebuild_view(void)
{
	const catTitle *t;
	u32 n = 0, i;
	u32 anchor = 0;
	int had_anchor = 0;

	/* LA VISTA CADUCA SOLA cuando llega catalogo nuevo.
	 *
	 * Antes solo se reconstruia cuando la interfaz lo pedia, y la primera
	 * vuelta se hacia con la tabla vacia: view_n quedaba en 0, view_dirty
	 * en 0, y la rejilla se quedaba diciendo "ningun juego disponible con
	 * tu suscripcion" para siempre. Cambiar de pestana y volver lo
	 * arreglaba, que es la senal clasica de un estado que nadie invalida.
	 *
	 * generation sube cada vez que la tabla se rehace, asi que sirve para
	 * las dos cosas: reconstruir la vista y volver a marcar favoritos. */
	{
		const catInfo *ci = catStatus();

		if (ci->generation != view_gen) {
			/* Tabla nueva: los generos que habia se han ido con ella, y
			 * genre_sel apuntaria a un nombre de la lista vieja. La
			 * biblioteca se quedaria vacia filtrando contra algo que ya no
			 * existe, y la barra de abajo anunciandolo tan tranquila. */
			view_gen   = ci->generation;
			genre_sel  = 0;
			genres_n   = 1;
			view_dirty = 1;
		}

		if (ci->details_gen != view_det) {
			view_det   = ci->details_gen;
			view_dirty = 1;
		}
	}

	if (!view_dirty) return;

	/* QUE JUEGO habia bajo el cursor, antes de tocar nada.
	 *
	 * grid_sel es una posicion en la lista, y la lista se mueve: con un
	 * filtro por genero puesto, cada lote de datos que llega mete titulos
	 * nuevos en su sitio de la tabla y corre todo lo que va detras. Sin
	 * esto, cada 2,2 segundos el cursor apunta a otro juego sin que nadie
	 * lo haya movido - y si en ese momento pulsas X, abres otro. Con la
	 * ficha abierta era peor: te cambiaba el juego en las narices.
	 *
	 * El indice de TABLA si es estable dentro de una generacion, asi que
	 * sirve de ancla. */
	if (view && view_n && grid_sel >= 0 && grid_sel < (int)view_n) {
		anchor = view[grid_sel];
		had_anchor = 1;
	}

	t = catTitles(&n);

	/* La tabla se sobrescribe entera al refrescar y el bit de favorito
	 * vive dentro, asi que hay que volver a aplicarlos. */
	{
		u32 gen = catStatus()->generation;

		if (n && (!favs_loaded || gen != favs_gen)) {
			favs_loaded = 1;
			favs_gen = gen;
			load_favs();
		}
	}

	if (!view) {
		view = (u32*)malloc((size_t)CAT_MAX_TITLES * sizeof(u32));
		if (!view) return;
	}

	view_n = 0;
	for (i = 0; i < n && view_n < CAT_MAX_TITLES; i++) {
		if (tab == TAB_FAVS && !t[i].favorite) continue;
		if (tab != TAB_FAVS && !show_all && !t[i].entitled) continue;
		if (genre_sel > 0 && strcmp(t[i].genre, genres[genre_sel]) != 0)
			continue;
		view[view_n++] = i;
	}

	view_dirty = 0;

	/* El cursor vuelve al MISMO juego, este donde este ahora. */
	if (had_anchor) {
		u32 k;
		int found = -1;

		for (k = 0; k < view_n; k++)
			if (view[k] == anchor) { found = (int)k; break; }

		if (found >= 0) {
			grid_sel = found;
		} else if (in_ficha) {
			/* Ha desaparecido de la lista (otro filtro, otra tabla). Se
			 * cierra la ficha en vez de dejarla enseñando otro juego con
			 * pinta de ser el que abriste. */
			in_ficha = 0;
			desc_scroll = 0;
			ficha_msg = 0;
		}
	}

	if (grid_sel >= (int)view_n) grid_sel = view_n ? (int)view_n - 1 : 0;
	if (grid_sel < 0) grid_sel = 0;

	/* Y la fila de arriba, que si no se queda apuntando fuera y la rejilla
	 * sale en blanco sin que nadie diga por que. */
	{
		int row  = grid_sel / GRID_COLS;
		int rows = ((int)view_n + GRID_COLS - 1) / GRID_COLS;

		if (grid_top > rows - GRID_ROWS)   grid_top = rows - GRID_ROWS;
		if (row < grid_top)                grid_top = row;
		if (row - grid_top >= GRID_ROWS)   grid_top = row - GRID_ROWS + 1;
		if (grid_top < 0)                  grid_top = 0;
	}
}

/* El hueco mientras la caratula viaja.
 *
 * No es un rectangulo vacio a proposito: un degradado con las iniciales se
 * distingue del de al lado, asi que la rejilla ya es navegable antes de
 * que baje una sola imagen. Y como cada caratula cuesta unos 600 ms, esa
 * diferencia es la que separa "esta cargando" de "esta colgado". */
static void draw_placeholder(gr33nSurface *s, int x, int y, int size,
                             const char *name, u32 accent)
{
	u32 dark = GR33N_RGB(((accent >> 16) & 0xff) / 5,
	                     ((accent >> 8) & 0xff) / 5,
	                     (accent & 0xff) / 5);
	char ini[3];
	int n = 0;
	const char *p = name;

	gfxVGradient(s, x, y, size, size, accent, dark);

	while (*p && n < 2) {
		if ((unsigned char)*p > ' ') ini[n++] = *p;
		while (*p && *p != ' ' && *p != ':' && *p != '-') p++;
		while (*p == ' ' || *p == ':' || *p == '-') p++;
	}
	ini[n] = '\0';

	if (n)
		textDraw(s, x + (size - textWidth(5, ini)) / 2,
		         y + (size - TEXT_GLYPH_H * 5) / 2, 5, COL_BLACK, ini);
}

/* Un color estable por juego, para que el hueco no baile entre arranques
 * ni entre celdas. Sale del propio identificador. */
static u32 tint_of(const char *id)
{
	u32 h = 2166136261u;

	while (*id) { h ^= (unsigned char)*id++; h *= 16777619u; }

	/* Verdes y azules apagados: la rejilla tiene que dejar leer el
	 * nombre, no competir con el. */
	return GR33N_RGB(40 + (h & 63), 70 + ((h >> 8) & 90), 60 + ((h >> 16) & 80));
}

/* Cuantos caracteres caben en una celda a escala 2. */
#define NAME_SCALE  2
#define NAME_FITS   (GRID_CELL / (TEXT_ADVANCE * NAME_SCALE))

/* UN solo reloj de desplazamiento, porque solo hay una cosa desplazandose
 * a la vez: o el nombre del juego seleccionado en la rejilla, o el titulo
 * de la ficha abierta. Se pone a cero al cambiar de una a otra para que el
 * texto empiece siempre por el principio: llegar a una celda y ver el
 * nombre por la mitad es peor que no desplazarlo.
 *
 * El tic va en el DIBUJO y no en uiUpdate a proposito. uiUpdate corre
 * antes, asi que al abrir la ficha veria todavia el estado viejo y el
 * primer fotograma del titulo saldria desplazado. */
static u32 marq_clock = 0;
static int marq_key = -1;

static void marq_tick(int key)
{
	if (marq_key != key) { marq_key = key; marq_clock = 0; }
	else marq_clock++;
}

/* La ventana de texto que toca enseñar en este fotograma.
 *
 * De caracter en caracter y no de pixel en pixel: asi no hay que recortar
 * glifos a medias contra el borde, que obligaria a tocar el dibujado de
 * texto entero. A un paso cada ocho fotogramas se lee sin marear.
 *
 * El ciclo es ida y vuelta con pausa en los dos extremos: volver de golpe
 * al principio da un tiron que llama mas la atencion que el propio texto. */
static void marq_window(char *out, u32 outsize, const char *nm, int fits)
{
	int len = textLen(nm);
	int over, from;
	u32 hold = 45;                       /* 0,75 s parado */
	u32 step = 8;                        /* fotogramas por caracter */
	u32 run, total, t;

	if (fits < 1) fits = 1;

	if (len <= fits) {
		snprintf(out, outsize, "%s", nm);
		return;
	}

	over  = len - fits;
	run   = (u32)over * step;
	total = hold + run + hold + run;
	t     = marq_clock % total;

	if (t < hold)                   from = 0;
	else if (t < hold + run)        from = (int)((t - hold) / step);
	else if (t < hold + run + hold) from = over;
	else                            from = over - (int)((t - hold - run - hold) / step);

	if (from < 0)    from = 0;
	if (from > over) from = over;

	textSlice(out, outsize, nm, from, fits);
}

static void draw_name(gr33nSurface *s, int x, int y, const char *nm, int sel)
{
	char win[128];

	/* Lo que cabe entero no se mueve. Un nombre corto bailando seria
	 * ruido por ruido. */
	if (textLen(nm) <= NAME_FITS) {
		textDraw(s, x, y, NAME_SCALE, sel ? COL_GREEN : COL_TEXT_DIM, nm);
		return;
	}

	if (!sel) {
		/* Los no seleccionados se cortan con puntos suspensivos, para
		 * que se vea que hay mas y no parezca que el juego se llama
		 * asi. */
		textSlice(win, sizeof(win), nm, 0, NAME_FITS - 2);
		{
			u32 n = (u32)strlen(win);
			if (n + 3 <= sizeof(win)) { win[n] = '.'; win[n+1] = '.';
			                            win[n+2] = '\0'; }
		}
		textDraw(s, x, y, NAME_SCALE, COL_TEXT_DIM, win);
		return;
	}

	marq_window(win, sizeof(win), nm, NAME_FITS);
	textDraw(s, x, y, NAME_SCALE, COL_GREEN, win);
}

static void draw_grid(gr33nSurface *s)
{
	const catTitle *t = catTitles(NULL);
	const catInfo  *ci = catStatus();
	int col, row, first;

	rebuild_view();

	if (!t || !view || view_n == 0) {
		const char *msg;

		/* Cada vacio tiene su motivo y se dice cual. "Cargando
		 * biblioteca" en Favoritos, cuando el catalogo lleva rato
		 * cargado, es sencillamente falso. */
		if (tab == TAB_FAVS && ci->state == CAT_OK)
			msg = "Todavia no has marcado ningun favorito. Triangulo "
			      "sobre un juego.";
		else if (genre_sel > 0 && ci->state == CAT_OK)
			msg = "Ningun juego con ese filtro. Cuadrado para cambiarlo.";
		else if (ci->state == CAT_FAILED)
			msg = ci->err;
		else if (ci->state == CAT_OK)
			msg = TR("Ningun juego disponible con tu suscripcion.",
			         "No games available with your subscription.");
		else if (authHaveToken())
			msg = "Cargando biblioteca...";
		else
			msg = "Inicia sesion en Ajustes para ver tus juegos.";

		/* Sin nada que enseñar no hay nada cerca. Si no se dice, la cache
		 * sigue protegiendo la pagina de la ultima vez. */
		catFocus(NULL, 0, 0);

		textWrap(s, GRID_X, CONTENT_Y + 40, 3, COL_TEXT_DIM,
		         W - GRID_X * 2, 34, 3, msg);
		return;
	}

	marq_tick(grid_sel);

	first = grid_top * GRID_COLS;

	/* Se le dice al catalogo QUE titulos hay en pantalla, por indice de
	 * tabla, y de paso una pagina de margen por arriba y otra por abajo.
	 *
	 * La lista va en este orden -primero lo visible, luego el margen-
	 * porque catNeed pide los dieciseis primeros que le falten: asi el
	 * lote se gasta en lo que se esta viendo y no en la prefetch.
	 *
	 * Antes esto era catNeed(first, GRID_PAGE), o sea un RANGO, y con el
	 * filtro por defecto (586 de 2531) el rango de la tabla no tiene nada
	 * que ver con lo que se ve: se pedian los datos de veintiun juegos
	 * distintos de los de la pantalla. De ahi que los nombres fueran
	 * apareciendo a trozos. */
	{
		static u32 near[CAT_FOCUS_MAX];
		u32 near_n = 0, vis_n = 0;
		int k;
		int vis_end = first + GRID_PAGE;      /* fin de lo visible */
		int from    = first - GRID_PAGE;      /* pagina de arriba  */
		int to      = vis_end + GRID_PAGE;    /* pagina de abajo   */

		if (from < 0) from = 0;
		if (vis_end > (int)view_n) vis_end = (int)view_n;
		if (to > (int)view_n) to = (int)view_n;

		/* Lo visible primero. */
		for (k = first; k < vis_end; k++)
			if (near_n < CAT_FOCUS_MAX) near[near_n++] = view[k];

		vis_n = near_n;

		/* Y despues el margen, saltandose lo que ya esta. */
		for (k = from; k < to; k++) {
			if (k >= first && k < vis_end) continue;
			if (near_n < CAT_FOCUS_MAX) near[near_n++] = view[k];
		}

		/* La cache protege las 63 y sabe cuales de ellas son las 21 que se
		 * ven; los datos de tienda se piden solo de esas 21.
		 *
		 * No es lo mismo: mientras haya un lote de datos pendiente el hilo
		 * no baja ni una caratula, porque los datos van primero. Pedirlos
		 * de las 63 son cuatro vueltas de 2,2 s con la rejilla en gris
		 * antes de que aparezca la primera imagen. De las 21 es una, y del
		 * resto ya se encarga el barrido de fondo. */
		catFocus(near, near_n, vis_n);
		catNeed(near, vis_n);

		/* Y SE PIDEN LAS DEL MARGEN, unas pocas por fotograma.
		 *
		 * catArt reserva la ranura cuando le preguntas por algo que no
		 * tiene, asi que preguntar por lo de la pagina de arriba y la de
		 * abajo es todo el adelanto que hace falta. La cache ya sabe que
		 * esas van despues de las visibles.
		 *
		 * Esto es lo que arregla "bajas, subes, y estan sin caratula": no
		 * es que se hubieran desalojado -de eso se encarga catFocus- es que
		 * nadie las habia pedido nunca. Cuatro por fotograma son 240 al
		 * segundo, mucho mas de lo que la red puede servir; el limite esta
		 * para no recorrer los 63 sesenta veces por segundo. */
		{
			static u32 pre = 0;
			u32 tries;

			for (tries = 0; tries < 4 && near_n > vis_n; tries++) {
				if (pre < vis_n || pre >= near_n) pre = vis_n;
				catArt(near[pre]);
				pre++;
			}
		}
	}

	for (row = 0; row < GRID_ROWS; row++) {
		for (col = 0; col < GRID_COLS; col++) {
			int slot = first + row * GRID_COLS + col;
			int x = GRID_X + col * GRID_STEP_X;
			int y = CONTENT_Y + row * GRID_STEP_Y;
			const catTitle *g;
			const u32 *px;
			u32 idx;

			if (slot >= (int)view_n) return;

			idx = view[slot];
			g   = &t[idx];

			if (slot == grid_sel) {
				gfxFillRect(s, x - GRID_SEL_PAD, y - GRID_SEL_PAD,
				            GRID_CELL + GRID_SEL_PAD * 2, GRID_SEL_H,
				            COL_PANEL_HI);
				gfxRect(s, x - GRID_SEL_PAD, y - GRID_SEL_PAD,
				        GRID_CELL + GRID_SEL_PAD * 2, GRID_SEL_H, 3,
				        COL_GREEN);
			}

			px = catArt(idx);

			if (px)
				gfxBlit(s, x, y, GRID_CELL, GRID_CELL, px,
				        CAT_ART_PX, CAT_ART_PX);
			else
				draw_placeholder(s, x, y, GRID_CELL,
				                 g->name[0] ? g->name : g->title_id,
				                 tint_of(g->title_id));

			gfxRect(s, x, y, GRID_CELL, GRID_CELL, 2, COL_LINE);

			/* El candado de "esto no lo puedes jugar". Solo aparece con
			 * el filtro abierto, que es cuando puede haber alguno. */
			if (!g->entitled) {
				gfxFillRect(s, x + GRID_CELL - 30, y + 6, 24, 22, COL_BAR);
				gfxRect(s, x + GRID_CELL - 30, y + 6, 24, 22, 1, COL_WARN);
				/* Un candadito con lo que hay: el arco encima y el
				 * cuerpo debajo. */
				gfxRect(s, x + GRID_CELL - 24, y + 9, 12, 8, 2, COL_WARN);
				gfxFillRect(s, x + GRID_CELL - 26, y + 16, 16, 9, COL_WARN);
			}

			/* La estrella de favorito. */
			if (g->favorite)
				textDraw(s, x + 6, y + 6, 3, COL_WARN, "*");

			draw_name(s, x, y + GRID_CELL + GRID_NAME_Y,
			          g->name[0] ? g->name : g->title_id,
			          slot == grid_sel);
		}
	}
}

/* La ficha: caratula grande, datos y la descripcion entera, que se recorre
 * con el stick derecho porque hay juegos con dos mil caracteres. */
static void draw_ficha(gr33nSurface *s)
{
	const catTitle *t = catTitles(NULL);
	const catTitle *g;
	const u32 *px;

	/* La caratula A TAMANO NATIVO. Antes eran 240 px sacados de los 160
	 * que guarda la rejilla, y gfxBlit coge el pixel mas cercano: a 1,5x
	 * eso son escalones y se veia. Aqui es 256 a 256, uno a uno. */
	int cx = 48, cy = CONTENT_Y + 8, cs = CAT_ART_DL;
	int tx = cx + cs + 40;
	int name_w = W - 48 - tx;                       /* hueco del titulo */
	int name_fits = name_w / (TEXT_ADVANCE * 4);    /* a escala 4 */
	char name[128];
	u32 idx;

	if (!t || !view || view_n == 0 || grid_sel < 0 ||
	    grid_sel >= (int)view_n) return;

	idx = view[grid_sel];
	g   = &t[idx];

	marq_tick(-(grid_sel + 2));   /* su propio reloj, distinto del de la rejilla */

	/* La descripcion NO viene en la cache del disco -son megabytes- asi
	 * que un juego restaurado del disco ya esta marcado como completo y el
	 * lote de datos nunca vuelve a pedirla. Hay que pedirla aparte. */
	catWantDesc(idx);

	gfxFillRect(s, 32, CONTENT_Y, W - 64, CONTENT_H, COL_PANEL);

	px = catArtBig(idx);
	if (px) gfxBlit(s, cx, cy, cs, cs, px, CAT_ART_DL, CAT_ART_DL);
	else if ((px = catArt(idx)) != NULL)
		/* Mientras baja la grande, la de la rejilla ampliada. Se ve
		 * peor, pero se ve: un hueco gris durante medio segundo cada vez
		 * que abres una ficha es peor que una imagen con escalones. */
		gfxBlit(s, cx, cy, cs, cs, px, CAT_ART_PX, CAT_ART_PX);
	else
		draw_placeholder(s, cx, cy, cs,
		                 g->name[0] ? g->name : g->title_id,
		                 tint_of(g->title_id));
	gfxRect(s, cx, cy, cs, cs, 2, COL_LINE);

	/* El titulo tambien se desplaza. El mas largo del catalogo son 46
	 * caracteres y a escala 4 caben 37: se salia por la derecha de la
	 * pantalla, sin borde ni nada que avisara de que habia mas. */
	marq_window(name, sizeof(name), g->name[0] ? g->name : g->title_id,
	            name_fits);
	textDraw(s, tx, cy + 4, 4, COL_TEXT, name);

	if (g->developer[0])
		textDraw(s, tx, cy + 50, 2, COL_TEXT_DIM, g->developer);
	if (g->genre[0])
		textDraw(s, tx, cy + 76, 2, COL_GREEN_DIM, g->genre);

	/* Estado de la suscripcion, por titulo. Es lo unico honesto que se
	 * puede decir: el servicio no da una fecha de caducidad, da si tienes
	 * derecho a cada juego. */
	if (g->entitled) {
		gfxFillRect(s, tx, cy + 110, 220, 52, COL_GREEN);
		{
			const char *lbl = TR("Jugar", "Play");
			textDraw(s, tx + (220 - textWidth(3, lbl)) / 2,
			         cy + 110 + (52 - TEXT_GLYPH_H * 3) / 2, 3,
			         COL_BLACK, lbl);
		}
		textDraw(s, tx + 240, cy + 126, 2, COL_TEXT_DIM, ok_glyph());
	} else {
		gfxRect(s, tx, cy + 110, 380, 52, 2, COL_WARN);
		textDraw(s, tx + 16, cy + 126, 2, COL_WARN,
		         TR("No incluido en tu suscripcion",
		            "Not included in your subscription"));
	}

	if (g->program[0])
		textDraw(s, tx, cy + 176, 2, COL_TEXT_DIM, g->program);

	/* La descripcion de ESTE juego, con su ventana de lineas. */
	{
		const char *d = catDescription(idx);
		int dy = cy + cs + 28;
		int avail = CONTENT_Y + CONTENT_H - dy;
		int lines = avail / 26;
		int total;

		if (lines < 1) lines = 1;

		gfxFillRect(s, 48, dy - 12, W - 96, 2, COL_LINE);

		if (d && d[0]) {
			/* UNA LINEA MENOS CUANDO HAY CONTADOR.
			 *
			 * El "1/29" se pinta abajo del todo, en la misma banda que la
			 * ultima linea de texto, y se le montaba encima: en la
			 * pantalla se leia "...ayuda a tu1/29". Se le deja su sitio
			 * en vez de dibujar los dos en el mismo pixel. */
			int hueco = lines;

			total = textWrapView(s, 48, dy + 4, 2, COL_TEXT_DIM, W - 96, 26,
			                     lines, d, desc_scroll);

			if (total > hueco && lines > 1) {
				lines--;
				total = textWrapView(s, 48, dy + 4, 2, COL_TEXT_DIM,
				                     W - 96, 26, lines, d, desc_scroll);
			}

			/* El tope del desplazamiento sale de aqui, del recuento real
			 * de lineas: la interfaz no tiene por que saber como se
			 * envuelve el texto. */
			desc_max = total - lines;
			if (desc_max < 0) desc_max = 0;

			if (desc_max > 0) {
				char pos[32];
				snprintf(pos, sizeof(pos), "%d/%d",
				         desc_scroll + 1, desc_max + 1);
				textDraw(s, W - 48 - textWidth(2, pos),
				         CONTENT_Y + CONTENT_H - 20, 2, COL_TEXT_DIM, pos);
			}
		} else {
			desc_max = 0;
			textDraw(s, 48, dy + 4, 2, COL_TEXT_DIM,
			         g->detailed
			             ? TR("Este juego no trae descripcion.",
			                  "This game has no description.")
			             : TR("Cargando descripcion...",
			                  "Loading description..."));
		}
	}

	/* EN QUE ANDA LA SESION.
	 *
	 * Se enseña lo que dice el servidor, con su nombre, sin traducir. Si
	 * manda un estado que no conocemos se lee tal cual en la tele, que es
	 * infinitamente mas util que un "desconocido" nuestro. */
	{
		const sesInfo *sn = sesStatus();
		int sy = cy + 176 + (g->program[0] ? 26 : 0);
		char line[160];
		u32 col = COL_WARN;

		line[0] = '\0';

		switch (sn->state) {
		case SES_IDLE:
			if (ficha_msg && !g->entitled)
				snprintf(line, sizeof(line),
				         TR("Sin derecho a este juego: no se pide maquina.",
				    "You are not entitled to this game: no machine requested."));
			break;

		case SES_ASKING:
			snprintf(line, sizeof(line),
			         TR("Pidiendo una maquina a xCloud...",
			            "Asking xCloud for a machine..."));
			break;

		case SES_QUEUED:
			if (sn->wait_s)
				snprintf(line, sizeof(line),
				         TR("En cola: %s (unos %u s)",
				            "In queue: %s (about %u s)"),
				         sn->server_state, (unsigned)sn->wait_s);
			else
				snprintf(line, sizeof(line), TR("En cola: %s", "In queue: %s"),
				         sn->server_state);
			break;

		case SES_CONNECTING:
			snprintf(line, sizeof(line),
			         TR("Diciendole a Microsoft quien eres...",
			            "Telling Microsoft who you are..."));
			break;

		case SES_PROVISIONING:
			snprintf(line, sizeof(line),
			         TR("Preparando la maquina: %s (%u)",
			            "Getting the machine ready: %s (%u)"),
			         sn->server_state[0] ? sn->server_state : "...",
			         (unsigned)sn->polls);
			break;

		case SES_CONFIG:
			snprintf(line, sizeof(line),
			         TR("Pidiendo la configuracion...",
			            "Requesting the configuration..."));
			break;

		case SES_READY:
			col = COL_GREEN;
			if (sn->server_ip[0])
				snprintf(line, sizeof(line),
				         TR("Maquina lista en %u ms (%s:%u es relleno). "
				            "Falta el video.",
				            "Machine ready in %u ms (%s:%u is a "
				            "placeholder). Video still missing."),
				         (unsigned)sn->ms, sn->server_ip,
				         (unsigned)sn->server_port);
			else
				snprintf(line, sizeof(line),
				         TR("Maquina lista en %u ms. Falta WebRTC: mira "
				            "el log.",
				            "Machine ready in %u ms. WebRTC still "
				            "missing: check the log."),
				         (unsigned)sn->ms);
			break;

		case SES_FAILED:
			snprintf(line, sizeof(line), "%.150s", sn->err);
			break;
		}

		if (line[0])
			textWrap(s, tx, sy, 2, col, W - 48 - tx, 24, 3, line);
	}
}

static void draw_library(gr33nSurface *s)
{
	int i;

	/* Lista */
	gfxFillRect(s, LIST_X, CONTENT_Y, LIST_W, CONTENT_H, COL_PANEL);

	if (catalog_n == 0) {
		textDraw(s, LIST_X + 24, CONTENT_Y + 28, 2, COL_TEXT_DIM,
		         TR("Sin juegos", "No games"));
		textWrap(s, LIST_X + 24, CONTENT_Y + 60, 2, COL_TEXT_DIM,
		         LIST_W - 48, 26, 8,
		         "No hay ningun juego disponible para lanzar.");
	}

	/* Se cuenta por filas visibles, no por indice absoluto. Aparte de
	 * leerse mejor, evita el "i < lib_top + MAX_ROWS" que es justo el
	 * patron (X + c) < X que hace saltar -Wstrict-overflow en ppu-gcc. */
	{
		int rows = catalog_n - lib_top;
		if (rows > MAX_ROWS) rows = MAX_ROWS;

		for (i = 0; i < rows; i++) {
			int idx = lib_top + i;
			int ry  = CONTENT_Y + i * ROW_H;
			int sel = (idx == lib_sel);

			if (sel) {
				gfxFillRect(s, LIST_X, ry, LIST_W, ROW_H, COL_PANEL_HI);
				gfxFillRect(s, LIST_X, ry, 5, ROW_H, COL_GREEN);
			}

			textDraw(s, LIST_X + 24, ry + (ROW_H - TEXT_GLYPH_H * 3) / 2, 3,
			         sel ? COL_GREEN : COL_TEXT,
		         tr(catalog[idx]->name, catalog[idx]->name_en));
		}
	}

	/* Detalle */
	gfxFillRect(s, DET_X, CONTENT_Y, DET_W, CONTENT_H, COL_PANEL);

	if (catalog_n == 0) {
		const char *msg = "Nada que mostrar";
		textDraw(s, DET_X + (DET_W - textWidth(3, msg)) / 2,
		         CONTENT_Y + CONTENT_H / 2 - 10, 3, COL_TEXT_DIM, msg);
		return;
	}

	{
		const uiTitle *t = catalog[lib_sel];
		int cx = DET_X + 32, cy = CONTENT_Y + 32, cs = 200;
		int tx = cx + cs + 32;

		draw_cover(s, cx, cy, cs, t);

		textDraw(s, tx, cy + 6, 4, COL_TEXT, tr(t->name, t->name_en));
		textDraw(s, tx, cy + 52, 2, COL_TEXT_DIM,
		         tr(t->publisher, t->publisher_en));

		/* Boton jugar */
		gfxFillRect(s, tx, cy + 110, 220, 52, COL_GREEN);
		{
			const char *lbl = TR("Jugar", "Play");
			textDraw(s, tx + (220 - textWidth(3, lbl)) / 2,
			         cy + 110 + (52 - TEXT_GLYPH_H * 3) / 2, 3, COL_BLACK, lbl);
		}
		textDraw(s, tx + 240, cy + 110 + (52 - TEXT_GLYPH_H * 2) / 2, 2,
		         COL_TEXT_DIM, "X");

		gfxFillRect(s, cx, cy + cs + 32, DET_W - 64, 2, COL_LINE);

		textWrap(s, cx, cy + cs + 56, 2, COL_TEXT_DIM,
		         DET_W - 64, 28, 8, tr(t->desc, t->desc_en));
	}
}

/* Flechas de "esto se cambia con izquierda y derecha". Se dibujan con la
 * fuente para no inventar otra primitiva. */
static void draw_arrows(gr33nSurface *s, int lx, int rx, int y, u32 col)
{
	textDraw(s, lx, y, 2, col, "<");
	textDraw(s, rx, y, 2, col, ">");
}

static u32 lat_color(u32 ms)
{
	if (ms == 0)   return COL_TEXT_DIM;
	if (ms <  50)  return COL_GREEN;
	if (ms < 100)  return COL_WARN;
	return GR33N_RGB(255, 80, 80);
}

/* Lectura en vivo de un eje, en verde si la zona muerta lo ignora. Es
 * toda la calibracion que hace falta: suelta el mando y sube el valor
 * hasta que los cuatro esten en verde. */
static void draw_axis(gr33nSurface *s, int x, int y, const char *name, int v)
{
	int dead = (v <= set_deadzone && v >= -set_deadzone);
	char buf[24];

	snprintf(buf, sizeof(buf), "%s %d", name, v);
	textDraw(s, x, y, 2, dead ? COL_GREEN : COL_WARN, buf);
}

static void draw_settings(gr33nSurface *s)
{
	const int val_r = W - LIST_X - 32;   /* borde derecho del valor */
	const int lat_w = 120;               /* hueco reservado a la latencia */
	int i, desc_y;

	gfxFillRect(s, LIST_X, CONTENT_Y, W - LIST_X * 2, CONTENT_H, COL_PANEL);

	rebuild_settings();
	if (set_sel >= settings_vis_n) set_sel = settings_vis_n - 1;
	if (set_sel < 0) set_sel = 0;
	clamp_scroll(set_sel, settings_vis_n, SET_ROWS, &set_top);

	for (i = set_top;
	     i < settings_vis_n && i < set_top + SET_ROWS;
	     i++) {
		const uiSetting *st = &settings[settings_vis[i]];
		int ry  = CONTENT_Y + (i - set_top) * SET_ROW_H;
		int sel = (i == set_sel);
		int ty  = ry + (SET_ROW_H - TEXT_GLYPH_H * 3) / 2;
		u32 fg  = sel ? COL_GREEN : COL_TEXT;

		if (sel) {
			gfxFillRect(s, LIST_X, ry, W - LIST_X * 2, SET_ROW_H, COL_PANEL_HI);
			gfxFillRect(s, LIST_X, ry, 5, SET_ROW_H, COL_GREEN);
		}

		textDraw(s, LIST_X + 24, ty, 3, fg, tr(st->name, st->name_en));

		switch (st->kind) {
		case SET_ACTION: {
			const char *txt = st->status ? st->status() : ">";
			int vy = ry + (SET_ROW_H - TEXT_GLYPH_H * 2) / 2;

			textDraw(s, val_r - textWidth(2, txt), vy, 2,
			         sel ? COL_GREEN : COL_TEXT_DIM, txt);
			break;
		}

		case SET_TOGGLE: {
			int bx = val_r - 36, by = ry + (SET_ROW_H - 36) / 2;
			int on = *st->value;

			gfxRect(s, bx, by, 36, 36, 2, on ? COL_GREEN : COL_LINE);
			if (on) draw_check(s, bx + 6, by + 4, 26, COL_GREEN);
			break;
		}

		case SET_RANGE: {
			char buf[16];
			int vy = ry + (SET_ROW_H - TEXT_GLYPH_H * 3) / 2;
			int w;

			snprintf(buf, sizeof(buf), "%d", *st->value);
			w = textWidth(3, buf);

			textDraw(s, val_r - 28 - w, vy, 3, fg, buf);
			draw_arrows(s, val_r - 28 - w - 32, val_r - 18,
			            ry + (SET_ROW_H - TEXT_GLYPH_H * 2) / 2,
			            sel ? COL_GREEN : COL_LINE);
			break;
		}

		case SET_CHOICE: {
			int idx = *st->value;
			const char *opt;
			int vy = ry + (SET_ROW_H - TEXT_GLYPH_H * 2) / 2;
			int right = val_r;
			int w;

			/* El indice se recorta AQUI TAMBIEN y no solo al cargar.
			 * La lista de regiones cambia sola cuando termina el login,
			 * y entre que cambia y que clamp_settings vuelve a pasar hay
			 * fotogramas de por medio. Leer st->options[idx] con el
			 * indice de la lista anterior es leer fuera. */
			if (idx < 0 || idx >= opts_n(st)) idx = 0;
			opt = st->options[idx] ? st->options[idx] : "?";
			w = textWidth(2, opt);

			/* La LATENCIA es del selector de servidor y de nadie mas.
			 *
			 * Esto pintaba "-- ms" en cualquier SET_CHOICE, asi que la
			 * esquina del panel de depuracion salia con un ping al lado.
			 * Un numero puesto donde no significa nada no es un adorno:
			 * es una medida inventada. */
			if (st->value == &set_server) {
				/* La opcion 0 es "Automatico" y no es una region: no
				 * tiene direccion propia, asi que se enseña el ping de
				 * la que Microsoft haya marcado por defecto, que es a
				 * la que se ira. Misma cuenta que aplicar_region, y la
				 * misma funcion: si se pintara una region y se jugara
				 * en otra, el numero de la pantalla seria mentira. */
				int reg = idx_region(idx, (int)authRegionsN(),
				                     authRegionDefault());
				u32 ms  = (reg >= 0) ? pingMs(reg) : 0;
				char lat[16];

				/* "-- ms" mientras no se pueda medir, y "x ms" si la
				 * region no contesto. Un hueco vacio se lee como "cero"
				 * y cero es muy optimista; un cero a secas, peor. */
				if (ms)
					snprintf(lat, sizeof(lat), "%u ms", (unsigned)ms);
				else if (reg >= 0 && pingFallo(reg))
					snprintf(lat, sizeof(lat), "x ms");
				else
					snprintf(lat, sizeof(lat), "-- ms");

				textDraw(s, val_r - textWidth(2, lat), vy, 2,
				         lat_color(ms), lat);
				right = val_r - lat_w;
			}

			textDraw(s, right - w, vy, 2, fg, opt);
			draw_arrows(s, right - w - 30, right + 12, vy,
			            sel ? COL_GREEN : COL_LINE);
			break;
		}
		}
	}

	{
		const uiSetting *cur = &settings[settings_vis[set_sel]];

		/* Solo la zona muerta enseña los cuatro ejes en vivo, y hay que
		 * dejarles sitio ANTES de repartir lineas de texto. */
		int axes = (cur->kind == SET_RANGE && cur->value == &set_deadzone);
		int avail, lines;

		/* SITIO FIJO. La raya va donde acaba el hueco de las filas, se
		 * vean todas o no: si la lista se desplaza, el texto no puede ir
		 * subiendo y bajando debajo de ella. */
		{
			int filas = settings_vis_n < SET_ROWS ? settings_vis_n
			                                      : SET_ROWS;
			desc_y = CONTENT_Y + filas * SET_ROW_H + 20;
		}

		gfxFillRect(s, LIST_X + 24, desc_y, W - LIST_X * 2 - 48, 2, COL_LINE);

		/* Cuantos hay y por cual vas. Sin esto, una lista que se desplaza
		 * no dice que se desplaza: parece que faltan ajustes.
		 *
		 * A la IZQUIERDA y JUSTO DEBAJO de la raya, no arriba a la
		 * derecha: ahi caia encima de la casilla de la ultima fila y se
		 * leia "Debug [x] 6/11" como si fuera parte del ajuste. */
		if (settings_vis_n > SET_ROWS) {
			char pos[24];
			snprintf(pos, sizeof(pos), "%d/%d", set_sel + 1, settings_vis_n);
			textDraw(s, W - LIST_X - 32 - textWidth(2, pos), desc_y + 6,
			         2, COL_TEXT_DIM, pos);
		}

		desc_y += 22;

		avail = CONTENT_Y + CONTENT_H - desc_y;
		if (axes) avail -= 30;
		lines = avail / 28;
		if (lines < 1) lines = 1;
		if (lines > SET_DESC_LINEAS) lines = SET_DESC_LINEAS;

		desc_y += textWrap(s, LIST_X + 24, desc_y, 2, COL_TEXT_DIM,
		                   W - LIST_X * 2 - 48, 28, lines,
		                   tr(cur->desc, cur->desc_en)) * 28;

		if (axes) {
			int x = LIST_X + 24;

			desc_y += 8;
			draw_axis(s, x,       desc_y, "lx", pad_lx);
			draw_axis(s, x + 130, desc_y, "ly", pad_ly);
			draw_axis(s, x + 260, desc_y, "rx", pad_rx);
			draw_axis(s, x + 390, desc_y, "ry", pad_ry);
		}
	}
}

void uiDraw(gr33nSurface *s)
{
	gfxFillRect(s, 0, 0, W, H, COL_BG);

	draw_topbar(s);

	{
		char hints[200];

		if (tab == TAB_DEBUG) {
			draw_library(s);
			snprintf(hints, sizeof(hints),
			         TR("%s Lanzar    Arriba/Abajo Mover    L1/R1 Pestana%s",
			            "%s Launch    Up/Down Move    L1/R1 Tab%s"),
			         ok_glyph(),
			         set_quit_app ? TR("    START+SELECT Salir",
			                           "    START+SELECT Quit") : "");
		} else if (tab == TAB_SETTINGS) {
			draw_settings(s);
			snprintf(hints, sizeof(hints),
			         TR("Izq/Der Cambiar    Arriba/Abajo Mover    "
			            "L1/R1 Pestana%s",
			            "Left/Right Change    Up/Down Move    "
			            "L1/R1 Tab%s"),
			         set_quit_app ? TR("    START+SELECT Salir",
			                           "    START+SELECT Quit") : "");
		} else if (in_ficha) {
			draw_ficha(s);
			snprintf(hints, sizeof(hints),
			         TR("Stick der. Descripcion    /\\ Favorito    "
			            "%s Volver",
			            "Right stick Description    /\\ Favourite    "
			            "%s Back"), back_glyph());
		} else {
			draw_grid(s);

			snprintf(hints, sizeof(hints),
			         TR("%s Ficha    [] %s    /\\ Favorito    "
			            "%u juegos    L1/R1 Pestana",
			            "%s Details    [] %s    /\\ Favourite    "
			            "%u games    L1/R1 Tab"),
			         ok_glyph(),
			         genre_sel == 0 ? (show_all ? TR("Todos", "All")
			                                    : TR("En tu suscripcion",
			                                         "In your subscription"))
			                        : genres[genre_sel],
			         (unsigned)view_n);
		}

		draw_botbar(s, hints);
	}
}

void uiDrawStreamHint(gr33nSurface *s)
{
	char msg[48];
	int tw, bw;

	if (mode != UI_STREAM) return;
	if (frame_counter - stream_entered > HINT_FRAMES) return;

	/* Lo que pone en pantalla sale de la MISMA tabla que decide si se
	 * sale, no de un literal en paralelo. Un aviso que dice una
	 * combinacion y un codigo que espera otra es peor que no avisar. */
	{
		int i = (set_exit_combo >= 0 && set_exit_combo < COMBOS_N)
		        ? set_exit_combo : 0;

		if (combo_btn[i].pulsar == 0)
			snprintf(msg, sizeof(msg), "SELECT + %s para volver",
			         back_glyph());
		else
			snprintf(msg, sizeof(msg), TR("%s para volver", "%s to go back"),
			         combo_name[i]);
	}

	tw = textWidth(2, msg);
	bw = tw + 32;

	gfxFillRect(s, (W - bw) / 2, H - 72, bw, 40, COL_BAR);
	gfxRect(s, (W - bw) / 2, H - 72, bw, 40, 2, COL_GREEN_DIM);
	textDraw(s, (W - tw) / 2, H - 72 + 13, 2, COL_TEXT, msg);
}
