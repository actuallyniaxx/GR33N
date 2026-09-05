/* GR33N - catalogo de xCloud, paso 1: que puede jugar esta cuenta.
 *
 * Este fichero se escribe sabiendo que NO conocemos la forma exacta de la
 * respuesta. Sabemos los nombres de los campos que nos interesan
 * (titleId, details.productId, details.hasEntitlement) pero no como viene
 * envuelto: ¿un array pelado? ¿un objeto con "results" dentro? ¿otra
 * cosa?
 *
 * Asi que en vez de adivinar el envoltorio y fallar en silencio, se
 * prueban los sospechosos habituales y, SI NINGUNO CUELA, se registran los
 * nombres de las claves de primer nivel y un titulo de ejemplo entero. Una
 * sola ejecucion nos dice la forma real y la siguiente version ya no
 * adivina nada.
 *
 * Es la misma leccion de cellVdec y cellPngDec aplicada a un servicio en
 * vez de a una biblioteca: cuando no sabes la forma de algo, el codigo que
 * escribes primero es el que te la enseña.
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>

#include <ppu-types.h>
#include <sys/systime.h>
#include <sys/thread.h>

#include "gr33n.h"
#include "link.h"
#include "store.h"
#include "net_tls.h"
#include "auth.h"
#include "catalog.h"
#include "i18n.h"
#include "imgdec.h"
#include "cJSON.h"

#define AUTH_JSON_CT  "application/json"

/* --------------------------------------------------------------------- */

/* MEDIDO, no supuesto: con 1 MB la respuesta no cupo. El log dijo
 * exactamente eso -"la respuesta no cabe: mas de 1048575 bytes"- en vez de
 * entregar media respuesta, que es lo que habria hecho la version de hace
 * dos dias.
 *
 * Se prueba de mayor a menor porque no sabemos cuanto hay que pedirle a la
 * consola de golpe, y quedarse sin memoria en el arranque es peor que
 * bajarse el catalogo un poco justo. En cuanto un arranque registre el
 * Content-Length real, este numero deja de ser un techo y pasa a ser una
 * medida. */
static const u32 sink_ladder[] = {
	8 * 1024 * 1024,
	6 * 1024 * 1024,
	4 * 1024 * 1024,
	2 * 1024 * 1024
};
#define SINK_LADDER_N ((u32)(sizeof(sink_ladder)/sizeof(sink_ladder[0])))

/* Casi un megabyte (1536 x ~600 bytes). En BSS se pagaria siempre, entres
 * o no en la biblioteca; con malloc se paga al arrancar el catalogo y se
 * suelta al salir. */
static catTitle *titles = NULL;
static catInfo   info;
static u32       titles_n = 0;

/* Anillo de descripciones. Se llenan con el MISMO lote que trae los
 * nombres, asi que no cuestan ni una peticion extra. 24 x 4 KB = 96 KB. */
static char desc_buf[CAT_DESC_N][CAT_DESC_MAX];
static u32  desc_idx[CAT_DESC_N];
static u32  desc_next = 0;

/* La que se esta mirando ahora mismo. No se desaloja. */
static volatile u32 desc_pin = 0xffffffffu;

/* Peticion de datos de la tienda, atendida por el hilo del catalogo. Se
 * declaran aqui arriba porque run_fetch encadena el primer lote y esta
 * definida antes que el paso 2. */
static volatile int fetch_req = 0;
static volatile int det_req  = 0;
static volatile u32 det_from = 0;

/* Lista explicita de que titulos quiere la interfaz, cuando la hay.
 *
 * det_from sirve para el barrido de fondo, que va en orden de tabla y para
 * el que un "desde aqui" es exactamente lo que se necesita. Pero lo que se
 * VE no va en orden de tabla: con el filtro por defecto son 586 de 2531
 * salteados, y pedir "los dieciseis siguientes desde el primero visible"
 * traia cuatro de los que se veian y doce que no. De ahi que los nombres
 * fueran apareciendo a trozos. */
static u32 det_list[CAT_FOCUS_MAX];
static volatile u32 det_list_n = 0;

/* Por donde va el barrido de fondo. Se reinicia con cada tabla nueva. */
static u32 sweep_at = 0;

/* 1 cuando la tabla en memoria tiene algo que el fichero del disco no.
 *
 * Sin esto, arrancar con un catalogo.bin ya completo terminaba el barrido
 * en la primera vuelta y reescribia 1,4 MB identicos al disco duro, en
 * cada arranque, sin que hubiera cambiado un byte. */
static int cache_dirty = 0;

static int art_probed = 0;
static int batches_since_save = 0;

/* Definida mas abajo, junto al resto de la sonda de imagen. */
static void probe_art(catTitle *t);
static void desc_put(u32 idx, const char *txt);
static void cache_reset(void);
static u32  artc_read(const char *id, void *out, u32 max);
static void artc_write(const char *id, const void *data, u32 len);
static void artc_index(void);

#define ART_SINK  (1024 * 1024)

static void clog_(const char *fmt, ...)
{
	char buf[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	printf("[cat] %s\n", buf);
	linkLog("[cat] %s", buf);
}

static void cfail(const char *fmt, ...)
{
	va_list ap;

	info.state = CAT_FAILED;
	va_start(ap, fmt);
	vsnprintf(info.err, sizeof(info.err), fmt, ap);
	va_end(ap);

	printf("[cat] %s\n", info.err);
	linkLog("!! [cat] %s", info.err);
}

static u64 now_us(void)
{
	u64 sec = 0, nsec = 0;
	sysGetCurrentTime(&sec, &nsec);
	return sec * 1000000ull + nsec / 1000ull;
}

/* --------------------------------------------------------------------- */

/* Copia un campo de texto de cJSON. Si no cabe, FALLA.
 *
 * La primera version avisaba y recortaba igual, y eso es exactamente el
 * fallo contra el que lleva todo el proyecto avisando, solo que con un
 * mensaje encima. El log de la primera ejecucion sacó 66 avisos de
 * "titleId mide 34 y solo caben 23 - se recorta" y siguió tan tranquilo:
 * 66 titulos con el identificador partido por la mitad, que es un titulo
 * que no se puede lanzar. Un aviso no arregla un dato corrupto.
 *
 * Devuelve  0 = bien
 *          -1 = no esta o no es texto
 *          -2 = no cabe */
static u32 trunc_warned = 0;

static int copy_str(const cJSON *node, char *out, u32 max, const char *what)
{
	if (!cJSON_IsString(node) || !node->valuestring) return -1;

	if (strlen(node->valuestring) + 1 > max) {
		/* Solo los primeros, o son cien lineas de lo mismo. */
		if (trunc_warned < 3)
			clog_("!! %s mide %u y solo caben %u: se DESCARTA el titulo",
			      what, (unsigned)strlen(node->valuestring),
			      (unsigned)max - 1);
		trunc_warned++;
		out[0] = '\0';
		return -2;
	}

	snprintf(out, max, "%s", node->valuestring);
	return 0;
}

/* Encuentra el array de titulos venga como venga.
 *
 * Si no lo encuentra NO devuelve NULL y ya: deja en el log los nombres de
 * las claves que si hay, que es exactamente lo que hace falta para
 * arreglarlo en el siguiente intento. */
static cJSON *find_titles_array(cJSON *root)
{
	static const char *const wrappers[] = {
		"results", "Results", "titles", "Titles", "items", "value",
		"Products", "products"
	};
	u32 i;
	cJSON *it;

	if (cJSON_IsArray(root)) {
		clog_("la raiz ya es un array");
		return root;
	}

	if (!cJSON_IsObject(root)) {
		cfail("la respuesta no es ni objeto ni array");
		return NULL;
	}

	for (i = 0; i < sizeof(wrappers)/sizeof(wrappers[0]); i++) {
		cJSON *a = cJSON_GetObjectItemCaseSensitive(root, wrappers[i]);
		if (cJSON_IsArray(a)) {
			clog_("los titulos vienen dentro de \"%s\"", wrappers[i]);
			return a;
		}
	}

	/* Ninguno. Que diga lo que SI hay. */
	{
		char keys[256] = "";
		u32 used = 0;

		cJSON_ArrayForEach(it, root) {
			int n;
			if (!it->string) continue;
			n = snprintf(keys + used, sizeof(keys) - used, "%s%s",
			             used ? ", " : "", it->string);
			if (n < 0 || (u32)n >= sizeof(keys) - used) break;
			used += (u32)n;
		}

		cfail("no encuentro el array de titulos. Claves de primer nivel: %s",
		      keys[0] ? keys : "(ninguna)");
	}

	return NULL;
}

/* Deja en el log un titulo entero, tal cual viene. Una vez.
 *
 * Cuatrocientos de estos son megabytes y no caben en ningun log, pero UNO
 * cabe de sobra y contiene toda la informacion sobre la forma: los nombres
 * reales de los campos, como se anida, que tipo tiene cada cosa. Con eso
 * la siguiente version deja de adivinar. */
static void log_sample(const cJSON *item, const char *que)
{
	char *txt = cJSON_PrintUnformatted(item);

	if (!txt) return;

	clog_("--- ejemplo de %s, tal cual viene ---", que);
	{
		const char *p = txt;
		u32 left = (u32)strlen(txt);
		int line = 0;

		/* En trozos, porque el log corta a 256 y esto es mas largo. */
		while (left && line < 20) {
			char chunk[200];
			u32 take = left > sizeof(chunk) - 1 ? sizeof(chunk) - 1 : left;

			memcpy(chunk, p, take);
			chunk[take] = '\0';
			clog_("  %s", chunk);

			p    += take;
			left -= take;
			line++;
		}
		if (left) clog_("  ... (%u bytes mas)", (unsigned)left);
	}

	free(txt);
}

/* La suscripcion no se pregunta: se deduce.
 *
 * Este servicio no tiene un "dime que suscripcion tengo". Lo que tiene es,
 * por cada titulo, por que programa te dejan jugarlo. Si aparece un
 * programa de Game Pass en los titulos a los que tienes derecho, tienes
 * Game Pass. Si NINGUN titulo trae derecho, o no tienes suscripcion o ha
 * caducado - y eso es justo lo que hay que avisarle al usuario. */
static void note_program(const cJSON *details, catTitle *t)
{
	const cJSON *subs = cJSON_GetObjectItemCaseSensitive(details,
	                                                     "userSubscriptions");
	const cJSON *progs = cJSON_GetObjectItemCaseSensitive(details,
	                                                      "userPrograms");
	const cJSON *src = cJSON_IsArray(subs) && cJSON_GetArraySize(subs) > 0
	                 ? subs
	                 : (cJSON_IsArray(progs) ? progs : NULL);
	const cJSON *first;

	if (!src || cJSON_GetArraySize(src) == 0) return;

	first = cJSON_GetArrayItem(src, 0);

	/* Puede ser una cadena suelta o un objeto con nombre dentro. Se
	 * aceptan las dos formas en vez de suponer una. */
	if (cJSON_IsString(first)) {
		snprintf(t->program, sizeof(t->program), "%s", first->valuestring);
	} else if (cJSON_IsObject(first)) {
		const cJSON *nm = cJSON_GetObjectItemCaseSensitive(first, "name");
		if (!cJSON_IsString(nm))
			nm = cJSON_GetObjectItemCaseSensitive(first, "id");
		if (cJSON_IsString(nm))
			snprintf(t->program, sizeof(t->program), "%s", nm->valuestring);
	}

	if (t->program[0] && !info.sub[0])
		snprintf(info.sub, sizeof(info.sub), "%s", t->program);
}

/* --------------------------------------------------------------------- */

static int parse_titles(const char *js)
{
	cJSON *root = cJSON_Parse(js);
	cJSON *arr, *it;
	int sampled = 0;

	if (!root) {
		cfail("la respuesta de /v2/titles no es JSON");
		return -1;
	}

	if (!titles) {
		cfail("la tabla de titulos no esta reservada");
		cJSON_Delete(root);
		return -1;
	}

	arr = find_titles_array(root);
	if (!arr) { cJSON_Delete(root); return -1; }

	titles_n        = 0;
	info.entitled_n = 0;
	info.skipped    = 0;
	info.sub[0]     = '\0';
	trunc_warned    = 0;

	/* Cuantos trae la RESPUESTA, antes de tocar nada.
	 *
	 * Antes solo se registraba lo que cabia en la tabla, y la primera
	 * ejecucion dio "640 titulos" con la tabla de 640: imposible saber si
	 * habia exactamente 640 o si se habian quedado doscientos fuera. Un
	 * numero que coincide con tu propio limite nunca es un dato, es una
	 * sospecha. */
	info.source_n = (u32)cJSON_GetArraySize(arr);
	clog_("la respuesta trae %u titulos", (unsigned)info.source_n);

	cJSON_ArrayForEach(it, arr) {
		catTitle *t;
		cJSON *det, *tid, *pid, *ent;

		if (!sampled) { log_sample(it, "titulo"); sampled = 1; }

		if (titles_n >= CAT_MAX_TITLES) {
			/* Nunca en silencio. Un catalogo cortado que parece
			 * completo es peor que un error. */
			clog_("AVISO: la lista se corta en %u titulos y hay mas",
			      (unsigned)CAT_MAX_TITLES);
			break;
		}

		t = &titles[titles_n];
		memset(t, 0, sizeof(*t));

		tid = cJSON_GetObjectItemCaseSensitive(it, "titleId");
		det = cJSON_GetObjectItemCaseSensitive(it, "details");

		if (copy_str(tid, t->title_id, sizeof(t->title_id), "titleId") != 0) {
			info.skipped++;
			continue;
		}

		if (cJSON_IsObject(det)) {
			pid = cJSON_GetObjectItemCaseSensitive(det, "productId");
			ent = cJSON_GetObjectItemCaseSensitive(det, "hasEntitlement");

			copy_str(pid, t->product_id, sizeof(t->product_id), "productId");

			t->entitled = cJSON_IsTrue(ent) ? 1 : 0;
			note_program(det, t);
		}

		if (t->entitled) info.entitled_n++;
		titles_n++;
	}

	cJSON_Delete(root);

	info.n = titles_n;
	info.generation++;

	/* Tabla nueva: los indices viejos ya no significan lo mismo. */
	cache_reset();

	/* Y el barrido vuelve a empezar, porque `detailed` se ha ido con la
	 * tabla vieja. Sin esto sweep_done se quedaba a 1 sobre un catalogo
	 * entero sin datos. */
	info.sweep_done = 0;
	sweep_at = 0;

	return 0;
}

static void run_fetch(void)
{
	const xcloudInfo *xc = authXCloud();
	static char tok[8192];
	char host[96], path[256], hdr[9216];
	char *sink;
	u32 sink_cap = 0;
	tlsResult res;
	u64 t0 = now_us();
	int n;

	info.state = CAT_WORKING;

	if (!xc || xc->state != XC_OK || !xc->base_uri[0] ||
	    authGsTokenCopy(tok, sizeof(tok)) == 0) {
		cfail("sin sesion de xCloud todavia");
		return;
	}

	/* base_uri viene entero (https://uks.core...), y tlsRequest quiere
	 * servidor y ruta por separado. */
	{
		const char *p = xc->base_uri;
		const char *slash;
		u32 hlen;

		if (strncmp(p, "https://", 8) == 0) p += 8;
		else if (strncmp(p, "http://", 7) == 0) p += 7;

		slash = strchr(p, '/');
		hlen  = slash ? (u32)(slash - p) : (u32)strlen(p);

		if (hlen == 0 || hlen >= sizeof(host)) {
			cfail("no entiendo el baseUri: %.80s", xc->base_uri);
			return;
		}

		memcpy(host, p, hlen);
		host[hlen] = '\0';
	}

	snprintf(path, sizeof(path), "/v2/titles");

	n = snprintf(hdr, sizeof(hdr),
	             "Authorization: Bearer %s\r\n"
	             "Content-Type: application/json\r\n",
	             tok);
	if (n < 0 || (size_t)n >= sizeof(hdr)) {
		cfail("la cabecera de autorizacion no cabe");
		return;
	}

	{
		u32 i;

		sink = NULL;
		for (i = 0; i < SINK_LADDER_N && !sink; i++) {
			sink = (char*)malloc(sink_ladder[i]);
			if (sink) sink_cap = sink_ladder[i];
		}
	}

	if (!sink) {
		cfail("sin memoria ni para %u MB",
		      (unsigned)(sink_ladder[SINK_LADDER_N - 1] / (1024*1024)));
		return;
	}

	clog_("pidiendo el catalogo a %s%s (buffer de %u MB)",
	      host, path, (unsigned)(sink_cap / (1024*1024)));

	{
		int r = tlsRequestTo(host, path, "GET", NULL, NULL, hdr,
		                     sink, sink_cap, &res);

		/* OCUPADO no es FALLO. cfail deja el catalogo en CAT_FAILED para
		 * siempre y la biblioteca enseñando un error; si el hilo de
		 * sesion tiene el turno, lo que hay que hacer es volver a
		 * pedirlo dentro de un momento. */
		if (r == -2) {
			free(sink);
			fetch_req = 1;
			usleep(200000);
			return;
		}

		if (r != 0) {
			cfail("sin respuesta de %s: %s", host, res.err);
			free(sink);
			return;
		}
	}

	{
		const char *js = res.body;
		u32 blen = res.len;

		info.bytes = res.len;

		if (res.http_status != 200) {
			cfail("HTTP %d en /v2/titles%s%.150s", res.http_status,
			      js ? " - " : "", js ? js : "");
			free(sink);
			return;
		}

		if (!js || !blen) {
			cfail("el catalogo vino vacio");
			free(sink);
			return;
		}

		/* EL DATO que decide el tamano del buffer en la proxima
		 * version. Hoy es una corazonada de 1 MB; manana sera un
		 * numero. */
		clog_("respuesta de %u KB de cuerpo", (unsigned)(blen / 1024));

		if (parse_titles(js) != 0) { free(sink); return; }
	}

	free(sink);

	info.state = CAT_OK;
	info.ms    = (u32)((now_us() - t0) / 1000);

	clog_("%u de %u titulos guardados (%u descartados), %u jugables ahora, "
	      "programa \"%s\" (%u ms)",
	      (unsigned)info.n, (unsigned)info.source_n, (unsigned)info.skipped,
	      (unsigned)info.entitled_n,
	      info.sub[0] ? info.sub : "(ninguno)", (unsigned)info.ms);

	if (info.skipped)
		clog_("!! %u titulos descartados por campos que no caben. Eso son "
		      "juegos que no van a aparecer.", (unsigned)info.skipped);

	if (info.entitled_n == 0)
		clog_("AVISO: ni un solo titulo con derecho. O no hay suscripcion "
		      "o ha caducado.");

	/* Y el primer lote de datos, para ver nombres de verdad en el log sin
	 * necesidad de interfaz. Cuando exista la pantalla, esto lo pedira
	 * ella segun lo que haya que pintar. */
	det_list_n = 0;
	det_from = 0;
	det_req  = 1;
}

/* --------------------------------------------------------------------- */
/* Paso 2: que ES cada juego                                             */
/*                                                                       */
/* Otro servicio, otras reglas: catalog.gamepass.com NO pide             */
/* autenticacion. Es la tienda y le da igual quien pregunte; los datos    */
/* son publicos. Lo que si pide son unas cabeceras de identificacion del  */
/* cliente, y sin ellas contesta cosas raras.                            */
/*                                                                       */
/* Se piden POR LOTES y de lo que se va a enseñar. Bajarse las 2531       */
/* descripciones enteras para pintar doce filas seria tirar decenas de    */
/* megabytes por una radio de 2007.                                      */
/* --------------------------------------------------------------------- */

#define STORE_HOST  "catalog.gamepass.com"

/* LA RUTA SE ARMA EN CADA PETICION, no se concatena al compilar.
 *
 * Antes era un literal con CAT_LANGUAGE pegado dentro, o sea el idioma de
 * la tienda decidido en tiempo de compilacion. Ahora sale del mismo ajuste
 * que el idioma del juego: si la biblioteca sale en castellano y el juego
 * arranca en ingles, el ajuste no significa nada.
 *
 * El market NO se toca todavia, y eso es deliberado -- ver el comentario
 * de CAT_MARKET en catalog.h. */
#define STORE_PATH_MAX 160

static const char *store_path(char *buf, u32 n)
{
	int w = snprintf(buf, n,
	                 "/v3/products?market=" CAT_MARKET
	                 "&language=%s"
	                 "&hydration=RemoteHighSapphire0",
	                 xclocActual());

	/* Si no cupiera --no puede, pero-- se vuelve al idioma de siempre en
	 * vez de mandar una ruta cortada, que el servidor contestaria con un
	 * 400 en HTML sin decir por que. Ya paso con el sessionPath. */
	if (w <= 0 || (u32)w >= n)
		snprintf(buf, n, "/v3/products?market=" CAT_MARKET
		                 "&language=" CAT_LANGUAGE
		                 "&hydration=RemoteHighSapphire0");

	return buf;
}

/* MEDIDO: 16 productos son 149.539 bytes, o sea unos 9,3 KB cada uno.
 * 2 MB dan para 200 largos, mas de lo que se va a pedir de una vez.
 *
 * El otro numero medido es peor: esa peticion tardo 2842 ms, y solo 240 de
 * esos son transferencia. El resto lo pasa el servidor montando la
 * respuesta. A ese ritmo, pedir los 2531 titulos serian SIETE MINUTOS, asi
 * que lo de pedir solo lo que se va a enseñar no era una optimizacion
 * elegante: es la unica forma de que esto sea usable. */
#define STORE_SINK  (2 * 1024 * 1024)

/* Igual que con /v2/titles: no sabemos la forma exacta de la respuesta,
 * asi que la primera version la REGISTRA en vez de adivinarla. */
static int det_sampled = 0;

/* Busca un campo por varios nombres posibles. Los servicios de Microsoft
 * mezclan mayusculas y minusculas segun la version de la API, y probar
 * tres nombres cuesta menos que un viaje a la consola. */
static const cJSON *pick(const cJSON *o, const char *const *names, u32 n)
{
	u32 i;

	for (i = 0; i < n; i++) {
		const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, names[i]);
		if (v) return v;
	}

	return NULL;
}

#define PICK(o, ...) \
	({ static const char *const _n[] = { __VA_ARGS__ }; \
	   pick((o), _n, (u32)(sizeof(_n)/sizeof(_n[0]))); })

/* La caratula CUADRADA.
 *
 * Yo habia escrito esto buscando un array "Images" y eligiendo la de
 * proporciones mas cercanas a 1:1. Muy elegante y completamente
 * equivocado: la tienda NO manda un array. Manda campos sueltos con el
 * proposito en el nombre, y ninguno lleva ancho ni alto.
 *
 *   Image_Tile        el mosaico cuadrado  <- este
 *   Image_Poster      vertical, 2:3
 *   Image_Hero        panoramica
 *   Image_TitledHero  panoramica con el titulo encima
 *   Screenshots[]     capturas
 *
 * Asi que se van probando por orden de preferencia. Si algun dia anaden el
 * array, el primer PICK lo cogera igual.
 *
 * Y las URL vienen SIN esquema, empezando por //. */
static void pick_square_art(const cJSON *prod, catTitle *t)
{
	static const char *const fields[] = {
		"Image_Tile", "Image_BoxArt", "Image_Poster",
		"Image_TitledHero", "Image_Hero"
	};
	u32 i;

	for (i = 0; i < sizeof(fields)/sizeof(fields[0]); i++) {
		const cJSON *im = cJSON_GetObjectItemCaseSensitive(prod, fields[i]);
		const cJSON *url;

		if (!cJSON_IsObject(im)) continue;

		url = PICK(im, "URL", "Url", "url", "Uri", "uri");
		if (!cJSON_IsString(url) || !url->valuestring[0]) continue;

		/* Se le piden los parametros de tamano que usa la propia tienda.
		 * Si el CDN los ignora llega la imagen entera y funciona igual
		 * -el decodificador de JPEG sabe reducir- pero si los respeta nos
		 * ahorramos bajar medio megabyte por caratula en una consola que
		 * va por WiFi de 2007. Sin format: que elija el servidor, que
		 * para eso miramos los bytes magicos y no la extension. */
		snprintf(t->art_url, sizeof(t->art_url), "%s%s?w=%u&h=%u",
		         strncmp(url->valuestring, "//", 2) == 0 ? "https:" : "",
		         url->valuestring, (unsigned)CAT_ART_DL,
		         (unsigned)CAT_ART_DL);
		return;
	}
}

static void fill_one(const cJSON *prod, catTitle *t)
{
	const cJSON *v;

	if (!det_sampled) {
		det_sampled = 1;
		log_sample(prod, "producto de la tienda");
	}

	v = PICK(prod, "ProductTitle", "productTitle", "Title", "title");
	if (cJSON_IsString(v)) snprintf(t->name, sizeof(t->name), "%s",
	                                v->valuestring);

	v = PICK(prod, "DeveloperName", "developerName", "Developer",
	         "PublisherName", "publisherName");
	if (cJSON_IsString(v)) snprintf(t->developer, sizeof(t->developer), "%s",
	                                v->valuestring);

	/* El genero suele venir como lista. Se coge el primero, que es el
	 * principal, y con eso basta para filtrar. */
	v = PICK(prod, "Categories", "categories", "Genres", "genres");
	if (cJSON_IsArray(v) && cJSON_GetArraySize(v) > 0) {
		const cJSON *g = cJSON_GetArrayItem(v, 0);
		if (cJSON_IsString(g))
			snprintf(t->genre, sizeof(t->genre), "%s", g->valuestring);
	} else if (cJSON_IsString(v)) {
		snprintf(t->genre, sizeof(t->genre), "%s", v->valuestring);
	}

	pick_square_art(prod, t);

	/* MEDIDO: ProductDescription son varios KB con saltos de linea
	 * dobles y titulos en mayusculas; ProductDescriptionShort es un
	 * parrafo. La larga es la que hay que poder recorrer con el stick
	 * derecho, asi que es la que se guarda - la de ESTE titulo, en su
	 * ranura del anillo. */
	v = PICK(prod, "ProductDescription", "productDescription",
	         "Description", "description");
	if (cJSON_IsString(v))
		desc_put((u32)(t - titles), v->valuestring);

	t->detailed = 1;
}

/* Un lote de datos de tienda.
 *
 * Dos formas de decir que se quiere, y las dos hacen falta:
 *
 *   want != NULL   estos de aqui, en este orden. Lo usa la interfaz para
 *                  pedir justo lo que hay en pantalla.
 *   want == NULL   los CAT_BATCH siguientes sin rellenar a partir de
 *                  `from`. Lo usa el barrido de fondo, que va en orden de
 *                  tabla y para el que un "desde aqui" es lo natural.
 *
 * La primera version solo tenia la segunda, y la interfaz la llamaba con
 * el primer indice visible. Con el filtro por defecto -586 de 2531- los
 * dieciseis siguientes en orden de tabla incluyen unos cuatro de los que
 * se ven: los nombres aparecian a trozos, cuatro por cada vuelta de 2,2 s.
 */
static u32 run_details(const u32 *want, u32 want_n, u32 from, u32 cap)
{
	static char body[CAT_BATCH * 32 + 64];
	static u32 asked[CAT_BATCH];
	tlsResult res;
	char *sink;
	u32 i, n = 0, done = 0;
	u32 total = want ? want_n : titles_n;
	int len;
	u64 t0 = now_us();

	if (!titles) return 0;
	if (!want && from >= titles_n) return 0;

	if (!cap || cap > CAT_BATCH) cap = CAT_BATCH;

	/* El cuerpo: {"Products":["id","id",...]} */
	len = snprintf(body, sizeof(body), "{\"Products\":[");

	/* Se apunta QUE se ha pedido. Sin esta lista no hay forma de marcar
	 * como intentados los que el servicio no devuelva, y el barrido los
	 * vuelve a pedir eternamente. */
	for (i = want ? 0 : from; i < total && n < cap; i++) {
		u32 idx = want ? want[i] : i;
		int k;

		if (idx >= titles_n) continue;
		if (titles[idx].detailed || !titles[idx].product_id[0]) continue;

		k = snprintf(body + len, sizeof(body) - (size_t)len, "%s\"%s\"",
		             n ? "," : "", titles[idx].product_id);
		if (k < 0 || (size_t)(len + k) >= sizeof(body) - 4) break;

		asked[n++] = idx;
		len += k;
	}

	if (!n) return 0;

	/* Si el turno esta cogido, ni se monta el lote: son dos megabytes de
	 * malloc y un free para acabar en -2. A veinte vueltas por segundo
	 * mientras dura la cadena de sesion, eso es fragmentar el monton
	 * justo antes de que run_fetch intente reservar sus ocho megas.
	 *
	 * Y si hay alguien con prioridad -abriendo una sesion de juego- nos
	 * apartamos aunque el modulo este libre. Ver tlsHold(). */
	if (tlsBusy() || tlsHeld()) return 0;

	len += snprintf(body + len, sizeof(body) - (size_t)len, "]}");

	sink = (char*)malloc(STORE_SINK);
	if (!sink) {
		clog_("sin memoria para los datos de la tienda");
		return 0;
	}

	clog_("pidiendo datos de %u titulos a %s", (unsigned)n, STORE_HOST);

	{
		char ruta[STORE_PATH_MAX];
		int r = tlsRequestTo(STORE_HOST, store_path(ruta, sizeof(ruta)),
		                     "POST", AUTH_JSON_CT,
		                     body,
		                     "ms-cv: 0\r\n"
		                     "calling-app-name: Xbox Cloud Gaming Web\r\n"
		                     "calling-app-version: 21.0.0\r\n",
		                     sink, STORE_SINK, &res);

		if (r != 0) {
			/* CERO, no n. Devolver n aqui decia "he pedido n titulos"
			 * cuando no se habia enviado un solo byte, y el barrido se
			 * lo creia: devolvia 1, cat_thread hacia continue SIN dormir,
			 * y volvia a intentarlo. En el log del 23 de agosto salieron
			 * cientos de "pidiendo datos de 6 titulos / sin respuesta" en
			 * el mismo milisegundo.
			 *
			 * Otra vez la respuesta plausible: un camino de fallo que
			 * devuelve algo con pinta de exito. */
			if (r == -2) {
				/* El otro hilo tiene el turno. No es un error: es que
				 * hay que volver luego. */
				free(sink);
				return 0;
			}

			clog_("!! sin respuesta de %s: %s", STORE_HOST, res.err);
			free(sink);
			return 0;
		}
	}

	{
		const char *js = res.body;
		/* arr e it son const porque PICK devuelve const: el arbol de la
		 * tienda se lee, no se toca. root no puede serlo porque hay que
		 * liberarlo. */
		cJSON *root;
		const cJSON *arr, *it;

		/* LOS TRES CAMINOS DE ABAJO SALEN ANTES de marcar `asked`, asi
		 * que ninguno de los seis queda con datos ni con "ya lo intente".
		 *
		 * Devolver n aqui es decirle al barrido "he hecho algo": vuelve
		 * a llamar sin dormir, sweep_at sigue donde estaba, y se pide el
		 * MISMO lote otra vez. Contra un 429 eso son dos peticiones y
		 * media por segundo al mismo sitio, para siempre, que es la
		 * forma mas rapida de que la tienda te bloquee.
		 *
		 * Se devuelve 0 y se duerme. La misma leccion que el r != 0 de
		 * arriba, tres veces mas: arregle una de las cuatro caras y me
		 * quede tan tranquilo. */
		if (res.http_status != 200) {
			clog_("!! HTTP %d en la tienda%s%.140s", res.http_status,
			      js ? " - " : "", js ? js : "");
			free(sink);
			return 0;
		}

		root = js ? cJSON_Parse(js) : NULL;
		if (!root) {
			clog_("!! la respuesta de la tienda no es JSON");
			free(sink);
			return 0;
		}

		/* MEDIDO: la tienda contesta { "Products": {...}, "InvalidIds": [...] }
		 * y Products es un OBJETO indexado por productId, no un array.
		 *
		 * Mi buscador generico lo descarto justamente por eso: el nombre
		 * estaba en la lista pero cJSON_IsArray decia que no. El mensaje
		 * de fallo ("Claves de primer nivel: Products, InvalidIds") es lo
		 * que permitio verlo en una sola ejecucion.
		 *
		 * Y resulta comodo: la clave YA es el productId, asi que no hay
		 * que buscarlo dentro ni emparejar por posicion. */
		arr = PICK(root, "Products", "products");

		if (!arr) {
			clog_("!! la tienda no devolvio \"Products\"");
			cJSON_Delete(root);
			free(sink);
			return 0;
		}

		{
			const cJSON *bad = PICK(root, "InvalidIds", "invalidIds");
			int nbad = cJSON_IsArray(bad) ? cJSON_GetArraySize(bad) : 0;

			if (nbad)
				clog_("la tienda no reconoce %d de los %u identificadores",
				      nbad, (unsigned)n);
		}

		cJSON_ArrayForEach(it, arr) {
			const char *pid = it->string;   /* la clave ES el productId */
			u32 j;

			/* Por si algun dia lo cambian a array. */
			if (!pid || !pid[0]) {
				const cJSON *p = PICK(it, "ProductId", "productId",
				                      "Id", "id");
				if (!cJSON_IsString(p)) continue;
				pid = p->valuestring;
			}

			/* Una clave vacia casaria con TODOS los titulos sin
			 * product_id y les meteria los datos de este producto. */
			if (!pid[0]) continue;

			/* TODOS los que tengan ese productId, no solo el primero.
			 *
			 * MEDIDO EN EL LOG: "11 productos pedidos no volvieron".
			 * No es que no volvieran: varios titleId comparten un mismo
			 * productId (un juego y su edicion, por ejemplo). La tienda
			 * devuelve UNA entrada por producto, se rellenaba solo el
			 * primer titulo, y los demas se quedaban sin datos - asi
			 * que el barrido los volvia a pedir en el siguiente lote, y
			 * en el siguiente. Por eso el catalogo no terminaba de
			 * cargar nunca. */
			for (j = 0; j < titles_n; j++) {
				if (strcmp(titles[j].product_id, pid) != 0) continue;
				fill_one(it, &titles[j]);
				done++;
			}
		}

		cJSON_Delete(root);
	}

	free(sink);

	/* Aviso a la interfaz de que hay datos nuevos. Se sube UNA vez por
	 * lote y no una por titulo: lo que se quiere es "reconstruye la
	 * vista", no "reconstruyela dieciseis veces". */
	info.details_gen++;
	cache_dirty = 1;

	clog_("%u de %u titulos con datos (%u ms)", (unsigned)done, (unsigned)n,
	      (unsigned)((now_us() - t0) / 1000));

	/* Cada cierto numero de lotes, al disco. Apagar la consola a los dos
	 * minutos no deberia tirar dos minutos de descargas. */
	if (++batches_since_save >= 20) {
		batches_since_save = 0;
		cache_dirty = 0;
		catSaveCache();
	}

	/* Y los que se pidieron y no volvieron se marcan como intentados.
	 *
	 * Sin esto, un producto que la tienda no reconoce (los hay: salen en
	 * InvalidIds) se vuelve a pedir en cada pasada del barrido y el
	 * catalogo no termina jamas. Quedan sin nombre, que es la verdad, y
	 * la rejilla enseña su identificador. */
	{
		u32 pending = 0;

		for (i = 0; i < n; i++) {
			if (titles[asked[i]].detailed) continue;
			titles[asked[i]].detailed = 1;
			pending++;
		}

		if (pending)
			clog_("%u sin datos en la tienda: marcados, no se vuelven a "
			      "pedir", (unsigned)pending);
	}

	/* Tres ejemplos de lo que ha quedado, para verlo sin interfaz. */
	{
		u32 shown = 0;

		for (i = from; i < titles_n && shown < 3; i++) {
			if (!titles[i].detailed) continue;
			clog_("ej: \"%s\" | %s | %s",
			      titles[i].name[0] ? titles[i].name : "(sin nombre)",
			      titles[i].developer[0] ? titles[i].developer : "(sin autor)",
			      titles[i].genre[0] ? titles[i].genre : "(sin genero)");
			shown++;
		}
	}

	/* Y la sonda de imagen sobre el primero que traiga caratula. Una vez
	 * por sesion: es una prueba, no la version buena. */
	/* La sonda se marca como hecha SI SE HACE. Se ponia antes de llamar,
	 * asi que si el turno estaba cogido -normal en los primeros segundos-
	 * la sonda se perdia para toda la sesion y el log decia "sin
	 * intentar" como si fuera un fallo de la caratula. */
	if (!art_probed && !tlsBusy()) {
		for (i = 0; i < titles_n; i++) {
			if (!titles[i].detailed || !titles[i].art_url[0]) continue;
			art_probed = 1;
			clog_("caratula: %.150s", titles[i].art_url);
			probe_art(&titles[i]);
			break;
		}
	}

	return n;
}

/* --------------------------------------------------------------------- */
/* Sonda: bajar y decodificar UNA caratula                               */
/*                                                                       */
/* No es la version buena -esa necesita una cache de imagenes que se      */
/* disena cuando exista la rejilla- sino la prueba de que el camino       */
/* entero funciona: URL de la tienda -> descarga -> bytes magicos ->      */
/* decodificador que toque -> pixeles.                                   */
/*                                                                       */
/* Y de paso mide lo unico que no sabemos: cuanto pesa una caratula de    */
/* verdad, en que formato viene, y si el CDN respeta el tamano pedido.    */
/* Tres numeros que hacen falta para dimensionar la cache y que no se     */
/* pueden adivinar desde aqui.                                           */
/* --------------------------------------------------------------------- */

static void probe_art(catTitle *t)
{
	char host[128], path[512];
	char *sink;
	imgImage img;
	tlsResult res;
	int rc;

	if (!t->art_url[0]) { clog_("sonda: ese titulo no trae caratula"); return; }

	rc = tlsSplitUrl(t->art_url, host, sizeof(host), path, sizeof(path));
	if (rc != 0) {
		clog_("!! sonda: no entiendo la URL de la caratula (%d): %.90s",
		      rc, t->art_url);
		return;
	}

	sink = (char*)malloc(ART_SINK);
	if (!sink) { clog_("!! sonda: sin memoria"); return; }

	clog_("sonda: bajando la caratula de \"%s\"", t->name);

	if (tlsRequestTo(host, path, "GET", NULL, NULL, NULL,
	                 sink, ART_SINK, &res) != 0) {
		clog_("!! sonda: %s", res.err);
		free(sink);
		return;
	}

	{
		const char *data;
		u32 len = 0;

		if (res.http_status != 200) {
			clog_("!! sonda: HTTP %d al bajar la caratula",
			      res.http_status);
			free(sink);
			return;
		}

		data = res.body;
		len  = res.len;
		if (!data || !len) {
			clog_("!! sonda: caratula vacia");
			free(sink);
			return;
		}

		/* Los primeros bytes, en claro. Si el formato no es ni PNG ni
		 * JPEG, esto lo dice antes que cualquier codigo de error. */
		{
			const unsigned char *b = (const unsigned char*)data;
			clog_("sonda: %u bytes, empieza por %02x %02x %02x %02x",
			      (unsigned)len, b[0], b[1], b[2], b[3]);
		}

		if (imgDecodeTo(data, len, NULL, 0, 0, &img) != 0) {
			clog_("!! sonda: %s", img.err);
			free(sink);
			return;
		}

		clog_("sonda: LISTA. %ux%u pixeles de %u bytes en %u ms",
		      (unsigned)img.w, (unsigned)img.h, (unsigned)len,
		      (unsigned)img.ms);

		if (img.w != CAT_ART_DL || img.h != CAT_ART_DL)
			clog_("sonda: el CDN NO respeto el tamano pedido (%ux%u); "
			      "se pidio %ux%u", (unsigned)img.w, (unsigned)img.h,
			      (unsigned)CAT_ART_DL, (unsigned)CAT_ART_DL);
	}

	free(sink);
}

/* --------------------------------------------------------------------- */
/* Cache en disco                                                        */
/*                                                                       */
/* Sin esto, cada arranque son 2,3 s de catalogo, 80 s de barrido para    */
/* tener los nombres y 586 descargas de caratula a 600 ms cada una. Con   */
/* esto, el arranque lee dos ficheros del disco duro y el resto se        */
/* actualiza de fondo sin que se note.                                   */
/*                                                                       */
/* Dos ficheros y no uno porque tienen vidas distintas: la tabla se       */
/* reescribe entera de vez en cuando y las caratulas solo se ANADEN, que  */
/* con nueve megabytes es la diferencia entre escribir 16 KB y escribirlo */
/* todo cada vez que llega una imagen.                                   */
/* --------------------------------------------------------------------- */

#define CACHE_FILE   "catalogo.bin"
#define CACHE_MAGIC  0x47523301u

typedef struct {
	u32 magic;
	u32 rec_size;   /* sizeof(catTitle) */
	u32 count;
	u32 entitled;
} cacheHead;

int catLoadCache(void)
{
	cacheHead h;
	FILE *f;
	size_t got;
	u64 t0 = now_us();

	if (!titles) return -1;

	f = storeOpen(CACHE_FILE, "rb");
	if (!f) return -1;

	if (fread(&h, 1, sizeof(h), f) != sizeof(h)) { fclose(f); return -1; }

	/* rec_size es la comprobacion que importa. Si algun dia se le anade
	 * un campo a catTitle, la cache vieja tiene otro tamano de registro y
	 * leerla daria basura con pinta de datos. Comparar el tamano es mas
	 * fiable que acordarse de subir un numero de version a mano. */
	if (h.magic != CACHE_MAGIC || h.rec_size != (u32)sizeof(catTitle) ||
	    h.count == 0 || h.count > CAT_MAX_TITLES) {
		clog_("cache del catalogo no vale (magic %08x, registro %u, %u titulos)",
		      (unsigned)h.magic, (unsigned)h.rec_size, (unsigned)h.count);
		fclose(f);
		return -1;
	}

	got = fread(titles, sizeof(catTitle), h.count, f);
	fclose(f);

	if (got != h.count) {
		clog_("!! cache cortada: %u de %u titulos", (unsigned)got,
		      (unsigned)h.count);
		return -1;
	}

	titles_n        = h.count;
	info.n          = h.count;
	info.source_n   = h.count;
	info.entitled_n = h.entitled;
	info.state      = CAT_OK;
	info.ms         = (u32)((now_us() - t0) / 1000);
	info.generation++;   /* tabla nueva: la vista de la interfaz ya no vale */
	cache_reset();       /* y los indices de las caches, tampoco */

	clog_("catalogo desde el disco: %u titulos, %u jugables (%u ms)",
	      (unsigned)titles_n, (unsigned)info.entitled_n, (unsigned)info.ms);

	return 0;
}

void catSaveCache(void)
{
	cacheHead h;
	FILE *f;

	if (!titles || !titles_n) return;

	f = storeOpen(CACHE_FILE, "wb");
	if (!f) return;

	h.magic    = CACHE_MAGIC;
	h.rec_size = (u32)sizeof(catTitle);
	h.count    = titles_n;
	h.entitled = info.entitled_n;

	if (fwrite(&h, 1, sizeof(h), f) == sizeof(h) &&
	    fwrite(titles, sizeof(catTitle), titles_n, f) == titles_n)
		clog_("catalogo guardado: %u titulos (%u KB)", (unsigned)titles_n,
		      (unsigned)((titles_n * sizeof(catTitle)) / 1024));
	else
		clog_("!! no se pudo guardar el catalogo");

	fclose(f);
}

/* --------------------------------------------------------------------- */
/* EN DISCO: caratulas y descripciones                                   */
/*                                                                       */
/* Dos ficheros con el mismo formato y una sola implementacion. De las    */
/* caratulas se guarda el JPEG original y no los pixeles: 16 KB contra    */
/* 100 KB, y decodificarlo cuesta 8 ms, que al lado de los 600 ms de      */
/* bajarlo es gratis.                                                    */
/*                                                                       */
/* El fichero solo crece: [id 24][longitud][bytes] una detras de otra. Al */
/* arrancar se recorre saltando de cabecera en cabecera para construir un */
/* indice en memoria, sin leer ni un byte de contenido.                   */
/*                                                                       */
/* Las descripciones no estaban aqui y se notaba: "sales, vuelves a       */
/* entrar y otra vez sin descripcion hasta que se descarga". El anillo de */
/* 24 en memoria se vacia en cuanto miras 24 juegos, y volver a pedir una */
/* son 2,8 s de peticion HTTPS para un texto de dos kilobytes que ya      */
/* habiamos bajado.                                                       */
/* --------------------------------------------------------------------- */

#define ARTC_FILE   "caratulas.bin"
#define DESCC_FILE  "descripciones.bin"

typedef struct {
	char id[24];
	u32  off;
	u32  len;
} blobEntry;

typedef struct {
	const char *file;
	const char *what;      /* para el log */
	blobEntry  *idx;
	u32         n;
	u32         cap_len;   /* lo mas grande que se acepta guardar */
} blobStore;

static blobStore artc  = { ARTC_FILE,  "caratulas",     NULL, 0, 4u * 1024u * 1024u };
static blobStore descc = { DESCC_FILE, "descripciones", NULL, 0, CAT_DESC_MAX };

static void blob_index(blobStore *b)
{
	FILE *f;
	u32 off = 0;

	if (b->idx) return;

	b->idx = (blobEntry*)calloc(CAT_MAX_TITLES, sizeof(blobEntry));
	if (!b->idx) return;

	f = storeOpen(b->file, "rb");
	if (!f) return;

	while (b->n < CAT_MAX_TITLES) {
		char id[24];
		u32 len;

		if (fread(id, 1, sizeof(id), f) != sizeof(id)) break;
		if (fread(&len, 1, sizeof(len), f) != sizeof(len)) break;
		if (len == 0 || len > b->cap_len) break;

		off += (u32)sizeof(id) + (u32)sizeof(len);

		memcpy(b->idx[b->n].id, id, sizeof(id));
		b->idx[b->n].id[sizeof(id) - 1] = '\0';
		b->idx[b->n].off = off;
		b->idx[b->n].len = len;
		b->n++;

		off += len;
		if (fseek(f, (long)len, SEEK_CUR) != 0) break;
	}

	fclose(f);

	if (b->n) clog_("%s en disco: %u", b->what, (unsigned)b->n);
}

/* La entrada mas RECIENTE con ese identificador, o -1.
 *
 * Hacia atras a proposito: el fichero solo crece, asi que si algo se
 * reescribio, la buena es la ultima. */
static int blob_find(blobStore *b, const char *id)
{
	u32 i;

	if (!b->idx || !id || !id[0]) return -1;

	for (i = b->n; i > 0; i--)
		if (strcmp(b->idx[i - 1].id, id) == 0) return (int)(i - 1);

	return -1;
}

static u32 blob_read(blobStore *b, const char *id, void *out, u32 max)
{
	FILE *f;
	int i = blob_find(b, id);
	size_t got;

	if (i < 0 || b->idx[i].len > max) return 0;

	f = storeOpen(b->file, "rb");
	if (!f) return 0;

	if (fseek(f, (long)b->idx[i].off, SEEK_SET) != 0) { fclose(f); return 0; }
	got = fread(out, 1, b->idx[i].len, f);
	fclose(f);

	return got == b->idx[i].len ? b->idx[i].len : 0;
}

static void blob_write(blobStore *b, const char *id, const void *data, u32 len)
{
	FILE *f;
	char rec[24];
	long off;

	if (!b->idx || b->n >= CAT_MAX_TITLES || !len || len > b->cap_len) return;
	if (!id || !id[0]) return;

	/* Ya esta: no se escribe dos veces. El fichero solo crece, asi que un
	 * duplicado es espacio tirado para siempre. */
	if (blob_find(b, id) >= 0) return;

	f = storeOpen(b->file, "ab");
	if (!f) return;

	memset(rec, 0, sizeof(rec));
	snprintf(rec, sizeof(rec), "%s", id);

	off = ftell(f);

	if (fwrite(rec, 1, sizeof(rec), f) == sizeof(rec) &&
	    fwrite(&len, 1, sizeof(len), f) == sizeof(len) &&
	    fwrite(data, 1, len, f) == len) {
		memcpy(b->idx[b->n].id, rec, sizeof(rec));
		b->idx[b->n].off = (u32)off + (u32)sizeof(rec) + (u32)sizeof(len);
		b->idx[b->n].len = len;
		b->n++;
	}

	fclose(f);
}

/* Los tres nombres de siempre, para no tocar el resto del fichero. */
static void artc_index(void) { blob_index(&artc); blob_index(&descc); }

static u32 artc_read(const char *id, void *out, u32 max)
{
	return blob_read(&artc, id, out, max);
}

static void artc_write(const char *id, const void *data, u32 len)
{
	blob_write(&artc, id, data, len);
}

/* --------------------------------------------------------------------- */
/* Cache de caratulas                                                    */
/*                                                                       */
/* El reparto de trabajo entre los dos hilos es lo unico delicado aqui, y */
/* se resuelve sin candados con una regla sencilla:                      */
/*                                                                       */
/*   - El hilo de DIBUJO reserva ranuras (escribe title y pone ready=0)   */
/*     y NUNCA toca una que no este lista, porque puede que el otro este  */
/*     escribiendo pixeles justo ahi.                                    */
/*   - El hilo del CATALOGO solo rellena ranuras ya reservadas, y pone    */
/*     ready=1 LO ULTIMO, cuando los pixeles ya estan.                   */
/*                                                                       */
/* Asi el peor caso de una carrera es que una caratula tarde un fotograma */
/* mas en aparecer. No hay ninguno en el que se pinte basura.            */
/* --------------------------------------------------------------------- */

typedef struct {
	/* volatile los DOS, y no solo `ready`.
	 *
	 * La reserva son dos escrituras y el otro hilo mira las dos. Si el
	 * compilador puede mover la de `title` -y podia, porque no era
	 * volatile- la ranura pasa por (title = el viejo, ready = 0), que es
	 * justo el patron que art_pump interpreta como "reservada para el
	 * viejo": se baja la caratula equivocada y se publica bajo el nombre
	 * nuevo. Pintar basura, que es lo que la regla de aqui arriba dice que
	 * no puede pasar. */
	volatile u32 title; /* indice en titles[], o ART_NONE */
	u32 stamp;          /* ultima vez que alguien la pidio */
	volatile int ready; /* 1 = los pixeles valen */
	u32 *px;
} artSlot;

#define ART_NONE  0xffffffffu

static artSlot *art = NULL;
static u32 art_clock = 0;

/* Los titulos que la rejilla tiene cerca, por indice de tabla. Lo escribe
 * y lo lee el mismo hilo -el de dibujo-, asi que no hace falta nada. */
static u32 focus[CAT_FOCUS_MAX];
static u32 focus_n = 0;

/* De esos, cuantos se estan VIENDO. Van los primeros de la lista.
 *
 * Hace falta separarlos del margen porque mandan sobre cosas distintas:
 * para NO DESALOJAR cuenta la lista entera -bajar cinco pantallas y volver
 * no deberia tirar nada-, pero para DEJAR ARRANCAR al barrido de fondo solo
 * cuenta lo visible. Si el margen tambien contara, en una instalacion nueva
 * el barrido no empezaria jamas. */
static u32 focus_vis = 0;

void catFocus(const u32 *idx, u32 n, u32 vis)
{
	if (!idx || !n) { focus_n = 0; focus_vis = 0; return; }

	if (n > CAT_FOCUS_MAX) n = CAT_FOCUS_MAX;
	if (vis > n) vis = n;

	memcpy(focus, idx, (size_t)n * sizeof(u32));
	focus_n   = n;
	focus_vis = vis;
}

/* 1 si este titulo esta EN PANTALLA ahora mismo, no solo cerca. */
static int art_on_screen(u32 idx)
{
	u32 i;

	for (i = 0; i < focus_vis; i++)
		if (focus[i] == idx) return 1;

	return 0;
}

/* 0 = esta cerca de lo que se esta mirando, no se toca.
 * 1 = candidato a caer, y entre los candidatos manda la antiguedad.
 *
 * Es binario y no una distancia porque la distancia solo tiene sentido en
 * el orden de la REJILLA, que no es el de la tabla en cuanto hay un filtro
 * puesto. La interfaz ya sabe cual es ese orden, asi que manda la lista
 * hecha y aqui solo hay que mirar si esta o no.
 *
 * Sin lista, TODO es candidato: se cae al desalojo por antiguedad de toda
 * la vida. Devolver 0 aqui seria decir que todo esta cerca, o sea proteger
 * las noventa y seis ranuras justo cuando la interfaz acaba de decir que
 * no esta enseñando nada. */
static u32 art_far(u32 idx)
{
	u32 i;

	if (!focus_n) return 1;

	for (i = 0; i < focus_n; i++)
		if (focus[i] == idx) return 0;

	return 1;
}

static int art_alloc(void)
{
	u32 i;

	if (art) return 0;

	art = (artSlot*)calloc(CAT_ART_SLOTS, sizeof(artSlot));
	if (!art) return -1;

	for (i = 0; i < CAT_ART_SLOTS; i++) {
		art[i].title = ART_NONE;
		art[i].px = (u32*)malloc((size_t)CAT_ART_PX * CAT_ART_PX * 4);
		if (!art[i].px) {
			clog_("!! solo caben %u caratulas en memoria", (unsigned)i);
			break;
		}
	}

	clog_("cache de caratulas: %u ranuras de %ux%u (%u KB)",
	      (unsigned)CAT_ART_SLOTS, (unsigned)CAT_ART_PX, (unsigned)CAT_ART_PX,
	      (unsigned)((CAT_ART_SLOTS * (size_t)CAT_ART_PX * CAT_ART_PX * 4) / 1024));

	return 0;
}

static void art_free(void)
{
	u32 i;

	if (!art) return;

	for (i = 0; i < CAT_ART_SLOTS; i++) free(art[i].px);
	free(art);
	art = NULL;
}

const u32 *catArt(u32 idx)
{
	u32 i, victim = ART_NONE, oldest = 0;

	if (!art || !titles || idx >= titles_n) return NULL;

	art_clock++;

	/* ¿Ya esta? */
	for (i = 0; i < CAT_ART_SLOTS; i++) {
		if (art[i].title != idx || !art[i].px) continue;
		art[i].stamp = art_clock;
		return art[i].ready ? art[i].px : NULL;
	}

	/* Sin URL no hay nada que pedir. Se devuelve NULL y la interfaz pinta
	 * el hueco generado, que para eso esta. */
	if (!titles[idx].detailed || !titles[idx].art_url[0]) return NULL;

	/* Una ranura: primero una vacia, si no LA MAS LEJANA de lo que se
	 * esta viendo. Nunca una a medio rellenar.
	 *
	 * Antes se desalojaba la mas vieja, y eso es exactamente lo que hace
	 * que bajar cinco pantallas y volver encuentre el principio
	 * descargado: al bajar, lo de arriba es lo que lleva mas rato sin
	 * pedirse, asi que era lo primero en caer. Por distancia, lo que
	 * acabas de dejar atras es lo ultimo que se tira. */
	for (i = 0; i < CAT_ART_SLOTS; i++) {
		if (!art[i].px) continue;

		if (art[i].title == ART_NONE) { victim = i; break; }
		if (!art[i].ready) continue;

		{
			u32 far = art_far(art[i].title);

			/* A igualdad de distancia, la mas vieja. */
			if (far > oldest || (far == oldest && victim != ART_NONE &&
			                     art[i].stamp < art[victim].stamp)) {
				oldest = far;
				victim = i;
			} else if (victim == ART_NONE) {
				oldest = far;
				victim = i;
			}
		}
	}

	if (victim == ART_NONE) return NULL;   /* todas en vuelo, se reintenta */

	/* EL ORDEN IMPORTA. Primero se anula, luego se baja ready, y solo al
	 * final se pone el titulo nuevo.
	 *
	 * Al reves -ready y despues title- la ranura pasaba por (titulo VIEJO,
	 * ready 0), que es exactamente lo que art_pump busca para ponerse a
	 * trabajar. Dos instrucciones de ventana, pero el PPU tiene dos hilos
	 * de verdad y art_pump no duerme mientras queden ranuras que llenar.
	 * El resultado era la caratula del juego viejo publicada bajo el
	 * nombre del nuevo, hasta el siguiente desalojo. */
	art[victim].title = ART_NONE;
	art[victim].ready = 0;
	art[victim].stamp = art_clock;
	art[victim].title = idx;

	return NULL;
}

/* La caratula GRANDE, para la ficha. Una sola ranura.
 *
 * La rejilla guarda 160 porque es lo que pinta. La ficha la enseñaba a 240
 * ampliando desde esos 160 con gfxBlit, que coge el pixel mas cercano: a
 * 1,5x eso son escalones, y se veia. Aqui se decodifica a tamano nativo y
 * se pinta uno a uno.
 *
 * Una ranura basta porque la ficha enseña un juego cada vez. Son 256 KB. */
static u32 *big_px = NULL;
static u32  big_idx = ART_NONE;
static volatile int big_ready = 0;
static volatile u32 big_want = ART_NONE;

const u32 *catArtBig(u32 idx)
{
	if (!titles || idx >= titles_n) return NULL;

	if (big_idx == idx) return big_ready ? big_px : NULL;

	big_want = idx;
	return NULL;
}

static int big_pump(void)
{
	char host[128], path[512];
	char *sink;
	imgImage img;
	tlsResult res;
	u32 idx = big_want;

	if (idx == ART_NONE || !titles || idx >= titles_n) return 0;
	if (big_idx == idx) { big_want = ART_NONE; return 0; }

	big_want = ART_NONE;

	/* ESTE indice queda atendido pase lo que pase, y por eso se apunta
	 * antes de intentarlo. Si sale mal, big_ready se queda en 0 y la ficha
	 * tira de la caratula de la rejilla, que es lo correcto.
	 *
	 * Sin esta linea, un fallo dejaba big_idx apuntando a otro titulo,
	 * catArtBig volvia a pedir el mismo en el fotograma siguiente, y otra
	 * vez, y otra: sesenta intentos por segundo contra el CDN mientras la
	 * ficha estuviera abierta. */
	big_ready = 0;
	big_idx   = idx;

	if (!big_px) {
		big_px = (u32*)malloc((size_t)CAT_ART_DL * CAT_ART_DL * 4);
		if (!big_px) return 0;
	}

	if (!titles[idx].art_url[0]) return 0;
	if (tlsSplitUrl(titles[idx].art_url, host, sizeof(host),
	                path, sizeof(path)) != 0) return 0;

	sink = (char*)malloc(ART_SINK);
	if (!sink) return 0;

	{
		u32 have = artc_read(titles[idx].product_id, sink, ART_SINK);

		if (!have && tlsHeld()) {
			/* Con prioridad puesta no se baja: se vuelve a pedir
			 * luego, que para eso big_want existe. */
			big_idx  = ART_NONE;
			big_want = idx;
			free(sink);
			return 0;
		}

		if (!have) {
			int r = tlsRequestTo(host, path, "GET", NULL, NULL, NULL,
			                     sink, ART_SINK, &res);

			/* OCUPADO NO ES "NO HAY". Si el hilo de sesion tiene el
			 * turno -los ocho segundos de la cadena de Xbox Live al
			 * arrancar, que es justo cuando el usuario abre su primera
			 * ficha- salir de aqui con big_idx puesto y big_ready a 0
			 * dejaba catArtBig devolviendo NULL para siempre: nadie
			 * volvia a poner big_want. La caratula grande de ese juego
			 * se perdia el resto de la sesion. */
			if (r == -2) {
				big_idx  = ART_NONE;
				big_want = idx;      /* que se vuelva a pedir */
				free(sink);
				return 0;
			}

			have = res.len;

			if (r == 0 && res.http_status == 200 && res.body && have &&
			    imgLooksLikeImage(res.body, have)) {
				memmove(sink, res.body, have);
				artc_write(titles[idx].product_id, sink, have);
			} else {
				have = 0;
			}
		}

		/* El tope de tamano que habia aqui sobra: imgShrink reduce lo
		 * que le echen, y rechazar una caratula por venir a 512 era
		 * quedarse sin imagen teniendola delante. */
		if (have && imgDecodeTo(sink, have, big_px,
		                        CAT_ART_DL, CAT_ART_DL, &img) == 0)
			big_ready = 1;   /* LO ULTIMO, con los pixeles ya puestos */
	}

	free(sink);
	return 1;
}

/* --------------------------------------------------------------------- */

/* La descripcion de UN titulo, pedida a la tienda.
 *
 * Hace falta porque el catalogo del disco NO guarda descripciones: son
 * megabytes y solo se lee una cada vez. Al abrir una ficha de un juego que
 * vino de la cache, el anillo esta vacio y hay que ir a por ella. */
static volatile u32 desc_want = ART_NONE;

/* El ultimo que se intento, salga bien o mal.
 *
 * Sin esto, un juego que la tienda no describe -los hay: salen en
 * InvalidIds, o sencillamente vienen sin el campo- se pedia otra vez en el
 * fotograma siguiente, y otra, y otra: un POST HTTPS completo cada 2,8 s
 * mientras la ficha estuviera abierta. Y como las descripciones van antes
 * que las caratulas en el hilo, mientras tanto no bajaba ni una imagen.
 * En pantalla ponia "Este juego no trae descripcion", que es verdad, asi
 * que no habia nada raro que mirar.
 *
 * Es la misma proteccion que ya tenia big_pump, que la aprendio por el
 * mismo camino. */
static volatile u32 desc_tried = ART_NONE;

void catWantDesc(u32 idx)
{
	if (!titles || idx >= titles_n) return;

	/* La que se esta mirando no se desaloja del anillo aunque entre un
	 * lote entero de golpe. */
	desc_pin = idx;

	if (catDescription(idx)[0]) return;      /* ya la tenemos */
	if (desc_tried == idx) return;           /* ya se intento y no habia */

	desc_want = idx;
}

static int desc_pump(void)
{
	static char body[128];
	char *sink;
	tlsResult res;
	u32 idx = desc_want;

	if (idx == ART_NONE || !titles || idx >= titles_n) {
		desc_want = ART_NONE;   /* un indice invalido tambien queda atendido */
		return 0;
	}

	desc_want = ART_NONE;

	if (!titles[idx].product_id[0]) { desc_tried = idx; return 0; }

	/* EL DISCO PRIMERO, igual que con las caratulas. Leerla son unos
	 * milisegundos; pedirla a la tienda, 2,8 segundos durante los cuales
	 * el hilo no hace nada mas. */
	{
		static char cached[CAT_DESC_MAX];
		u32 have = blob_read(&descc, titles[idx].product_id,
		                     cached, sizeof(cached) - 1);

		if (have) {
			cached[have] = '\0';
			desc_put(idx, cached);
			return 1;
		}
	}

	/* El disco ya se ha mirado ahi arriba y no lo tenia. Si ademas el
	 * turno esta cogido -o hay prioridad puesta- se vuelve a pedir luego
	 * sin reservar nada. */
	if (tlsBusy() || tlsHeld()) { desc_want = idx; return 0; }

	snprintf(body, sizeof(body), "{\"Products\":[\"%s\"]}",
	         titles[idx].product_id);

	sink = (char*)malloc(STORE_SINK);
	if (!sink) return 0;

	{
		char ruta[STORE_PATH_MAX];
		int r = tlsRequestTo(STORE_HOST, store_path(ruta, sizeof(ruta)),
		                     "POST", AUTH_JSON_CT,
		                     body,
		                     "ms-cv: 0\r\n"
		                     "calling-app-name: Xbox Cloud Gaming Web\r\n"
		                     "calling-app-version: 21.0.0\r\n",
		                     sink, STORE_SINK, &res);

		/* OCUPADO NO ES "NO HAY". El hilo de sesion tiene el turno un
		 * par de segundos al arrancar; si eso contara como intento, la
		 * ficha que abras en ese rato se quedaria sin descripcion hasta
		 * que te fueras a otro juego y volvieras. */
		if (r == -2) { free(sink); return 0; }

		desc_tried = idx;
		if (r != 0) { free(sink); return 0; }
	}

	{
		const char *js = res.body;
		cJSON *root = (res.http_status == 200 && js) ? cJSON_Parse(js) : NULL;

		if (root) {
			const cJSON *prods = PICK(root, "Products", "products");
			const cJSON *it;

			if (prods) {
				cJSON_ArrayForEach(it, prods) {
					const cJSON *d = PICK(it, "ProductDescription",
					                      "productDescription",
					                      "Description", "description");
					if (cJSON_IsString(d)) {
						desc_put(idx, d->valuestring);
						break;
					}
				}
			}

			cJSON_Delete(root);
		}
	}

	free(sink);
	return 1;
}

/* Rellenar ranuras reservadas, y en el orden que importa.
 *
 * MEDIDO EN EL LOG DE LA CONSOLA (23 ago, 310 s de sesion): entre el
 * segundo 80 y el 130 se decodifico UNA caratula. En los ultimos 30 s, 598.
 * La diferencia no es la red ni el decodificador: es que en el primer tramo
 * el hilo estaba dentro de un lote de datos de tienda -hubo uno de 11,9 s- y
 * en el segundo no habia ninguno. 53 lotes, 193 de los 310 segundos.
 *
 * De ahi las tres reglas de aqui abajo, que son las que pidio el usuario con
 * otras palabras: "para, y dibuja lo que hay EN PANTALLA, no 17 filas mas
 * abajo".
 *
 *   1. Lo que se ha ido de la pantalla se SUELTA. Una reserva vieja es una
 *      descarga de 600 ms que nadie va a mirar, y mientras tanto espera lo
 *      que si se ve.
 *   2. Lo que esta en el disco se sirve TODO de una vez. Son 8-12 ms cada
 *      una y no tocan la red; hacer una por vuelta del hilo era gratuito y
 *      lento a la vez.
 *   3. Y solo despues, UNA de la red. Esa si bloquea, y al volver hay que
 *      mirar otra vez que sigue haciendo falta. */

/* Cuantas ranuras hay pedidas y sin rellenar entre lo que se esta viendo.
 * El barrido de fondo mira esto para no ponerse a bloquear el hilo cuando
 * la pantalla todavia esta a medias. */
static u32 art_pending(int solo_visibles)
{
	u32 i, n = 0;

	if (!art) return 0;

	for (i = 0; i < CAT_ART_SLOTS; i++) {
		if (!art[i].px || art[i].title == ART_NONE || art[i].ready) continue;
		if (solo_visibles && !art_on_screen(art[i].title)) continue;
		n++;
	}

	return n;
}

static int art_pump(void)
{
	u32 i, idx = ART_NONE, slot = ART_NONE;
	char host[128], path[512];
	char *sink;
	imgImage img;
	tlsResult res;
	int served = 0, slot_visible = 0;

	if (!art || !titles) return 0;

	/* 1. Soltar lo que ya no se ve. */
	for (i = 0; i < CAT_ART_SLOTS; i++) {
		if (!art[i].px || art[i].title == ART_NONE || art[i].ready) continue;
		if (art_far(art[i].title)) art[i].title = ART_NONE;
	}

	if (!art_pending(0)) return 0;

	sink = (char*)malloc(ART_SINK);
	if (!sink) return 0;

	/* 2. Todo lo que ya esta en el disco, y de paso se elige a quien le
	 *    toca la red: el primero que no estuviera. */
	for (i = 0; i < CAT_ART_SLOTS; i++) {
		u32 t = art[i].title, have;

		if (!art[i].px || t == ART_NONE || art[i].ready) continue;

		if (t >= titles_n || !titles[t].art_url[0]) {
			art[i].title = ART_NONE;
			continue;
		}

		have = artc_read(titles[t].product_id, sink, ART_SINK);

		if (have && imgDecodeTo(sink, have, art[i].px,
		                        CAT_ART_PX, CAT_ART_PX, &img) == 0) {
			art[i].ready = 1;
			served++;
			continue;
		}

		/* Candidato para la red. Manda lo que se VE: el margen es
		 * adelanto, y adelantar no puede ir antes que lo que el usuario
		 * tiene delante ahora mismo. */
		if (art_on_screen(t)) {
			if (slot == ART_NONE || !slot_visible) {
				slot = i; idx = t; slot_visible = 1;
			}
		} else if (slot == ART_NONE) {
			slot = i; idx = t;
		}
	}

	/* Si ha salido algo del disco se vuelve YA, sin tocar la red: puede
	 * que la interfaz haya pedido otra cosa mientras, y bloquearse 600 ms
	 * ahora seria empezar tarde. */
	if (served) {
		if (served >= 4)
			clog_("%d caratulas del disco de golpe", served);
		free(sink);
		return 1;
	}

	if (slot == ART_NONE) { free(sink); return 0; }

	if (tlsSplitUrl(titles[idx].art_url, host, sizeof(host),
	                path, sizeof(path)) != 0) {
		art[slot].title = ART_NONE;
		free(sink);
		return 1;
	}

	/* Con prioridad puesta no se baja nada de la red. El disco de ahi
	 * arriba si, que no la toca. */
	if (tlsHeld()) { free(sink); return 0; }

	/* 3. Y una sola de la red.
	 *
	 * Si el modulo esta ocupado -el hilo de sesion en mitad de la cadena
	 * de Xbox Live- la reserva SE QUEDA en pie y se vuelve con las manos
	 * vacias. Soltarla seria tirar una peticion que sigue haciendo falta,
	 * y devolver 1 seria decir que se ha hecho algo: el hilo volveria a
	 * intentarlo sin dormir. */
	{
		int r = tlsRequestTo(host, path, "GET", NULL, NULL, NULL,
		                     sink, ART_SINK, &res);

		if (r == -2) { free(sink); return 0; }
		if (r != 0)  { art[slot].title = ART_NONE; free(sink); return 1; }
	}

	{
		const char *data = res.body;
		u32 len = res.len;

		if (res.http_status == 200 && data && len && imgLooksLikeImage(data, len)) {
			/* AL DISCO, pero solo despues de mirar que sea una imagen.
			 *
			 * Antes se guardaba cualquier cosa con HTTP 200 y bytes.
			 * caratulas.bin solo crece y blob_write no reescribe una id
			 * que ya este, asi que meter ahi una respuesta cortada -o
			 * unas cabeceras HTTP- envenenaba esa entrada PARA SIEMPRE:
			 * cada arranque futuro la leia, fallaba al decodificar, y
			 * volvia a la red.
			 *
			 * Se guarda aunque la ranura ya no valga: la descarga esta
			 * pagada, y tirarla porque el usuario haya seguido bajando
			 * es pagarla otra vez dentro de diez segundos. */
			artc_write(titles[idx].product_id, data, len);

			/* Y a los pixeles solo si la ranura sigue siendo suya. */
			if (art[slot].title == idx &&
			    imgDecodeTo(data, len, art[slot].px,
			                CAT_ART_PX, CAT_ART_PX, &img) == 0)
				art[slot].ready = 1;   /* LO ULTIMO */
			else
				art[slot].title = ART_NONE;
		} else {
			art[slot].title = ART_NONE;

			/* BORRAR LA URL ES PARA SIEMPRE, asi que solo cuando el
			 * servidor haya dicho que no de verdad.
			 *
			 * art_url vive dentro de catTitle, y catTitle se escribe
			 * entero en catalogo.bin. Un 404 es definitivo y se apunta;
			 * un corte de conexion, un cuerpo vacio o un HTTP 0 son
			 * pasajeros, y borrar la URL por eso dejaba ese juego sin
			 * caratula en todos los arranques siguientes, sin forma de
			 * recuperarla: al venir del disco ya llega con detailed = 1,
			 * asi que ni el barrido ni catNeed lo vuelven a mirar. */
			if (res.http_status >= 400 && res.http_status < 500) {
				titles[idx].art_url[0] = '\0';
				clog_("caratula de \"%s\": HTTP %d, no existe",
				      titles[idx].name, res.http_status);
			} else {
				clog_("caratula de \"%s\": no se pudo (HTTP %d), se "
				      "reintentara", titles[idx].name, res.http_status);
			}
		}
	}

	free(sink);
	return 1;
}

/* --------------------------------------------------------------------- */

/* Completa el catalogo poco a poco, empezando por lo jugable. Devuelve 1
 * si ha PEDIDO algo de verdad.
 *
 * Lo de "de verdad" no es una floritura. Antes devolvia 1 por haber
 * llamado a run_details, y run_details puede no pedir nada: se salta los
 * titulos sin product_id, que no entran en `asked[]` y por tanto nunca
 * quedan marcados como intentados. En cuanto habia UNO asi, este bucle lo
 * encontraba, llamaba, no se pedia nada, devolvia 1, y cat_thread hacia
 * `continue` SIN DORMIR. Un hilo del PPU al cien por cien recorriendo 2531
 * entradas decenas de miles de veces por segundo, compartiendo nucleo con
 * el hilo de dibujo. Nadie lo veria en el log: no hay ni una linea.
 *
 * Ahora se salta esos titulos aqui tambien, y ademas se cree lo que
 * run_details dice que ha hecho en vez de suponerlo. */
static int sweep_pump(void)
{
	u32 i;

	if (!titles || info.state != CAT_OK) return 0;

	/* Terminado es terminado. Sin esto se recorrian los 2531 dos veces,
	 * veinte veces por segundo, para no encontrar nada, durante el resto
	 * de la sesion. `detailed` no vuelve nunca a 0 dentro de una misma
	 * tabla, y parse_titles ya reinicia esto cuando llega una nueva. */
	if (info.sweep_done) return 0;

	/* EL BARRIDO CEDE EL PASO. Esta es la regla que faltaba.
	 *
	 * Estaba el ultimo de la lista de prioridades, que parecia suficiente,
	 * y no lo era: un lote de tienda BLOQUEA el hilo entre 1,5 y 12
	 * segundos, y durante ese rato no se baja ni una caratula por mucho que
	 * la interfaz las este pidiendo a gritos. Medido: 53 lotes, 193 de los
	 * 310 segundos de la sesion, y un tramo de 50 segundos con exactamente
	 * una caratula decodificada.
	 *
	 * Ser el ultimo de la cola no sirve de nada si cuando te toca no
	 * sueltas el turno hasta dentro de doce segundos. Asi que ni se empieza
	 * mientras quede algo pendiente de lo que se esta viendo. */
	if (art_pending(1) || big_want != ART_NONE || desc_want != ART_NONE)
		return 0;

	/* Y ni se asoma si hay alguien con prioridad. El barrido es lo mas
	 * prescindible de todo el programa: existe para que el filtro por
	 * genero vea los 586, no para nada que el usuario este esperando. */
	if (tlsHeld()) return 0;

	/* Y solo los JUGABLES, que son los que se ven por defecto.
	 *
	 * Los otros 1945 son 120 lotes mas -unos siete minutos de red- para
	 * rellenar juegos que con el filtro por defecto no aparecen. Los que se
	 * miren de verdad los pide catNeed cuando salgan en pantalla, que es
	 * como funciona todo lo demas aqui. */
	for (i = sweep_at; i < titles_n; i++) {
		if (titles[i].detailed || !titles[i].entitled) continue;
		if (!titles[i].product_id[0]) continue;
		sweep_at = i;
		return run_details(NULL, 0, i, CAT_SWEEP_BATCH) > 0;
	}

	{
		u32 huerfanos = 0, sin_datos = 0;

		for (i = 0; i < titles_n; i++) {
			if (!titles[i].product_id[0]) huerfanos++;
			else if (!titles[i].detailed) sin_datos++;
		}

		info.sweep_done = 1;
		clog_("jugables completos: %u titulos con datos, %u sin pedir "
		      "(no jugables, se piden al verlos)",
		      (unsigned)info.entitled_n, (unsigned)sin_datos);

		if (huerfanos)
			clog_("%u titulos sin productId: la tienda no puede decir nada "
			      "de ellos y se quedan con su identificador",
			      (unsigned)huerfanos);

		if (cache_dirty) { cache_dirty = 0; catSaveCache(); }
	}

	return 0;
}

/* La interfaz dice QUE titulos esta enseñando; si a alguno le falta el
 * nombre, se pide el lote con esos y solo esos.
 *
 * Se copia la lista aqui y el hilo del catalogo la lee cuando le toque. El
 * orden importa: primero los datos, y det_req al final, que es lo que el
 * otro hilo mira. */
void catNeed(const u32 *idx, u32 n)
{
	u32 i, k = 0;

	if (!titles || info.state != CAT_OK || det_req || !idx) return;

	for (i = 0; i < n && k < CAT_FOCUS_MAX; i++) {
		if (idx[i] >= titles_n || titles[idx[i]].detailed) continue;

		/* Sin productId la tienda no puede decir nada. Si no se filtra
		 * aqui, la interfaz sube det_req en cada fotograma para un lote
		 * que siempre sale vacio. */
		if (!titles[idx[i]].product_id[0]) continue;

		det_list[k++] = idx[i];
	}

	if (!k) return;

	det_list_n = k;
	det_req    = 1;
}

/* --------------------------------------------------------------------- */

/* Hilo propio, y no colgado del de sesion, por dos motivos.
 *
 * El primero es de dependencias: si el hilo de sesion llamara aqui,
 * auth.c tendria que conocer catalog.c y catalog.c ya conoce auth.c. Un
 * circulo que hoy no molesta y dentro de tres ficheros si.
 *
 * El segundo pesa mas: lo que viene despues de esto son las peticiones por
 * lotes a la tienda, que son muchas y largas. Bloquear ahi el hilo que
 * tambien renueva los tokens seria elegir entre tener caratulas y seguir
 * autenticado. */
#define CAT_THREAD_PRIO   1004
#define CAT_THREAD_STACK  (192 * 1024)

static sys_ppu_thread_t cat_tid;
static volatile int cat_running = 0;
static int cat_started = 0;

static void cat_thread(void *arg)
{
	(void)arg;

	/* EL ORDEN ES LA POLITICA, y cada linea de aqui abajo cuesta segundos
	 * de espera en pantalla si esta en el sitio equivocado.
	 *
	 * Nada de esto se puede interrumpir: una peticion a la tienda bloquea
	 * el hilo entre 1,5 y 12 segundos y no hay forma de cancelarla a
	 * medias. Asi que "ser el ultimo de la cola" no protege de nada; lo
	 * que protege es no EMPEZAR lo lento mientras quede algo de lo que se
	 * esta mirando.
	 *
	 * De arriba abajo: lo que hay abierto en la ficha, luego los nombres
	 * de la rejilla, luego sus caratulas, y el barrido de fondo solo
	 * cuando no queda nada de lo anterior. */
	while (cat_running) {
		if (fetch_req) {
			fetch_req = 0;
			run_fetch();
			continue;
		}

		/* La ficha abierta. Las dos tiran del disco primero, asi que en
		 * el caso normal son milisegundos y por eso van las primeras: el
		 * usuario esta mirando ESO. */
		if (big_pump())  continue;
		if (desc_pump()) continue;

		/* Los nombres de lo que hay en pantalla. */
		if (det_req) {
			static u32 want[CAT_FOCUS_MAX];
			u32 ln = det_list_n;

			/* Copiar ANTES de bajar det_req, que es lo que le da permiso
			 * al hilo de dibujo para escribir otra lista encima. Lo peor
			 * que puede pasar en la ventana que queda es un lote de
			 * productos de la pagina anterior, que se arregla solo en la
			 * siguiente vuelta. */
			if (ln > CAT_FOCUS_MAX) ln = CAT_FOCUS_MAX;
			if (ln) memcpy(want, det_list, (size_t)ln * sizeof(u32));

			det_req = 0;

			if (ln) run_details(want, ln, 0, CAT_BATCH);
			else    run_details(NULL, 0, det_from, CAT_BATCH);
			continue;
		}

		/* Y sus caratulas. Sin nombre no hay nada que enseñar y sin URL
		 * no hay caratula que bajar, de ahi que vaya despues. */
		if (art_pump())  continue;

		/* Solo cuando no queda nada visible pendiente: completar los
		 * jugables que todavia no tienen datos, para que el filtro por
		 * genero vea los 586 y no los dieciseis que se han mirado.
		 *
		 * sweep_pump vuelve a comprobar por su cuenta que no hay nada
		 * pendiente. Es a proposito: entre este punto y el siguiente
		 * fotograma el usuario puede haber movido la rejilla. */
		if (sweep_pump()) continue;

		usleep(50000);
	}

	sysThreadExit(0);
}

int catInit(void)
{
	memset(&info, 0, sizeof(info));
	titles_n = 0;
	{
		u32 d;
		for (d = 0; d < CAT_DESC_N; d++) {
			desc_buf[d][0] = '\0';
			desc_idx[d] = 0xffffffffu;
		}
	}

	if (art_alloc() != 0) clog_("sin memoria para la cache de caratulas");
	artc_index();

	titles = (catTitle*)calloc(CAT_MAX_TITLES, sizeof(catTitle));
	if (!titles) {
		cfail("sin memoria para la tabla de titulos (%u KB)",
		      (unsigned)((CAT_MAX_TITLES * sizeof(catTitle)) / 1024));
		return -1;
	}

	cat_running = 1;
	if (sysThreadCreate(&cat_tid, cat_thread, NULL, CAT_THREAD_PRIO,
	                    CAT_THREAD_STACK, THREAD_JOINABLE,
	                    "GR33N catalogo") != 0) {
		cat_running = 0;
		cfail("sysThreadCreate del hilo de catalogo fallo");
		return -1;
	}
	cat_started = 1;

	/* Lo primero de todo: si hay catalogo guardado, la biblioteca ya
	 * tiene juegos antes de que la red diga ni hola. La actualizacion de
	 * verdad llega despues y de fondo. */
	catLoadCache();

	return 0;
}

void catShutdown(void)
{
	fetch_req = 0;

	/* Cortar la peticion en vuelo ANTES de esperar, o el join se queda
	 * esperando a un hilo metido en una llamada de red. Ya nos paso con
	 * el de sesion y no se repite. */
	tlsAbort();

	if (cat_started) {
		u64 rv = 0;
		cat_running = 0;
		sysThreadJoin(cat_tid, &rv);
		cat_started = 0;
	}

	/* Despues del join, nunca antes: el hilo podria estar escribiendo. */
	free(titles);
	titles = NULL;
	titles_n = 0;
	art_free();
	free(big_px);
	big_px = NULL;

	free(artc.idx);  artc.idx  = NULL;  artc.n  = 0;
	free(descc.idx); descc.idx = NULL;  descc.n = 0;
}

int catFetch(void)
{
	const xcloudInfo *xc = authXCloud();

	if (!xc || xc->state != XC_OK) return -1;
	if (!cat_started) return -1;

	fetch_req = 1;
	return 0;
}

const catInfo *catStatus(void) { return &info; }

const catTitle *catTitles(u32 *n)
{
	if (n) *n = titles ? titles_n : 0;
	return titles;
}

const char *catDescription(u32 idx)
{
	u32 i;

	for (i = 0; i < CAT_DESC_N; i++)
		if (desc_idx[i] == idx) return desc_buf[i];

	return "";
}

static void desc_put(u32 idx, const char *txt)
{
	u32 i, tries;

	/* Si ya esta, se refresca EN SU SITIO y el cursor del anillo no se
	 * mueve.
	 *
	 * Antes se hacia desc_next = i, o sea que la siguiente descripcion
	 * nueva caia justo encima de la que se acababa de refrescar. Un anillo
	 * al reves: lo mas reciente era siempre lo primero en morir. */
	for (i = 0; i < CAT_DESC_N; i++)
		if (desc_idx[i] == idx) break;

	if (i == CAT_DESC_N) {
		/* La ranura que se esta MIRANDO no se toca. Con 24 huecos y lotes
		 * de 16, un solo lote se lleva dos tercios del anillo por delante,
		 * y si se lleva la de la ficha abierta el texto desaparece, el
		 * desplazamiento vuelve al principio, y catWantDesc gasta otra
		 * peticion entera en recuperar lo que ya teniamos. */
		for (tries = 0; tries < CAT_DESC_N; tries++) {
			i = desc_next;
			desc_next = (desc_next + 1) % CAT_DESC_N;

			/* La comparacion suelta -desc_idx[i] != desc_pin- parecia
			 * bastar y no bastaba: una ranura VACIA vale ART_NONE, y sin
			 * ficha abierta desc_pin tambien, asi que se saltaba TODAS las
			 * ranuras libres. Con el anillo recien inicializado la primera
			 * descripcion acababa en la ultima ranura y a partir de ahi el
			 * reparto era un disparate. Lo pillo t/ring.c a la primera. */
			if (desc_pin == ART_NONE || desc_idx[i] != desc_pin) break;
		}
	}

	/* El indice se publica DESPUES del texto, igual que ready en las
	 * caratulas: en medio hay un snprintf de hasta 4 KB, y el hilo de
	 * dibujo que casara el indice antes de tiempo leeria la descripcion
	 * anterior, o la mitad de cada una. */
	desc_idx[i] = ART_NONE;
	snprintf(desc_buf[i], CAT_DESC_MAX, "%s", txt);
	desc_idx[i] = idx;

	/* Y al disco. Esta es la UNICA puerta de entrada al anillo, asi que
	 * poniendolo aqui se guardan tanto las que llegan en un lote como la
	 * que se pide suelta al abrir una ficha, sin acordarse en dos sitios.
	 *
	 * blob_write no escribe si ya esta, asi que llamarlo de mas es gratis
	 * salvo un recorrido del indice. */
	if (titles && idx < titles_n)
		blob_write(&descc, titles[idx].product_id, desc_buf[i],
		           (u32)strlen(desc_buf[i]));
}

/* TODAS las caches de aqui indexan por posicion en titles[], y la tabla se
 * reescribe entera cada vez que sube `generation`. Un indice de la tabla
 * vieja apunta a otro juego en la nueva.
 *
 * Y esto pasa en CADA arranque, no en un caso raro: se lee el catalogo del
 * disco (generacion 1), la rejilla ya pinta caratulas, y unos segundos
 * despues llega la respuesta de la red (generacion 2). Basta con que
 * Microsoft haya quitado un titulo para que todo lo que va detras se corra
 * una posicion.
 *
 * El sintoma no era un fallo, era una pantalla coherente y equivocada: la
 * caratula de otro juego bajo el nombre correcto. Y la descripcion no se
 * arreglaba sola nunca, porque catWantDesc ve que hay una y no pide otra.
 * O sea, el fallo de James Bond otra vez, por una puerta que aquel arreglo
 * no cubria.
 *
 * Se llama desde el hilo del catalogo, que es el mismo que rellena las
 * ranuras, asi que no puede pillar a art_pump a medias. */
static void cache_reset(void)
{
	u32 i;

	if (art) {
		for (i = 0; i < CAT_ART_SLOTS; i++) {
			art[i].title = ART_NONE;
			art[i].ready = 0;
			art[i].stamp = 0;
		}
	}

	big_idx   = ART_NONE;
	big_ready = 0;
	big_want  = ART_NONE;

	for (i = 0; i < CAT_DESC_N; i++) {
		desc_idx[i] = ART_NONE;
		desc_buf[i][0] = '\0';
	}
	desc_next  = 0;
	desc_want  = ART_NONE;
	desc_tried = ART_NONE;
	desc_pin   = ART_NONE;

}
