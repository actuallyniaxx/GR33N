/* GR33N - el catalogo de xCloud
 *
 * Dos peticiones a dos servicios distintos, y conviene no confundirlos:
 *
 *   1. {baseUri}/v2/titles          LO QUE ESTA CUENTA PUEDE JUGAR
 *      Autenticado con el gsToken. Devuelve identificadores y derechos:
 *      que titulos hay, si tienes derecho a cada uno, y por que
 *      suscripcion. NO trae nombres ni imagenes.
 *
 *   2. catalog.gamepass.com/v3/products   QUE ES CADA COSA
 *      SIN autenticar. Nombre, descripcion, desarrollador, genero,
 *      caratulas. Es la tienda, y le da igual quien pregunte.
 *
 * El primero es corto y se pide una vez al entrar. El segundo es enorme
 * -cientos de juegos con su descripcion entera son megabytes- y por eso
 * se pide POR LOTES y solo de lo que se va a enseñar. Bajarse el catalogo
 * completo para pintar doce filas en pantalla es tirar el ancho de banda
 * de una consola que va por WiFi de 2007.
 */

#ifndef GR33N_CATALOG_H
#define GR33N_CATALOG_H

#include <ppu-types.h>

/* MEDIDO: la cuenta de prueba trae 2531 titulos.
 *
 * Dos intentos anteriores dieron "exactamente 640" con la tabla en 640 y
 * "exactamente 1536" con la tabla en 1536. Un numero que coincide con tu
 * propio limite no es un dato, es una sospecha - y las dos veces lo era.
 * Solo se supo el numero real cuando se empezo a registrar cuantos traia
 * la RESPUESTA, antes de guardar nada.
 *
 * 4096 da margen para que Microsoft siga anadiendo. La tabla va con malloc
 * (unos 2,4 MB) porque en BSS se pagarian siempre. */
#define CAT_MAX_TITLES  4096

/* Lo que se pide de golpe a la tienda. Ni uno (una peticion por juego, con
 * su negociacion TLS de 300 ms) ni todos (2531 descripciones enteras son
 * decenas de megabytes que no se van a leer). */
#define CAT_BATCH       16

/* Y lo que pide el barrido de FONDO, que es menos.
 *
 * MEDIDO: un lote de 16 tarda entre 1,5 y 12 segundos, y mientras dura el
 * hilo del catalogo no hace nada mas - ni una caratula. Ese es el tiempo
 * que el usuario se come si empieza a moverse justo cuando arranca un lote
 * de fondo, porque no hay forma de cancelar una peticion a medias.
 *
 * A 6 el lote baja a unos 1,2 s. Sale algo peor por titulo (se paga la
 * negociacion TLS mas veces) y muchisimo mejor de respuesta, que es lo que
 * se nota. Lo que pide la INTERFAZ sigue yendo de 16 en 16: eso es lo que
 * se esta mirando y cuanto antes llegue entero, mejor. */
#define CAT_SWEEP_BATCH  6

/* Mercado e idioma de la tienda. Cambia los nombres, las descripciones y
 * hasta que juegos salen. Cuando exista el ajuste de idioma, sale de ahi. */
/* Dos tamanos distintos y conviene no confundirlos.
 *
 * CAT_ART_DL es lo que se le PIDE al CDN. MEDIDO: respeta ?w= y ?h=, y a
 * 256 una caratula son 16,5 KB en JPEG. Pedirla ya pequena es la
 * diferencia entre 16 KB y medio megabyte por juego.
 *
 * CAT_ART_PX es lo que se GUARDA, ya reducido al tamano de la rejilla
 * promediando bloques. Se hace una vez al bajarla; escalar en cada
 * fotograma seria pagarlo sesenta veces por segundo.
 *
 * Se baja a 256 y no a 152 directamente porque la ficha a pantalla
 * completa querra la grande, y asi la misma descarga sirve para las dos
 * cosas cuando llegue ese momento. */
#define CAT_ART_DL      256
#define CAT_ART_PX      160

/* Caratulas decodificadas que caben a la vez.
 *
 * MEDIDO EN PANTALLA: con 28 y 21 visibles, bajar una pantalla y volver
 * desalojaba exactamente lo que se acababa de ver, y las caratulas
 * parpadeaban al subir. 96 son cuatro pantallas y media de margen, o sea
 * que el recorrido normal ya no expulsa nada.
 *
 * 96 x 160 x 160 x 4 = 9,4 MB. En 256 MB es asumible, y el disco ademas
 * guarda los JPEG originales, asi que un desalojo ya no cuesta una
 * descarga. */
#define CAT_ART_SLOTS   96

/* EL MERCADO NO ES EL IDIOMA, Y NO SE TOCA DESDE LOS AJUSTES.
 *
 * El idioma es cosmetica: pides las descripciones en ingles y te las dan.
 * El mercado decide QUE TITULOS EXISTEN para tu cuenta, y no lo eliges
 * tu: lo fija Microsoft con la region de la cuenta. Ponerlo a mano seria
 * pedir un catalogo que luego no se puede jugar.
 *
 * El login SI dice cual es (campo "market" de la respuesta). De momento
 * solo se REGISTRA y se compara con este: si algun dia sale distinto de
 * "ES", lo veremos en el log antes de que nadie se pregunte por que le
 * faltan juegos. Medir primero, cambiar despues. */
#define CAT_MARKET      "ES"

/* Solo el respaldo: el idioma de verdad sale de xclocActual(). */
#define CAT_LANGUAGE    "es-ES"

typedef struct {
	/* MEDIDO: el mas largo del catalogo real son 46 caracteres.
	 *
	 * Yo le habia dado 24 suponiendo que seria un identificador corto tipo
	 * "9PN198B459VB". No lo es: es un nombre en mayusculas sin espacios
	 * ("007FIRSTLIGHT"), y hay titulos con nombres largos. 66 de 640 se
	 * partieron en la primera prueba. 64 deja margen de sobra. */
	char title_id[64];    /* el de xCloud, para abrir sesion     */
	char product_id[24];  /* el de tienda, para pedir los datos  */

	/* Se rellenan en el paso 2, cuando toque enseñar este titulo. */
	char name[96];
	char developer[64];
	char genre[64];
	char art_url[256];    /* la caratula cuadrada                */

	/* La descripcion es larga de verdad y no cabe en la tabla: vive en
	 * su propio sitio y solo la del titulo que se esta mirando. */
	/* POR QUE puedes jugar a esto, no solo si puedes.
	 *
	 * Un booleano seria mas simple y seria un error. Hay al menos tres
	 * caminos distintos hasta el mismo juego: la suscripcion de Game
	 * Pass, tenerlo comprado, y ser gratuito. Y hay un cuarto que existe
	 * y que no vamos a implementar de momento: jugar a un juego que has
	 * comprado, sin suscripcion, aguantando anuncios.
	 *
	 * Ese cuarto camino queda para el final del todo, pero si aqui
	 * guardamos solo un si/no, cuando llegue el momento habra que volver
	 * a bajarse el catalogo entero para averiguar de donde salia el
	 * derecho. Guardar la cadena tal cual cuesta 32 bytes por titulo. */
	char program[32];

	u8   detailed;        /* 1 = ya se pidieron los datos        */
	u8   entitled;        /* hasEntitlement: se puede jugar YA   */
	u8   favorite;
	u8   pad;
} catTitle;

typedef enum {
	CAT_NONE = 0,
	CAT_WORKING,
	CAT_OK,
	CAT_FAILED
} catState;

typedef struct {
	catState state;

	u32 n;            /* titulos guardados                     */
	u32 source_n;     /* titulos que traia la respuesta        */
	u32 skipped;      /* descartados por no caber              */
	u32 entitled_n;   /* de los guardados, cuantos jugables ya */
	u32 bytes;        /* lo que ocupo la respuesta de verdad   */
	u32 ms;

	/* Suscripcion detectada a partir de los derechos de los titulos.
	 * No hay un "dime mi suscripcion" en este servicio: lo que hay es el
	 * derecho a cada juego y el programa por el que lo tienes. */
	char sub[96];

	/* 1 cuando ya se han pedido los datos de tienda de todo el catalogo.
	 * Hasta entonces el filtro por genero solo ve una parte, y la
	 * interfaz lo dice en vez de fingir que la lista esta completa. */
	int  sweep_done;

	/* Sube cada vez que la tabla se reconstruye desde la red. La interfaz
	 * lo mira para volver a aplicar los favoritos: la tabla se sobrescribe
	 * entera y el bit de favorito vive dentro. */
	u32  generation;

	/* Sube con cada lote de datos de tienda que entra. Sin esto la interfaz
	 * no tiene forma de enterarse de que han llegado nombres y generos
	 * nuevos, y una lista filtrada por genero se queda con los que hubiera
	 * cuando se aplico el filtro. */
	u32  details_gen;

	char err[224];
} catInfo;

int  catInit(void);
void catShutdown(void);

/* Pide la lista. No bloquea. Necesita que xCloud haya iniciado sesion. */
int  catFetch(void);

const catInfo  *catStatus(void);
const catTitle *catTitles(u32 *n);

/* La descripcion de UN titulo concreto, o "" si no la tenemos.
 *
 * La primera version guardaba UNA sola descripcion global, la del primer
 * juego que llegara, y la enseñaba en la ficha de todos: cualquier juego
 * que abrieras contaba la historia de James Bond. Ahora hay un anillo de
 * las ultimas CAT_DESC_N, que se llenan solas con el mismo lote que trae
 * los nombres - sin una peticion de mas. */
#define CAT_DESC_N     24
#define CAT_DESC_MAX   4096

const char *catDescription(u32 idx);

/* La caratula de un titulo, ya decodificada y reducida a CAT_ART_PX.
 *
 * Devuelve NULL si todavia no esta - y en ese caso la PIDE. Es decir: la
 * interfaz llama a esto para cada celda visible en cada fotograma, sin
 * pensar, y la caratula aparece cuando aparece. Mientras tanto se pinta el
 * hueco generado, que es lo que ya hacia la interfaz para los titulos de
 * prueba.
 *
 * El puntero vale hasta que esa ranura se desaloje, o sea hasta que se
 * dibujen otras CAT_ART_SLOTS caratulas distintas. Para usarlo dentro de un
 * fotograma sobra de largo; guardarlo de un fotograma para otro no. */
const u32 *catArt(u32 idx);

/* La caratula GRANDE del titulo que se esta mirando, a CAT_ART_DL, sin
 * reducir. Una sola: la ficha enseña un juego cada vez.
 *
 * La rejilla guarda 160 porque es lo que pinta, y ampliar 160 a 240 en la
 * ficha se ve mal - gfxBlit coge el pixel mas cercano y a 1,5x eso son
 * escalones. Aqui se decodifica a tamano nativo y se pinta uno a uno. */
const u32 *catArtBig(u32 idx);

/* Que titulos tiene cerca la rejilla: los visibles y una pantalla de
 * margen por arriba y otra por abajo. La cache no desaloja ninguno de
 * estos, asi que bajar cinco pantallas y volver no tira lo de arriba.
 *
 * Es una LISTA y no un rango, y eso es lo importante. La rejilla FILTRA:
 * por defecto enseña 586 de 2531, asi que los indices que se ven no son
 * consecutivos. Decir "desde el 0, veintiuno" protegia veintiun titulos que
 * no estaban en pantalla mientras desalojaba los que si. Un rango describe
 * bien lo que se ve solo cuando no hay filtro, o sea casi nunca. */
#define CAT_FOCUS_MAX  64

/* `vis` son cuantos de los `n` se estan VIENDO, y tienen que ir los
 * primeros de la lista. La distincion importa: la lista entera decide que
 * no se desaloja, pero solo lo visible decide cuando el barrido de fondo
 * tiene permiso para bloquear el hilo. */
void catFocus(const u32 *idx, u32 n, u32 vis);

/* Pide la descripcion de un titulo si no la tenemos. Hace falta porque el
 * catalogo del disco no guarda descripciones: son megabytes y solo se lee
 * una cada vez. */
void catWantDesc(u32 idx);

/* Guarda el catalogo en disco y lo lee al arrancar.
 *
 * Sin esto cada arranque son 2,3 s de catalogo mas 80 s de barrido para
 * tener los nombres, y 586 descargas de caratula. Con esto, el arranque
 * lee un fichero y el barrido se hace de fondo solo para actualizar. */
int  catLoadCache(void);
void catSaveCache(void);

/* Pide los datos de tienda de los titulos que se estan enseñando, si les
 * faltan. Misma lista y mismo motivo que catFocus: con el filtro puesto,
 * un rango pide los datos de otros juegos distintos de los que se ven. */
void catNeed(const u32 *idx, u32 n);

#endif /* GR33N_CATALOG_H */
