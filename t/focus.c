/* Prueba de sobremesa de tres cosas puramente aritmeticas que acaban de
 * cambiar en ui.c:
 *
 *   1. la lista de "lo que hay cerca" que se le pasa al catalogo,
 *   2. el mapa de filas visibles de Ajustes,
 *   3. la geometria de la rejilla, que es donde estaba el pixel comido.
 *
 * Se copian las constantes y la logica tal cual, que es justo lo que hace
 * util esta prueba: si maña alguien toca ui.c y no toca esto, deja de
 * cuadrar y se ve.
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>

/* --- constantes copiadas de ui.c / catalog.h ------------------------- */

#define CAT_ART_PX     160
#define CAT_FOCUS_MAX   64

#define GRID_COLS      7
#define GRID_ROWS      3
#define GRID_CELL      CAT_ART_PX
#define GRID_GAP       16
#define GRID_NAME_Y    6
#define GRID_NAME_H    26
#define GRID_SEL_PAD   4
#define GRID_SEL_H     (GRID_CELL + GRID_NAME_H + 2)
#define GRID_STEP_Y    (GRID_CELL + GRID_NAME_H)
#define GRID_PAGE      (GRID_COLS * GRID_ROWS)

#define TEXT_GLYPH_H   7
#define NAME_SCALE     2

#define TOPBAR_H       64
#define BOTBAR_H       48
#define H              720
#define CONTENT_Y      (TOPBAR_H + 24)
#define CONTENT_H      (H - CONTENT_Y - BOTBAR_H - 24)

#define SET_ROW_H      46

static int fails = 0;

#define CHECK(c, ...) do { if (!(c)) { \
	printf("FALLO %s:%d ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

/* --------------------------------------------------------------------- */
/* 1. La lista de cercania                                               */
/* --------------------------------------------------------------------- */

static unsigned view[4096];
static unsigned near[CAT_FOCUS_MAX];

static unsigned build_near(int first, int view_n)
{
	unsigned near_n = 0;
	int k;
	int vis_end = first + GRID_PAGE;
	int from    = first - GRID_PAGE;
	int to      = vis_end + GRID_PAGE;

	if (from < 0) from = 0;
	if (vis_end > view_n) vis_end = view_n;
	if (to > view_n) to = view_n;

	for (k = first; k < vis_end; k++)
		if (near_n < CAT_FOCUS_MAX) near[near_n++] = view[k];

	for (k = from; k < to; k++) {
		if (k >= first && k < vis_end) continue;
		if (near_n < CAT_FOCUS_MAX) near[near_n++] = view[k];
	}

	return near_n;
}

static void test_near(void)
{
	int view_n, first;

	/* view[] lleva indices de tabla salteados, que es el caso real: con
	 * el filtro por defecto se ven 586 de 2531. */
	for (view_n = 0; view_n <= 600; view_n += 7) {
		int i;

		for (i = 0; i < view_n; i++) view[i] = (unsigned)(i * 4 + 3);

		for (first = 0; first <= view_n + GRID_PAGE; first += GRID_COLS) {
			unsigned n = build_near(first, view_n);
			unsigned a, b;
			int visibles;

			CHECK(n <= CAT_FOCUS_MAX, "near_n %u con view_n %d first %d",
			      n, view_n, first);

			/* Todos los indices tienen que salir de view[]. */
			for (a = 0; a < n; a++)
				CHECK(near[a] % 4 == 3 && near[a] / 4 < (unsigned)view_n,
				      "indice inventado %u (view_n %d)", near[a], view_n);

			/* Sin repetidos: un repetido gasta hueco de los 64 y hace que
			 * catNeed pida dos veces el mismo producto. */
			for (a = 0; a < n; a++)
				for (b = a + 1; b < n; b++)
					CHECK(near[a] != near[b],
					      "repetido %u (view_n %d first %d)",
					      near[a], view_n, first);

			/* Lo VISIBLE va primero, que es de lo que depende que el lote
			 * de dieciseis se gaste en la pantalla y no en la prefetch. */
			visibles = view_n - first;
			if (visibles > GRID_PAGE) visibles = GRID_PAGE;
			if (visibles < 0) visibles = 0;

			for (a = 0; a < (unsigned)visibles; a++)
				CHECK(near[a] == view[first + (int)a],
				      "el visible %u no esta en su sitio (first %d)",
				      a, first);
		}
	}

	printf("cercania: ok\n");
}

/* --------------------------------------------------------------------- */
/* 1b. El cursor de adelanto                                             */
/* --------------------------------------------------------------------- */

/* Copiado de draw_grid. Recorre el margen unos pocos por fotograma, y
 * tiene que aguantar que la lista cambie de tamano debajo (cambio de
 * pestana, filtro, lote de datos nuevo) sin salirse ni atascarse. */
static void test_prefetch(void)
{
	static unsigned pre = 0;
	int ronda;
	unsigned tocados[CAT_FOCUS_MAX];

	/* Caso normal: una pagina visible y dos de margen. Con suficientes
	 * fotogramas hay que haber tocado TODO el margen, y solo el margen. */
	{
		unsigned vis_n = GRID_PAGE, near_n = GRID_PAGE * 3, t;
		int f;

		memset(tocados, 0, sizeof(tocados));
		pre = 0;

        for (f = 0; f < 200; f++) {
			for (t = 0; t < 4 && near_n > vis_n; t++) {
				if (pre < vis_n || pre >= near_n) pre = vis_n;
				CHECK(pre < near_n, "pre %u fuera de near_n %u", pre, near_n);
				tocados[pre]++;
				pre++;
			}
		}

		for (t = 0; t < vis_n; t++)
			CHECK(tocados[t] == 0, "se ha adelantado un visible (%u)", t);
		for (t = vis_n; t < near_n; t++)
			CHECK(tocados[t] > 0, "el margen %u no se ha pedido nunca", t);
	}

	/* Y ahora la lista encoge de golpe, que es lo que pasa al cambiar a
	 * Favoritos con tres juegos marcados. */
	for (ronda = 0; ronda < 200; ronda++) {
		unsigned vis_n = (unsigned)(ronda % (GRID_PAGE + 1));
		unsigned near_n = vis_n + (unsigned)(ronda % 7);
		unsigned t;

		for (t = 0; t < 4 && near_n > vis_n; t++) {
			if (pre < vis_n || pre >= near_n) pre = vis_n;
			CHECK(pre < near_n && pre >= vis_n,
			      "pre %u fuera de [%u,%u)", pre, vis_n, near_n);
			pre++;
		}
	}

	printf("adelanto: ok\n");
}

/* --------------------------------------------------------------------- */
/* 2. Filas visibles de Ajustes                                          */
/* --------------------------------------------------------------------- */

/* mismo orden que settings[] en ui.c */
static const struct { const char *name; int debug_only; } SET[] = {
	{ "Cuenta", 0 },
	{ "Zona muerta", 0 },
	{ "Intercambiar X y O", 0 },
	{ "Servidor", 0 },
	{ "Mostrar juegos no disponibles", 0 },
	{ "Debug", 0 },
	{ "Tamano del panel de depuracion", 1 },
	{ "Esquina del panel de depuracion", 1 }
};
#define SETTINGS_N ((int)(sizeof(SET)/sizeof(SET[0])))

static int vis[SETTINGS_N], vis_n;

static void rebuild_settings(int debug)
{
	int i;
	vis_n = 0;
	for (i = 0; i < SETTINGS_N; i++)
		if (!SET[i].debug_only || debug) vis[vis_n++] = i;
}

static void test_settings(void)
{
	int debug, sel, i;

	for (debug = 0; debug <= 1; debug++) {
		rebuild_settings(debug);

		CHECK(vis_n == (debug ? 8 : 6), "vis_n %d con debug %d", vis_n, debug);

		/* Los dos del panel van SIEMPRE al final, o sea debajo de Debug. */
		if (debug) {
			CHECK(SET[vis[vis_n - 1]].debug_only, "el ultimo no es del panel");
			CHECK(SET[vis[vis_n - 2]].debug_only, "el penultimo no es del panel");
			CHECK(strcmp(SET[vis[vis_n - 3]].name, "Debug") == 0,
			      "Debug no esta justo encima de los suyos");
		}

		/* Cualquier posicion del cursor apunta a una fila que existe. */
		for (sel = 0; sel < vis_n; sel++)
			CHECK(vis[sel] >= 0 && vis[sel] < SETTINGS_N, "fila %d fuera", sel);

		/* Y la lista cabe en la pantalla junto con su descripcion. */
		{
			int desc_y = CONTENT_Y + vis_n * SET_ROW_H + 20 + 22;
			int avail  = CONTENT_Y + CONTENT_H - desc_y;

			CHECK(avail >= 28 * 3, "solo %d px para la descripcion con "
			      "debug %d (menos de tres lineas)", avail, debug);
		}
	}

	/* Apagar Debug con el cursor EN Debug lo deja en Debug, no en otra
	 * fila que se ha corrido hacia arriba. */
	rebuild_settings(1);
	for (i = 0; i < vis_n; i++) if (strcmp(SET[vis[i]].name, "Debug") == 0) break;
	sel = i;

	rebuild_settings(0);
	if (sel >= vis_n) sel = vis_n - 1;
	CHECK(strcmp(SET[vis[sel]].name, "Debug") == 0,
	      "tras apagar Debug el cursor acaba en \"%s\"", SET[vis[sel]].name);

	printf("ajustes: ok\n");
}

/* --------------------------------------------------------------------- */
/* 3. Geometria de la rejilla                                            */
/* --------------------------------------------------------------------- */

static void test_grid_geom(void)
{
	int row;
	int name_h = TEXT_GLYPH_H * NAME_SCALE;

	for (row = 0; row < GRID_ROWS; row++) {
		int y         = CONTENT_Y + row * GRID_STEP_Y;
		int box_top   = y - GRID_SEL_PAD;
		int box_bot   = box_top + GRID_SEL_H - 1;      /* ultima fila del marco */
		int border_up = box_bot - 2;                   /* grosor 3              */
		int name_top  = y + GRID_CELL + GRID_NAME_Y;
		int name_bot  = name_top + name_h - 1;

		/* EL FALLO QUE SE ESTA ARREGLANDO: el borde de abajo empezaba en
		 * la ultima fila de pixeles del nombre. */
		CHECK(border_up > name_bot,
		      "fila %d: el marco empieza en %d y el nombre acaba en %d",
		      row, border_up, name_bot);

		/* Y no puede morder ni la fila de abajo ni la de arriba: el marco
		 * se pinta con relleno, asi que lo que pise, lo borra. */
		if (row + 1 < GRID_ROWS) {
			int next_art = CONTENT_Y + (row + 1) * GRID_STEP_Y;
			CHECK(box_bot < next_art,
			      "fila %d: el marco llega a %d y la caratula de abajo "
			      "empieza en %d", row, box_bot, next_art);
		}

		if (row > 0) {
			int prev_name_bot = CONTENT_Y + (row - 1) * GRID_STEP_Y
			                  + GRID_CELL + GRID_NAME_Y + name_h - 1;
			CHECK(box_top > prev_name_bot,
			      "fila %d: el marco empieza en %d y el nombre de arriba "
			      "acaba en %d", row, box_top, prev_name_bot);
		}

		/* Todo dentro del area de contenido. */
		CHECK(box_bot < CONTENT_Y + CONTENT_H,
		      "fila %d: el marco se sale por abajo (%d de %d)",
		      row, box_bot, CONTENT_Y + CONTENT_H);
	}

	printf("geometria: ok (paso %d, marco %d, contenido %d)\n",
	       GRID_STEP_Y, GRID_SEL_H, CONTENT_H);
}

int main(void)
{
	test_near();
	test_prefetch();
	test_settings();
	test_grid_geom();

	if (fails) { printf("\n%d FALLOS\n", fails); return 1; }
	printf("\ntodo bien\n");
	return 0;
}
