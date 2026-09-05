/* GR33N - sesion de juego en xCloud.
 *
 * Ver session.h para el porque. Resumen: esto todavia no reproduce nada,
 * pide una maquina y cuenta lo que contesta el servidor.
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
#include "net_tls.h"
#include "auth.h"
#include "catalog.h"
#include "i18n.h"   /* xclocActual(): el idioma que ha elegido el usuario */
#include "session.h"
#include "webrtc.h"
#include "aud.h"
#include "teredo.h"
#include "decoder.h"
#include "cJSON.h"

#define SES_JSON_CT  "application/json"

/* LO QUE NO SALE EN EL LOG.
 *
 * Volcar la respuesta entera sin interpretarla es el metodo que ha
 * resuelto el catalogo, la tienda y la sesion, y no se toca. Pero el
 * cuerpo del /connect lleva el token MSA de traspaso y /configuration
 * lleva la clave SRTP, y estos logs se mandan por ahi. De esos dos solo
 * hacia falta saber que llegaron y cuanto median.
 *
 * Con la comilla de delante para que no casen dentro de otra clave:
 * "monkey" acaba en "key" y "userTokenType" empieza por "userToken". */
static const char *const SES_SECRETO[] = {
	"\"userToken", "\"key", "\"access_token", "\"gsToken"
};
#define SES_SECRETO_N  (sizeof(SES_SECRETO) / sizeof(SES_SECRETO[0]))

/* Las respuestas de este servicio son JSON corto: una ruta, un estado, y
 * la configuracion de conexion. 128 KB sobra con holgura y no compite con
 * el megabyte largo del catalogo.
 *
 * Es un estatico y no un malloc a proposito: este hilo puede estar
 * pidiendo cada segundo durante minutos si hay cola, y reservar y soltar
 * 128 KB sesenta veces seguidas es fragmentar el monton por gusto. */
static char sink[128 * 1024];

/* Cuanto se espera entre sondeos del estado. Un segundo: el servidor no va
 * a provisionar mas rapido porque preguntemos mas, y cada pregunta es una
 * negociacion TLS entera que le quita el turno al catalogo. */
#define SES_POLL_US   (1000 * 1000)

/* Techo de espera. Cinco minutos es generoso para provisionar y sigue
 * siendo un techo: si se pasa, se dice y se suelta la maquina en vez de
 * esperar para siempre. */
#define SES_MAX_WAIT_S  300

/* La cola es otra cosa. En hora punta xCloud puede tener a alguien media
 * hora esperando maquina, y ahi rendirse a los cinco minutos es soltar un
 * puesto que ya estaba ganado. green-nx usa el mismo orden de magnitud. */
#define SES_MAX_QUEUE_S  1800

#define SES_THREAD_PRIO   1005
#define SES_THREAD_STACK  (192 * 1024)

/* --------------------------------------------------------------------- */

static sesInfo info;

static sys_ppu_thread_t ses_tid;
static volatile int ses_running = 0;
static int  ses_started = 0;
static volatile int start_req = 0;
static volatile int stop_req  = 0;

static char want_title[64];

/* LA FOTO DE LA REGION.
 *
 * Se rellena al empezar una sesion y se borra al soltarla. Mientras tenga
 * algo, TODAS las peticiones de esta sesion --incluido el DELETE final--
 * van contra esta direccion y no contra la que diga el ajuste en ese
 * momento. Ver el comentario largo en ses_call(). */
static char ses_base[192];

/* EL TECHO DE VERDAD LO PONE EL TRANSPORTE, no este buffer.
 *
 * net_tls monta la peticion entera -linea, cabeceras y cuerpo- en 16 KB, y
 * de ahi ya se descuenta la cabecera de autorizacion, que tiene hueco para
 * un gsToken de 8 KB. Lo que queda para el cuerpo son unos 7 KB largos.
 * Declarar 16 KB aqui seria prometer un tamaño que la capa de abajo va a
 * rechazar, y el mensaje de error saldria del sitio equivocado. */
#define SES_UTOK_MAX  7168

/* El token de traspaso, y si ya lo tenemos.
 *
 * ESTA FUERA DE step_connect A PROPOSITO. Estaba dentro, y como el paso se
 * reintenta cada 200 ms mientras el modulo TLS este ocupado, cada reintento
 * volvia a pedirle uno nuevo a login.live.com. Un bucle de concesiones de
 * refresh_token contra un servicio de Microsoft con limite de peticiones es
 * una forma estupenda de que te lo capen.
 *
 * Se limpia al empezar cada sesion, en run_session_inner(). */
static char utok[SES_UTOK_MAX];
static int  utok_listo = 0;

static void slog_(const char *fmt, ...)
{
	char buf[224];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	printf("[ses] %s\n", buf);
	linkLog("[ses] %s", buf);
}

static void sfail(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(info.err, sizeof(info.err), fmt, ap);
	va_end(ap);

	info.state = SES_FAILED;

	printf("[ses] FALLO: %s\n", info.err);
	linkLog("[ses] FALLO: %s", info.err);
}

static u64 now_us(void)
{
	u64 sec = 0, nsec = 0;
	sysGetCurrentTime(&sec, &nsec);
	return sec * 1000000ull + nsec / 1000ull;
}

/* --------------------------------------------------------------------- */
/* Buscar campos sin saber como se llaman exactamente                    */
/* --------------------------------------------------------------------- */

static const cJSON *pick(const cJSON *o, const char *const *names, u32 n)
{
	u32 i;

	if (!o) return NULL;

	for (i = 0; i < n; i++) {
		const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, names[i]);
		if (v) return v;
	}

	return NULL;
}

#define PICK(o, ...) \
	({ static const char *const _n[] = { __VA_ARGS__ }; \
	   pick((o), _n, (u32)(sizeof(_n)/sizeof(_n[0]))); })

/* Lo que hay dentro, sin interpretarlo.
 *
 * Es la funcion que resolvio el catalogo y la tienda en una ejecucion cada
 * una. Cuando no sabes la forma de algo, el codigo que escribes primero es
 * el que te la ensena; y una lista de claves de primer nivel cabe en una
 * linea de log y vale mas que tres hipotesis. */
static void log_shape(const char *what, const cJSON *root, const char *raw)
{
	char keys[224];
	int n = 0;
	const cJSON *it;

	keys[0] = '\0';

	if (cJSON_IsObject(root)) {
		cJSON_ArrayForEach(it, root) {
			int k;
			if (!it->string) continue;
			k = snprintf(keys + n, sizeof(keys) - (size_t)n, "%s%s",
			             n ? ", " : "", it->string);
			if (k < 0 || n + k >= (int)sizeof(keys)) break;
			n += k;
		}
		slog_("%s: claves de primer nivel: %s", what,
		      keys[0] ? keys : "(ninguna)");
	} else if (cJSON_IsArray(root)) {
		slog_("%s: es un ARRAY de %d", what, cJSON_GetArraySize(root));
	} else {
		slog_("%s: ni objeto ni array", what);
	}

	/* Y el cuerpo en bruto, a trozos. La primera vez que se pide algo
	 * esto es lo unico que no miente. */
	if (raw) {
		/* SE TAPA ANTES DE TROCEAR, no despues.
		 *
		 * Si se troceara primero, un secreto partido entre dos trozos no
		 * casaria con ninguna clave y saldria entero, repartido en dos
		 * lineas. Que es exactamente igual de publicado. */
		static char seguro[4096];
		u32 len, off = 0;

		linkRedact(seguro, sizeof(seguro), raw, SES_SECRETO, SES_SECRETO_N);
		len = (u32)strlen(seguro);

		slog_("--- %s, tal cual viene (%u bytes) ---", what,
		      (unsigned)strlen(raw));
		while (off < len && off < 2048) {
			slog_("  %.180s", seguro + off);
			off += 180;
		}
		if (len > 2048) slog_("  ... (%u bytes mas)", (unsigned)(len - 2048));
	}
}

/* Copia una cadena de un campo JSON, fallando si no cabe.
 *
 * Devuelve 0, o -1 si no habia campo, o -2 si no cabe. El -2 importa: un
 * sessionPath recortado es una peticion a otro sitio, y ya llevamos tres
 * cortes silenciosos en este proyecto. */
static int take_str(const cJSON *v, char *out, u32 max, const char *what)
{
	u32 len;

	if (!cJSON_IsString(v) || !v->valuestring) {
		slog_("!! falta %s", what);
		return -1;
	}

	len = (u32)strlen(v->valuestring);
	if (len >= max) {
		sfail("%s no cabe: %u bytes y el hueco son %u", what,
		      (unsigned)len, (unsigned)max);
		return -2;
	}

	memcpy(out, v->valuestring, len + 1);
	return 0;
}

/* --------------------------------------------------------------------- */
/* Una peticion al servidor de region                                    */
/* --------------------------------------------------------------------- */

/* Devuelve 0 y deja el JSON analizado en *root (hay que liberarlo), o
 * negativo. -2 significa "el modulo TLS esta ocupado, vuelve luego" y NO
 * es un fallo. */
static int ses_call(const char *method, const char *path, const char *body,
                    cJSON **root, tlsResult *res, const char *what)
{
	const xcloudInfo *xc = authXCloud();
	static char tok[8192];
	char host[128], full[320], hdr[9216];
	char base_path[128];
	const char *base;
	int r, n;

	*root = NULL;

	if (!xc || xc->state != XC_OK || !xc->base_uri[0]) {
		sfail("todavia no hay sesion de xCloud abierta");
		return -1;
	}

	/* LA REGION DE ESTA SESION ES LA QUE ERA AL EMPEZAR, NO LA DE AHORA.
	 *
	 * Con el selector de region cableado, xc->base_uri puede cambiar en
	 * mitad de una partida: basta con abrir Ajustes y darle a la derecha.
	 * Si cada peticion leyera el valor de ese momento, el /state, el
	 * /sdp y sobre todo el DELETE del final se irian a una region donde
	 * esta sesion no existe. El DELETE devolveria un 404 tan tranquilo y
	 * la maquina se quedaria reservada en la region de verdad hasta que
	 * caducara sola, gastando cuota de la cuenta y un servidor que otro
	 * podria estar usando.
	 *
	 * Asi que la region se fotografia al pedir la maquina y todas las
	 * peticiones de esa sesion van contra la foto. */
	base = ses_base[0] ? ses_base : xc->base_uri;

	if (authGsTokenCopy(tok, sizeof(tok)) == 0) {
		sfail("no hay gsToken");
		return -1;
	}

	/* base_uri viene entero (https://uks.core...), y hay que partirlo. */
	if (tlsSplitUrl(base, host, sizeof(host),
	                base_path, sizeof(base_path)) != 0) {
		sfail("no entiendo el baseUri: %.100s", base);
		return -1;
	}

	/* base_path suele ser "/" a secas; si trajera algo, se respeta.
	 *
	 * LA BARRA DE DELANTE SE PONE AQUI Y NO SE DA POR SUPUESTA.
	 *
	 * El servidor devuelve el sessionPath SIN barra inicial:
	 *
	 *   {"sessionPath":"v5/sessions/cloud/9F8AC269-...","queueConfig":"..."}
	 *
	 * y eso parece una ruta, asi que se pegaba tal cual detras del host. Lo
	 * que salia por el cable era:
	 *
	 *   GET v5/sessions/cloud/9F8AC269-.../state HTTP/1.1
	 *
	 * que no es una peticion valida - la linea de peticion pide una ruta
	 * absoluta - y el gateway de Azure contestaba un 400 en HTML sin decir
	 * por que. Un 400 que no nombra ningun campo porque el problema no
	 * estaba en ningun campo, estaba en la primera linea.
	 *
	 * Otra vez la respuesta plausible, y van diez: algo que se parece a lo
	 * que esperabas se usa sin comprobar que lo sea. Se normaliza en el
	 * unico sitio por el que pasan las tres peticiones (estado,
	 * configuracion y borrado) para que no dependa de que cada una se
	 * acuerde. */
	n = snprintf(full, sizeof(full), "%s%s%s",
	             (base_path[0] && strcmp(base_path, "/") != 0) ? base_path : "",
	             (path[0] == '/') ? "" : "/",
	             path);
	if (n < 0 || (size_t)n >= sizeof(full)) {
		sfail("la ruta no cabe: %.120s", path);
		return -1;
	}

	n = snprintf(hdr, sizeof(hdr),
	             "Authorization: Bearer %s\r\n"
	             "x-gssv-client: XboxComBrowser\r\n",
	             tok);
	if (n < 0 || (size_t)n >= sizeof(hdr)) {
		sfail("la cabecera de autorizacion no cabe");
		return -1;
	}

	/* Si el turno esta cogido, se vuelve EN SILENCIO.
	 *
	 * La primera version registraba el POST antes de intentarlo, y como
	 * el catalogo no soltaba el turno nunca, el log se lleno de cientos
	 * de "POST .../play" sin una sola peticion enviada. Un log que cuenta
	 * intenciones en vez de hechos es peor que no tener log: parece que
	 * algo pasa y no pasa nada. */
	if (tlsBusy()) return -2;

	slog_("%s %s%s", method, host, full);

	/* LO QUE SE MANDA, y solo cuando se manda de verdad.
	 *
	 * Esto estaba en step_play, justo antes de llamar aqui, y step_play se
	 * reintenta cada 200 ms hasta conseguir turno: el log salio con quince
	 * copias del mismo cuerpo y una sola peticion. Exactamente el mismo
	 * error que dice el comentario de aqui arriba, cometido un nivel mas
	 * arriba tres dias despues de escribirlo. */
	if (body) {
		char seguro[256];
		linkRedact(seguro, sizeof(seguro), body, SES_SECRETO, SES_SECRETO_N);
		slog_("cuerpo: %.180s", seguro);
	}

	r = tlsRequestTo(host, full, method, body ? SES_JSON_CT : NULL, body,
	                 hdr, sink, sizeof(sink), res);

	if (r == -2) return -2;   /* ocupado: no es fallo */

	if (r != 0) {
		sfail("%s: sin respuesta de %s: %s", what, host, res->err);
		return -1;
	}

	info.http_status = res->http_status;

	if (!res->body || !res->len) {
		/* Un DELETE que sale bien puede no traer cuerpo, y eso esta
		 * perfecto. Se distingue por el codigo, no por el vacio. */
		return 0;
	}

	*root = cJSON_Parse(res->body);
	return 0;
}

/* --------------------------------------------------------------------- */
/* Paso 0: recoger lo que dejamos tirado la ultima vez                   */
/* --------------------------------------------------------------------- */

/* UNA SESION ACTIVA POR CUENTA. Si queda una colgada -porque se fue la luz,
 * porque la aplicacion se salio al XMB, porque un DELETE se comio un fallo
 * de red-, TODOS los /play siguientes fallan, y el mensaje que devuelve el
 * servidor no dice "tienes una abierta": dice algo mucho mas opaco.
 *
 * Es exactamente la clase de fallo que quema un ciclo entero de compilar,
 * copiar a la consola y probar, buscando en el sitio equivocado. Media
 * pagina de codigo aqui lo evita.
 *
 * No devuelve nada a proposito: si esto falla, la sesion nueva se intenta
 * igual. Es limpieza, no un requisito. */
static void step_reap_inner(void)
{
	tlsResult res;
	cJSON *root = NULL;
	char rutas[4][192];
	u32 n = 0, i;
	int r, tries = 0;

	for (r = -2; r == -2; ) {
		if (!ses_running || stop_req) return;

		r = ses_call("GET", "/v5/sessions/cloud/active", NULL, &root, &res,
		             "sesiones abiertas");

		if (r == -2) {
			if (++tries > 25) return;   /* cinco segundos y a lo nuestro */
			usleep(200000);
		}
	}

	if (r != 0) return;

	/* EN LA NUBE ESTO CONTESTA 404, y conviene tenerlo escrito.
	 *
	 * La lista de sesiones abiertas la usa green-nx y funciona en su caso;
	 * contra el camino de nube de esta cuenta devuelve 404 con cuerpo
	 * vacio. O sea que el barrido no barre nada y hay que confiar en el
	 * DELETE de arriba y en el propio servidor, que segun /configuration
	 * tira la sesion sola a los timeoutForNoConnectionSeconds.
	 *
	 * Se deja puesto porque cuesta una peticion y el dia que exista nos
	 * ahorra un problema, pero se dice lo que contesta en vez de tragarlo:
	 * un 404 silencioso aqui parece "no habia nada colgando". */
	if (res.http_status != 200) {
		slog_("la lista de sesiones abiertas contesta HTTP %d; no barro nada",
		      res.http_status);
		if (root) cJSON_Delete(root);
		return;
	}

	/* La forma de esta respuesta tampoco la hemos visto. Se recogen las
	 * rutas vengan en un array suelto o dentro de un objeto, y si no
	 * cuadra ninguna de las dos se vuelca para saber por que. */
	if (root) {
		const cJSON *lista = cJSON_IsArray(root) ? root
		                   : PICK(root, "sessions", "Sessions", "results");

		if (cJSON_IsArray(lista)) {
			const cJSON *it;

			cJSON_ArrayForEach(it, lista) {
				const cJSON *p = cJSON_IsString(it) ? it
				               : PICK(it, "sessionPath", "SessionPath", "path");

				if (!cJSON_IsString(p) || !p->valuestring) continue;
				if (strlen(p->valuestring) >= sizeof(rutas[0])) continue;
				if (n >= 4) break;

				snprintf(rutas[n], sizeof(rutas[n]), "%s", p->valuestring);
				n++;
			}
		} else if (res.body && res.len > 2) {
			log_shape("sesiones abiertas", root, res.body);
		}

		cJSON_Delete(root);
	}

	if (!n) return;

	slog_("habia %u sesion(es) colgando de antes; soltandolas", (unsigned)n);

	for (i = 0; i < n; i++) {
		cJSON *r2 = NULL;
		int rc;
		int t2 = 0;

		for (rc = -2; rc == -2; ) {
			if (!ses_running || stop_req) return;
			rc = ses_call("DELETE", rutas[i], NULL, &r2, &res, "soltar vieja");
			if (rc == -2) { if (++t2 > 25) break; usleep(200000); }
		}

		if (r2) cJSON_Delete(r2);

		if (rc == 0 && res.http_status >= 200 && res.http_status < 300)
			slog_("  soltada %s", rutas[i]);
		else
			slog_("  !! no se pudo soltar %s (HTTP %d)", rutas[i],
			      rc == 0 ? res.http_status : 0);
	}
}

/* LA LIMPIEZA NO PUEDE MARCAR LA SESION COMO FALLIDA.
 *
 * ses_call() llama a sfail() cuando algo va mal, y sfail() pone
 * SES_FAILED. Eso esta bien para los pasos de verdad y esta mal aqui: esto
 * es trabajo de antes de empezar, y que un DELETE de una sesion vieja
 * pinte la pantalla de rojo antes de haber pedido nada seria mentir sobre
 * lo que esta pasando.
 *
 * Mismo patron que tlsHold en run_session, y por el mismo motivo: lo
 * garantiza la estructura y no la disciplina de acordarse en cada return. */
static void step_reap(void)
{
	sesState prev = info.state;
	int prev_http = info.http_status;
	char prev_err[sizeof(info.err)];

	memcpy(prev_err, info.err, sizeof(prev_err));

	step_reap_inner();

	info.state       = prev;
	info.http_status = prev_http;
	memcpy(info.err, prev_err, sizeof(info.err));
}

/* --------------------------------------------------------------------- */
/* Paso 1: pedir la maquina                                              */
/* --------------------------------------------------------------------- */

/* El cuerpo de la peticion, corregido POR EL SERVIDOR.
 *
 * La primera version llevaba "systemUpdateGroup":"5", copiado de los
 * clientes web, y el gateway contesto esto:
 *
 *   {"code":"InvalidRequestedSystemUpdateGroup","statusCode":400,
 *    "message":"Offering does not allow system update group selection"}
 *
 * O sea: la oferta xgpuweb no deja elegir grupo de actualizacion, y el
 * campo sobra. Fuera. Y de paso fuera "sdkType" y "osName", que no venian
 * de ningun cliente real - los puse yo por parecerme razonables, que es la
 * peor razon que hay para mandarle un campo a un servicio ajeno.
 *
 * Lo que queda es lo que se conoce de los clientes que hablan con este
 * endpoint. Si sobra o falta alguno mas, el 400 lo dira con nombre y
 * apellidos, igual que este.
 *
 * `serverId` vacio = que elija Microsoft. Cuando el ajuste de region este
 * cableado de verdad, sale de ahi. */
static int build_play_body(char *out, u32 max, const char *title_id)
{
	int n = snprintf(out, max,
		"{"
		"\"titleId\":\"%s\","
		"\"clientSessionId\":\"\","
		"\"settings\":{"
			"\"nanoVersion\":\"V3;WebrtcTransport.dll\","
			"\"enableTextToSpeech\":false,"
			"\"highContrast\":0,"
			/* EL IDIOMA CON EL QUE ARRANCA EL JUEGO.
			 *
			 * Estaba clavado a es-ES desde el primer dia. Ahora sale del
			 * ajuste, y va aqui y en la peticion a la tienda -- las dos
			 * del mismo sitio, para que la biblioteca y el juego no
			 * puedan acabar en idiomas distintos. */
			"\"locale\":\"%s\","
			"\"useIceConnection\":false,"
			"\"timezoneOffsetMinutes\":0"
		"},"
		"\"serverId\":\"\""
		"}",
		title_id, xclocActual());

	return (n > 0 && (u32)n < max) ? 0 : -1;
}

static int step_play(void)
{
	static char body[512];
	tlsResult res;
	cJSON *root = NULL;
	int r;

	info.state = SES_ASKING;

	if (build_play_body(body, sizeof(body), info.title_id) != 0) {
		sfail("el cuerpo de la peticion no cabe");
		return -1;
	}

	r = ses_call("POST", "/v5/sessions/cloud/play", body, &root, &res,
	             "pedir sesion");
	if (r == -2) return -2;
	if (r != 0)  return -1;

	/* LA PRIMERA VEZ SE ENSENA ENTERA, pase lo que pase. Es la unica
	 * peticion de todo el proyecto cuya forma no hemos visto nunca. */
	log_shape("sesion", root, res.body);

	/* 401 y 403 son LA respuesta a la pregunta de SISU, no un error
	 * cualquiera. Se dicen con todas las letras porque cambian el plan
	 * de trabajo de las proximas semanas. */
	if (res.http_status == 401 || res.http_status == 403) {
		sfail("HTTP %d al pedir sesion: la cadena simple NO basta aqui, "
		      "el gateway pide SISU (token de dispositivo y firma P-256)",
		      res.http_status);
		if (root) cJSON_Delete(root);
		return -1;
	}

	if (res.http_status != 200 && res.http_status != 201 &&
	    res.http_status != 202) {
		/* EL SERVIDOR YA HA DICHO QUE PASA. Repetirlo en pantalla vale
		 * mas que un numero: "InvalidRequestedSystemUpdateGroup" apunta
		 * al campo exacto, y "HTTP 400" no apunta a nada. */
		const cJSON *code = PICK(root, "code", "Code", "error");
		const cJSON *msg  = PICK(root, "message", "Message");

		sfail("HTTP %d: %s%s%s", res.http_status,
		      cJSON_IsString(code) ? code->valuestring : "(sin codigo)",
		      cJSON_IsString(msg) ? " - " : "",
		      cJSON_IsString(msg) ? msg->valuestring : "");

		if (root) cJSON_Delete(root);
		return -1;
	}

	if (!root) {
		sfail("la respuesta de la sesion no es JSON");
		return -1;
	}

	{
		const cJSON *p = PICK(root, "sessionPath", "SessionPath",
		                      "sessionpath", "path");

		if (take_str(p, info.path, sizeof(info.path), "sessionPath") != 0) {
			if (info.state != SES_FAILED)
				sfail("la respuesta no trae sessionPath");
			cJSON_Delete(root);
			return -1;
		}
	}

	cJSON_Delete(root);

	slog_("maquina pedida: %s", info.path);
	info.state = SES_PROVISIONING;
	return 0;
}

/* --------------------------------------------------------------------- */
/* Paso 2: decirle al servidor quien eres                                */
/* --------------------------------------------------------------------- */

/* EL PASO QUE FALTABA, y lo dijo el servidor con estas palabras:
 *
 *   {"code":"SessionNotActive","statusCode":410,
 *    "message":"Session 4A091940-... is not ready"}
 *
 * La maquina esta reservada y "ReadyToConnect", y aun asi la configuracion
 * contesta 410. Porque "ReadyToConnect" no describe la maquina: describe lo
 * que TE toca hacer. Hasta que no mandas el token de traspaso, la sesion
 * existe pero no es tuya.
 *
 * El token no es ninguno de los tres que ya teniamos. Ver authPassportToken
 * en auth.c.
 *
 * Devuelve 0, -1, o -2 si el modulo TLS esta ocupado. */
static int step_connect(void)
{
	/* Estatico y no en pila: el hilo tiene 192 KB. Solo lo toca este
	 * hilo, asi que no hay nada que proteger. */
	static char cbody[SES_UTOK_MAX + 64];

	char terr[224];
	tlsResult res;
	cJSON *root = NULL;
	int r, n;

	info.state = SES_CONNECTING;

	/* UNA SOLA VEZ POR SESION. Ver el comentario de utok_listo arriba. */
	if (!utok_listo) {
		r = authPassportToken(utok, sizeof(utok), sink, sizeof(sink),
		                      terr, sizeof(terr));
		if (r == -2) return -2;
		if (r != 0) {
			sfail("sin token de traspaso: %s", terr);
			return -1;
		}

		/* Un token MSA es base64url y no lleva comillas ni barras
		 * invertidas, asi que meterlo en el JSON a pelo vale. Pero "asi
		 * que" es una suposicion sobre datos ajenos, y de esas llevamos
		 * unas cuantas: si algun dia trae algo raro, mejor un mensaje con
		 * nombre que un 400 apuntando a la autenticacion. */
		if (strpbrk(utok, "\"\\\r\n")) {
			sfail("el token de traspaso trae caracteres que romperian el JSON");
			return -1;
		}

		utok_listo = 1;
	}

	n = snprintf(cbody, sizeof(cbody), "{\"userToken\":\"%s\"}", utok);
	if (n < 0 || (size_t)n >= sizeof(cbody)) {
		sfail("el cuerpo del connect no cabe: %u bytes", (unsigned)n);
		return -1;
	}

	{
		char path[256];
		snprintf(path, sizeof(path), "%s/connect", info.path);

		r = ses_call("POST", path, cbody, &root, &res, "autenticar la sesion");
	}

	if (r == -2) return -2;
	if (r != 0)  return -1;

	/* Otra respuesta que no hemos visto nunca. Los clientes de referencia
	 * miran el codigo y tiran el cuerpo; nosotros lo miramos una vez. */
	log_shape("connect", root, res.body);

	if (res.http_status < 200 || res.http_status >= 300) {
		const cJSON *code = PICK(root, "code", "Code", "error");
		const cJSON *msg  = PICK(root, "message", "Message");

		sfail("HTTP %d al autenticar la sesion: %s%s%s", res.http_status,
		      cJSON_IsString(code) ? code->valuestring : "(sin codigo)",
		      cJSON_IsString(msg) ? " - " : "",
		      cJSON_IsString(msg) ? msg->valuestring : "");

		if (root) cJSON_Delete(root);
		return -1;
	}

	if (root) cJSON_Delete(root);

	slog_("sesion autenticada; ahora deberia ponerse a provisionar");
	info.state = SES_PROVISIONING;
	return 0;
}

/* --------------------------------------------------------------------- */
/* Paso 3: esperar a que este lista                                      */
/* --------------------------------------------------------------------- */

/* Lo que devuelve step_state(). */
#define SES_ST_ESPERA    0   /* sigue trabajando, vuelve luego */
#define SES_ST_LISTA     1   /* Provisioned: ya esta           */
#define SES_ST_CONECTAR  2   /* ReadyToConnect: te toca a ti   */
#define SES_ST_MUERTA   (-1)

static int step_state(int *first)
{
	char path[256];
	tlsResult res;
	cJSON *root = NULL;
	int r, done = SES_ST_ESPERA;

	snprintf(path, sizeof(path), "%s/state", info.path);

	r = ses_call("GET", path, NULL, &root, &res, "estado de la sesion");
	if (r == -2) return SES_ST_ESPERA;   /* ocupado: se reintenta */
	if (r != 0)  return SES_ST_MUERTA;

	info.polls++;

	if (*first) { log_shape("estado", root, res.body); *first = 0; }

	if (res.http_status != 200) {
		sfail("HTTP %d al preguntar el estado", res.http_status);
		if (root) cJSON_Delete(root);
		return SES_ST_MUERTA;
	}

	if (!root) {
		sfail("el estado no es JSON");
		return SES_ST_MUERTA;
	}

	/* LA MAQUINA DE ESTADOS ENTERA, tal y como la contesta el servicio:
	 *
	 *   New -> WaitingForResources -> ReadyToConnect
	 *                                       |
	 *                              (POST /connect)
	 *                                       v
	 *                       Provisioning -> Provisioned      Failed
	 *
	 * "Provisioned" era mi apuesta original y resulta que existe. Lo que
	 * estaba mal no era el nombre: era el sitio. Yo lo esperaba antes del
	 * /connect, y solo llega despues.
	 *
	 * Se compara con lo que dice el servidor, sin normalizar: si manda un
	 * estado que no conocemos, se ve en pantalla en vez de convertirse en
	 * un "desconocido" que no ayuda a nadie. */
	{
		const cJSON *st = PICK(root, "state", "State", "sessionState");

		if (cJSON_IsString(st) && st->valuestring) {
			const char *s = st->valuestring;
			int nuevo = 0;

			if (strcmp(info.server_state, s) != 0) {
				snprintf(info.server_state, sizeof(info.server_state), "%s", s);
				slog_("estado: %s", info.server_state);
				nuevo = 1;
			}

			if (strcmp(s, "Provisioned") == 0) {
				done = SES_ST_LISTA;
			} else if (strcmp(s, "ReadyToConnect") == 0) {
				done = SES_ST_CONECTAR;
			} else if (strcmp(s, "Failed") == 0 ||
			           strcmp(s, "Terminated") == 0 ||
			           strcmp(s, "Canceled") == 0) {
				sfail("el servidor dice que la sesion ha fallado: \"%s\"", s);
				cJSON_Delete(root);
				return SES_ST_MUERTA;
			} else if (strstr(s, "Wait") || strstr(s, "Queue")) {
				info.state = SES_QUEUED;
			} else if (strcmp(s, "Provisioning") == 0 ||
			           strcmp(s, "New") == 0) {
				info.state = SES_PROVISIONING;
			} else {
				info.state = SES_PROVISIONING;

				/* UN ESTADO QUE NO CONOZCO SE DICE, no se sondea en
				 * silencio. Es literalmente lo que paso con
				 * "ReadyToConnect": el bucle giraba sin quejarse porque
				 * cualquier cosa inesperada cae en este else, y desde
				 * fuera "sigue provisionando" y "no se como se llama
				 * esto" tienen exactamente la misma pinta. */
				if (nuevo)
					slog_("!! \"%s\" no esta en la lista de estados que "
					      "conozco; sigo sondeando por si acaba en otro", s);
			}
		}
	}

	/* El servidor trae un hueco para explicarse. Casi siempre es null, y
	 * cuando no lo sea sera lo unico que importe de toda la respuesta. */
	{
		const cJSON *e = PICK(root, "errorDetails", "ErrorDetails", "error");

		if (e && !cJSON_IsNull(e)) {
			char *txt = cJSON_PrintUnformatted(e);
			slog_("!! errorDetails: %.180s", txt ? txt : "(no se puede leer)");
			if (txt) free(txt);
		}
	}

	/* La cola, si la hay. Los nombres son una apuesta; el volcado de
	 * arriba dira los de verdad en la primera ejecucion. */
	{
		const cJSON *w = PICK(root, "waitTime", "WaitTime", "queue");

		if (cJSON_IsObject(w)) {
			const cJSON *tot = PICK(w, "estimatedTotalWaitTimeInSeconds",
			                        "estimatedProvisioningTimeInSeconds",
			                        "waitTimeInSeconds");
			const cJSON *pos = PICK(w, "position", "queuePosition");

			if (cJSON_IsNumber(tot)) info.wait_s   = (u32)tot->valuedouble;
			if (cJSON_IsNumber(pos)) info.queue_pos = (int)pos->valuedouble;
		}
	}

	cJSON_Delete(root);
	return done;
}

/* --------------------------------------------------------------------- */
/* Paso 4: con quien se habla                                            */
/* --------------------------------------------------------------------- */

/* NO ES OBLIGATORIA, y por eso ya no mata la sesion.
 *
 * Los clientes de referencia que funcionan -greenlight, xbox-xcloud-player,
 * green-nx- ni siquiera la piden en el camino de nube: el punto final del
 * video sale de la respuesta SDP y del goteo de candidatos ICE. La
 * configuracion mejora el atravesado de NAT y trae la clave SRTP, pero
 * rendirse porque no llegue seria tirar una maquina ya provisionada por un
 * dato opcional.
 *
 * Para nosotros, hoy, sigue siendo LA respuesta interesante: es la unica
 * peticion del proyecto cuyo contenido no hemos visto nunca. Asi que se
 * pide, se vuelca entera, y si falla se dice bien alto y se sigue. */
static int step_config(void)
{
	char path[256];
	tlsResult res;
	cJSON *root = NULL;
	int r;

	info.state = SES_CONFIG;

	snprintf(path, sizeof(path), "%s/configuration", info.path);

	r = ses_call("GET", path, NULL, &root, &res, "configuracion");
	if (r == -2) return -2;
	if (r != 0)  return -1;

	/* ENTERA, SIEMPRE, sin interpretar ni un campo: hoy no sabemos ni que
	 * buscar, y esta es la respuesta que decide cuanto WebRTC hay que
	 * portar de verdad. */
	log_shape("configuracion", root, res.body);

	if (res.http_status != 200) {
		const cJSON *code = PICK(root, "code", "Code", "error");
		const cJSON *msg  = PICK(root, "message", "Message");

		slog_("!! HTTP %d al pedir la configuracion: %s%s%s -- no es "
		      "obligatoria, la sesion sigue en pie", res.http_status,
		      cJSON_IsString(code) ? code->valuestring : "(sin codigo)",
		      cJSON_IsString(msg) ? " - " : "",
		      cJSON_IsString(msg) ? msg->valuestring : "");
	}

	/* Y LO POCO QUE YA SABEMOS LEER.
	 *
	 *   {"keepAlivePulseInSeconds":60,"timeoutForNoConnectionSeconds":300,
	 *    "serverDetails":{"ipAddress":"13.104.118.6","port":1063,...,
	 *                     "srtp":{"key":"..."},...},
	 *    "clientStreamingConfigOverrides":null}
	 *
	 * DOS AVISOS GRANDES, los dos aprendidos por las malas.
	 *
	 * 1. LA IP DE AQUI NO SIRVE PARA MANDAR NADA. Los 13.104.x son un
	 *    candidato de relleno que NO contesta a STUN. El bueno es una
	 *    direccion Teredo que llega goteando por {sessionPath}/ice unos
	 *    segundos despues, y hay que sondear hasta que aparezca. Mandar
	 *    RTP a lo que dice este campo es hablarle a una pared.
	 *
	 * 2. LA CLAVE SRTP DE AQUI NO LA USA NADIE. Yo di por hecho que
	 *    entregarla en claro por HTTPS significaba que no hacia falta
	 *    DTLS. Suena razonable y es falso: la oferta SDP real lleva
	 *    a=fingerprint y a=setup, sin una sola linea a=crypto, y ni
	 *    green-nx ni los clientes web leen este campo. Es vestigial.
	 *    Las claves salen del saludo DTLS como en cualquier WebRTC.
	 *
	 * Se guardan igual, porque saber que llegan sigue diciendo que la
	 * sesion esta viva. Pero no son el atajo que yo canté.
	 *
	 * El resto -ipV4List, rigPort, routingPreference- no se toca: no
	 * sabemos para que sirve y el volcado de arriba lo guarda entero. */
	if (root) {
		const cJSON *sd = PICK(root, "serverDetails", "ServerDetails");
		const cJSON *ka = PICK(root, "keepAlivePulseInSeconds",
		                       "KeepAlivePulseInSeconds");

		if (cJSON_IsNumber(ka)) info.keepalive_s = (u32)ka->valuedouble;

		if (cJSON_IsObject(sd)) {
			const cJSON *ip = PICK(sd, "ipV4Address", "ipAddress");
			const cJSON *pt = PICK(sd, "ipV4Port", "port");
			const cJSON *sr = PICK(sd, "srtp", "Srtp");

			if (cJSON_IsString(ip) && ip->valuestring)
				snprintf(info.server_ip, sizeof(info.server_ip), "%s",
				         ip->valuestring);

			/* El puerto llega como numero o como cadena segun la version
			 * del servicio; se aceptan las dos en vez de apostar. */
			if (cJSON_IsNumber(pt))      info.server_port = (u32)pt->valuedouble;
			else if (cJSON_IsString(pt)) info.server_port = (u32)atoi(pt->valuestring);

			if (cJSON_IsObject(sr)) {
				const cJSON *k = PICK(sr, "key", "Key");
				info.have_srtp = (cJSON_IsString(k) && k->valuestring &&
				                  k->valuestring[0]);
			}

			if (info.server_ip[0])
				slog_("candidato de relleno: %s:%u (NO contesta a STUN)  "
				      "clave SRTP: %s  keepalive: %u s",
				      info.server_ip, (unsigned)info.server_port,
				      info.have_srtp ? "llega, pero no se usa" : "no llega",
				      (unsigned)info.keepalive_s);
		}
	}

	if (root) cJSON_Delete(root);

	info.state = SES_READY;
	return 0;
}

/* --------------------------------------------------------------------- */
/* Soltar la maquina                                                     */
/* --------------------------------------------------------------------- */

/* SE LLAMA SIEMPRE que haya una ruta, incluso si algo fallo por el camino.
 *
 * Una sesion reserva una maquina en un centro de datos y va contra la
 * cuota de la cuenta. Dejarla colgando porque nuestro codigo se rindio a
 * mitad es quedarse sin poder jugar, y encima desde el otro lado no se
 * distingue de un cliente roto. */
static void step_delete(void)
{
	tlsResult res;
	cJSON *root = NULL;
	int i, resuelto = 0;

	if (!info.path[0]) return;

	/* PRIORIDAD PROPIA, y no la del que llame.
	 *
	 * Esto se llama desde dentro de una sesion -que ya la tiene- y tambien
	 * desde el hilo al parar, que NO la tiene. Por ese segundo camino, el
	 * DELETE competia de tu a tu con el barrido del catalogo, que encadena
	 * lotes de hasta siete segundos: tres intentos de 200 ms no ganaban
	 * jamas y la maquina se quedaba reservada. En el log de 0.28.0 salio
	 * tres veces seguidas.
	 *
	 * tlsHold cuenta en vez de conmutar, asi que pedirla aqui dentro de
	 * una sesion que ya la tiene no se la quita a nadie. */
	tlsHold(1);

	/* Cincuenta intentos, diez segundos. Un lote de tienda dura siete como
	 * mucho, asi que con esto se gana el turno seguro. Soltar la maquina
	 * merece esperar diez segundos; no soltarla se la deja a Microsoft
	 * hasta que caduca sola. */
	for (i = 0; i < 50; i++) {
		int r = ses_call("DELETE", info.path, NULL, &root, &res, "soltar");

		if (root) { cJSON_Delete(root); root = NULL; }

		if (r == -2) { usleep(200000); continue; }

		resuelto = 1;

		/* QUE HAYA CONTESTADO NO ES QUE LO HAYA SOLTADO.
		 *
		 * Esto decia "maquina soltada (HTTP 400)" y se quedaba tan ancho:
		 * r == 0 solo significa que hubo respuesta, y el 400 de la barra
		 * que faltaba salia en el log como un exito. Una linea que dice
		 * que la cuota esta libre cuando no lo esta es peor que no tener
		 * linea, porque nadie va a ir a mirar. */
		if (r == 0 && res.http_status >= 200 && res.http_status < 300)
			slog_("maquina soltada (HTTP %d)", res.http_status);
		else if (r == 0)
			slog_("!! el servidor NO ha soltado la maquina: HTTP %d. Sigue "
			      "ocupando cuota hasta que caduque sola", res.http_status);
		else
			slog_("!! no se pudo soltar la maquina: puede quedar "
			      "ocupando cuota hasta que caduque sola");
		break;
	}

	/* LA RUTA SOLO SE OLVIDA SI SE LLEGO A MANDAR ALGO.
	 *
	 * Esto la borraba siempre, incluidos los tres reintentos que salen por
	 * "el modulo TLS esta ocupado" sin haber enviado nada. La consecuencia
	 * era una maquina reservada y nadie que supiera su nombre para
	 * soltarla, que es justo lo que el comentario de arriba jura que no
	 * pasa. Si no se resolvio, se deja puesta: el proximo intento -o el
	 * barrido del arranque siguiente- la encontrara. */
	if (resuelto) {
		info.path[0] = '\0';
		/* Y la foto de la region con ella, JUNTAS. Mientras haya una
		 * ruta que soltar tiene que quedar la direccion donde soltarla:
		 * separarlas es dejar un nombre sin sitio a donde llevarlo. */
		ses_base[0] = '\0';
	} else {
		slog_("!! diez segundos sin turno de red para el DELETE; me quedo "
		      "la ruta apuntada");
	}

	tlsHold(0);
}

/* --------------------------------------------------------------------- */

/* Definida mas abajo, con el resto de la negociacion. */
static void run_webrtc(void);

static void run_session_inner(void)
{
	u64 t0 = now_us();
	int first_state = 1;
	int r;

	memset(&info, 0, sizeof(info));
	info.queue_pos = -1;
	snprintf(info.title_id, sizeof(info.title_id), "%s", want_title);

	utok_listo = 0;

	/* LA FOTO DE LA REGION, aqui y no en ningun otro sitio.
	 *
	 * A partir de esta linea la sesion tiene una direccion propia y el
	 * ajuste de region puede moverse todo lo que quiera sin llevarse por
	 * delante el DELETE del final. Ver ses_call(). */
	{
		const xcloudInfo *xc = authXCloud();
		snprintf(ses_base, sizeof(ses_base), "%s",
		         (xc && xc->base_uri[0]) ? xc->base_uri : "");
		if (ses_base[0]) slog_("region de esta sesion: %s", ses_base);
	}

	/* EL ESTADO SE PONE ANTES DEL BARRIDO, y no es cosmetico: sesStart()
	 * deja empezar otra sesion mientras esto sea SES_IDLE, y el barrido
	 * puede tardar varios segundos. Dos pulsaciones de X seguidas
	 * entraban aqui dos veces, y el memset de la segunda se llevaba por
	 * delante el info.path de la primera: maquina reservada, nombre
	 * perdido. */
	info.state = SES_ASKING;

	slog_("pidiendo sesion para \"%s\"", info.title_id);

	/* PRIORIDAD, de aqui hasta el final.
	 *
	 * Mientras dure, el barrido del catalogo y las descargas de caratulas
	 * se apartan solos. Sin esto no habia forma: el catalogo encadena
	 * lotes de dos segundos y reclama el turno en cuanto suelta el
	 * anterior, y este hilo pedia cada 200 ms sin conseguirlo jamas.
	 *
	 * Se paga con carátulas que tardan mas mientras se abre una sesion, y
	 * es un precio ridiculo: el usuario esta mirando la pantalla de un
	 * juego que quiere abrir, no la rejilla. */
	/* 0. Lo que quedara colgando de la vez anterior. */
	step_reap();

	/* 1. La maquina. Puede haber que esperar un turno: el hilo de sesion
	 *    de Xbox Live o un lote de tienda ya en vuelo. */
	{
		int tries = 0;

		for (r = -2; r == -2; ) {
			if (!ses_running || stop_req) return;

			r = step_play();

			if (r == -2) {
				/* TECHO, y con voz. Un bucle de reintentos mudo y sin
				 * final es como se llega a cientos de lineas en el log
				 * sin una peticion enviada. */
				if (++tries > 100) {
					sfail("veinte segundos esperando turno de red y no "
					      "llega; lo dejo");
					return;
				}
				if (tries == 25)
					slog_("esperando turno de red (%d intentos)...", tries);

				usleep(200000);
			}
		}
	}
	if (r != 0) return;

	/* 2 y 3. Esperar, y autenticarse cuando el servidor lo pida.
	 *
	 * El /connect va AQUI y no en un paso suelto porque quien decide cuando
	 * toca es el servidor, no nosotros: llega cuando /state dice
	 * "ReadyToConnect", que puede ser en el primer sondeo o despues de
	 * media hora de cola. */
	{
		u32 waited = 0;
		int conectado = 0;

		for (;;) {
			if (!ses_running || stop_req) {
				step_delete(); return;
			}

			r = step_state(&first_state);
			if (r == SES_ST_MUERTA) { step_delete(); return; }
			if (r == SES_ST_LISTA)  break;

			if (r == SES_ST_CONECTAR && !conectado) {
				int c, t = 0;

				/* CON TECHO. La version sin el podia girar para siempre:
				 * el hilo de autenticacion NO respeta tlsHold(), asi que
				 * si el usuario se mete en la pantalla de login mientras
				 * se abre una sesion, ses_call devuelve -2 sin parar y
				 * este bucle se queda dentro, sin contar para el tiempo
				 * de espera, con una maquina reservada y el catalogo
				 * apartado. */
				for (c = -2; c == -2; ) {
					if (!ses_running || stop_req) { step_delete(); return; }
					c = step_connect();
					if (c == -2) {
						if (++t > 150) {   /* treinta segundos */
							sfail("treinta segundos esperando turno de red "
							      "para autenticar la sesion; la suelto");
							step_delete();
							return;
						}
						usleep(200000);
					}
				}

				if (c != 0) { step_delete(); return; }

				/* UNA SOLA VEZ. El servidor puede tardar un sondeo o dos
				 * en salir de "ReadyToConnect", y mandarle el token otra
				 * vez mientras tanto es pedirle a Microsoft que se pregunte
				 * que clase de cliente tiene delante. */
				conectado = 1;
				waited = 0;
				continue;   /* sin dormir: el estado acaba de cambiar */
			}

			usleep(SES_POLL_US);
			waited++;

			/* La cola tiene su propio techo. xCloud puede tener a alguien
			 * media hora esperando maquina en hora punta, y rendirse a los
			 * cinco minutos seria soltar un puesto que ya estaba ganado. */
			{
				u32 techo = (info.state == SES_QUEUED) ? SES_MAX_QUEUE_S
				                                       : SES_MAX_WAIT_S;

				if (waited >= techo) {
					sfail("%u segundos esperando y sigue en \"%s\"; suelto "
					      "la maquina", (unsigned)techo, info.server_state);
					step_delete();
					return;
				}
			}
		}
	}

	slog_("PROVISIONADA tras %u sondeos", (unsigned)info.polls);

	/* 4. Con quien se habla. Con techo, por lo mismo que el /connect. */
	{
		int t = 0;

		for (r = -2; r == -2; ) {
			if (!ses_running || stop_req) { step_delete(); return; }
			r = step_config();
			if (r == -2) {
				if (++t > 150) {
					slog_("!! treinta segundos esperando turno de red para "
					      "la configuracion; sigo sin ella");
					r = 0;
					info.state = SES_READY;
					break;
				}
				usleep(200000);
			}
		}
	}

	if (r != 0) { step_delete(); return; }

	info.ms = (u32)((now_us() - t0) / 1000);

	slog_("SESION LISTA en %u ms.", (unsigned)info.ms);

	/* 5. Y ahora WebRTC, que es lo que convierte una maquina reservada en
	 *    una conexion. */
	run_webrtc();

	step_delete();
}


/* ===================================================================== */
/* 5. LA NEGOCIACION WebRTC                                              */
/* ===================================================================== */

/* Aqui es donde la sesion deja de ser una maquina reservada y pasa a ser
 * una conexion. Toda la forma de esto viene de green-nx, que la saco del
 * cliente web oficial, y trae tres avisos por escrito que costaron caros
 * alli. Se copian aqui para que no haya que redescubrirlos:
 *
 *   1. LA RESPUESTA SDP SE PASA VERBATIM. Reserializarla puede tocar los
 *      finales de linea, y un '\r' colado en el ice-ufrag hace que las
 *      comprobaciones STUN se firmen con la clave equivocada. El servidor
 *      las tira en silencio: la conexion no llega y no hay ningun error.
 *
 *   2. LOS CANDIDATOS DEL SERVIDOR SE RECOGEN ANTES de darle la respuesta
 *      a libpeer, porque libpeer arma los pares UNA sola vez, dentro de
 *      set_remote_description.
 *
 *   3. EL PRIMER CANDIDATO QUE MANDA xCLOUD ES RELLENO. Un 13.104.x con
 *      prioridad 100 que no contesta a STUN. El bueno es una direccion
 *      Teredo que gotea unos segundos despues. Conformarse con el primero
 *      es quedarse sin conexion.
 *
 * Y uno propio, del formato: `exchangeResponse` NO es un objeto, es una
 * CADENA que lleva JSON dentro. Hay que analizarla dos veces. */

/* El bloque de configuracion de la oferta, literal del cliente web.
 *
 * Son las versiones de canal que se negocian. Va como cadena y no montado
 * con cJSON porque es constante y porque asi se ve de un vistazo que es
 * exactamente lo que manda el cliente que funciona. */
#define SES_SDP_CONFIG \
	"{\"chatConfiguration\":{\"bytesPerSample\":2," \
	"\"expectedClipDurationMs\":20," \
	"\"format\":{\"codec\":\"opus\",\"container\":\"webm\"}," \
	"\"numChannels\":1,\"sampleFrequencyHz\":24000}," \
	"\"chat\":{\"minVersion\":1,\"maxVersion\":1}," \
	"\"control\":{\"minVersion\":1,\"maxVersion\":3}," \
	"\"input\":{\"minVersion\":1,\"maxVersion\":9}," \
	"\"message\":{\"minVersion\":1,\"maxVersion\":1}," \
	"\"reliableinput\":{\"minVersion\":9,\"maxVersion\":9}," \
	"\"unreliableinput\":{\"minVersion\":9,\"maxVersion\":9}}"

/* Lo que devuelve el servidor en `exchangeResponse`: una cadena con JSON.
 *
 * Devuelve el objeto ya analizado, o NULL. Quien llama lo libera.
 * `status` 204 quiere decir "todavia no hay nada", que no es un fallo. */
static cJSON *ses_exchange(const cJSON *root, int *status)
{
	const cJSON *st = PICK(root, "status", "Status");
	const cJSON *ex;

	if (status) *status = cJSON_IsNumber(st) ? (int)st->valuedouble : 0;

	ex = PICK(root, "exchangeResponse", "ExchangeResponse");
	if (!cJSON_IsString(ex) || ex->valuestring == NULL ||
	    ex->valuestring[0] == '\0')
		return NULL;

	return cJSON_Parse(ex->valuestring);
}

/* Manda la oferta. */
static int step_sdp_offer(const char *oferta)
{
	char path[256];
	char *cuerpo = NULL;
	char *sdp_json = NULL;
	cJSON *s, *root = NULL;
	tlsResult res;
	int r = -1;

	/* El SDP lleva \r\n a puñados, asi que se escapa con cJSON en vez de
	 * a mano: un escapador propio es justo la clase de codigo que parece
	 * bien y se come una comilla el dia que llegue una. */
	s = cJSON_CreateString(oferta);
	if (s == NULL) { sfail("sin memoria para la oferta"); return -1; }
	sdp_json = cJSON_PrintUnformatted(s);
	cJSON_Delete(s);
	if (sdp_json == NULL) { sfail("no se pudo escapar la oferta"); return -1; }

	{
		size_t n = strlen(sdp_json) + sizeof(SES_SDP_CONFIG) + 128;
		cuerpo = (char *)malloc(n);
		if (cuerpo == NULL) {
			free(sdp_json);
			sfail("sin memoria para el cuerpo del /sdp");
			return -1;
		}
		snprintf(cuerpo, n,
		         "{\"messageType\":\"offer\",\"sdp\":%s,"
		         "\"requestId\":\"1\",\"configuration\":%s}",
		         sdp_json, SES_SDP_CONFIG);
	}
	free(sdp_json);

	snprintf(path, sizeof(path), "%s/sdp", info.path);

	for (;;) {
		if (!ses_running || stop_req) { free(cuerpo); return -1; }
		r = ses_call("POST", path, cuerpo, &root, &res, "oferta SDP");
		if (r != -2) break;
		usleep(200000);
	}

	free(cuerpo);
	if (root) cJSON_Delete(root);

	if (r != 0) return -1;

	slog_("oferta enviada (HTTP %d)", res.http_status);
	return 0;
}

/* Sondea hasta que llegue la respuesta. Copia el SDP TAL CUAL en `fuera`.
 *
 * Los numeros son los de green-nx: 120 intentos de 500 ms, o sea un minuto.
 * Un servidor de Azure en hora punta tarda. */
static int step_sdp_answer(char *fuera, size_t fuera_n)
{
	char path[256];
	int intento;

	snprintf(path, sizeof(path), "%s/sdp", info.path);

	for (intento = 0; intento < 120; intento++) {
		cJSON *root = NULL, *ex;
		tlsResult res;
		int st = 0, r;

		if (!ses_running || stop_req) return -1;

		r = ses_call("GET", path, NULL, &root, &res, "respuesta SDP");
		if (r == -2) { usleep(200000); intento--; continue; }
		if (r != 0)  { if (root) cJSON_Delete(root); return -1; }

		ex = ses_exchange(root, &st);
		cJSON_Delete(root);

		if (st == 204 || ex == NULL) {
			if (ex) cJSON_Delete(ex);
			usleep(500000);
			continue;
		}

		{
			const cJSON *sdp = PICK(ex, "sdp", "Sdp");

			if (!cJSON_IsString(sdp) || sdp->valuestring == NULL) {
				slog_("!! la respuesta no trae campo sdp");
				cJSON_Delete(ex);
				usleep(500000);
				continue;
			}

			/* Que no quepa es un FALLO. Media respuesta SDP tiene forma
			 * de respuesta SDP y libpeer se la traga sin quejarse. */
			if (strlen(sdp->valuestring) >= fuera_n) {
				sfail("la respuesta SDP no cabe: %u bytes",
				      (unsigned)strlen(sdp->valuestring));
				cJSON_Delete(ex);
				return -1;
			}

			strcpy(fuera, sdp->valuestring);
			cJSON_Delete(ex);
			slog_("respuesta SDP recibida: %u bytes tras %d sondeos",
			      (unsigned)strlen(fuera), intento + 1);

			/* Y AL LOG, LINEA A LINEA.
			 *
			 * Faltaba, y se noto: cuando el saludo DTLS fallo con "no
			 * remote fingerprint" no habia forma de saber si la culpa
			 * era de la respuesta o de libpeer, porque la respuesta no
			 * se veia por ningun lado. Se parte por saltos de linea y
			 * no por bytes para que los atributos no salgan cortados.
			 *
			 * Ojo: esto es lo que se VUELCA. Lo que se le pasa a
			 * libpeer sigue siendo `fuera` tal cual, sin tocar un solo
			 * byte. */
			{
				const char *q = fuera;
				int nl = 0;

				slog_("--- respuesta SDP ---");
				while (*q && nl < 60) {
					char linea[184];
					const char *fin = strchr(q, '\n');
					size_t len = fin ? (size_t)(fin - q) : strlen(q);

					if (len > 0 && q[len - 1] == '\r') len--;
					if (len >= sizeof(linea)) len = sizeof(linea) - 1;
					memcpy(linea, q, len);
					linea[len] = '\0';
					linkLog("[ans] %s", linea);

					nl++;
					if (!fin) break;
					q = fin + 1;
				}
				slog_("--- fin de la respuesta (%d lineas) ---", nl);
			}
			return 0;
		}
	}

	sfail("un minuto esperando la respuesta SDP y no llega");
	return -1;
}


/* Nuestros candidatos, sacados de la oferta y mandados por /ice.
 *
 * Ya van dentro del SDP, pero el cliente oficial los manda ademas por
 * aqui y green-nx avisa de que la FORMA importa: si no es exactamente
 * esta, xCloud se guarda su candidato bueno y no contesta a las
 * comprobaciones. Cada elemento del array es un objeto JSON CONVERTIDO A
 * CADENA -- doble codificacion, no es un error de lectura-- y la lista
 * termina con una entrada "a=end-of-candidates". */
static int step_ice_post(const char *oferta)
{
	char path[256], ufrag[80];
	const char *p;
	cJSON *lista, *raiz, *root = NULL;
	char *cuerpo;
	tlsResult res;
	int n = 0, r;

	ufrag[0] = '\0';
	p = strstr(oferta, "a=ice-ufrag:");
	if (p) {
		size_t i = 0;
		p += 12;
		while (*p && *p != '\r' && *p != '\n' && i < sizeof(ufrag) - 1)
			ufrag[i++] = *p++;
		ufrag[i] = '\0';
	}
	if (ufrag[0] == '\0') {
		slog_("!! la oferta no trae ice-ufrag; no mando candidatos");
		return -1;
	}

	lista = cJSON_CreateArray();
	if (lista == NULL) return -1;

	for (p = oferta; p && *p; ) {
		const char *fin = strchr(p, '\n');
		char linea[256], cand[256];
		size_t len = fin ? (size_t)(fin - p) : strlen(p);

		if (len < sizeof(linea)) {
			memcpy(linea, p, len);
			linea[len] = '\0';

			if (candNormaliza(linea, cand, sizeof(cand))) {
				cJSON *o = cJSON_CreateObject();
				char *txt;

				cJSON_AddStringToObject(o, "candidate", cand);
				cJSON_AddStringToObject(o, "sdpMid", "0");
				cJSON_AddNumberToObject(o, "sdpMLineIndex", 0);
				cJSON_AddStringToObject(o, "usernameFragment", ufrag);

				txt = cJSON_PrintUnformatted(o);
				cJSON_Delete(o);
				if (txt) {
					cJSON_AddItemToArray(lista, cJSON_CreateString(txt));
					free(txt);
					n++;
				}
			}
		}

		if (!fin) break;
		p = fin + 1;
	}

	/* La marca de final, como hace el cliente web. */
	{
		cJSON *o = cJSON_CreateObject();
		char *txt;

		cJSON_AddStringToObject(o, "candidate", "a=end-of-candidates");
		cJSON_AddStringToObject(o, "sdpMid", "0");
		cJSON_AddNumberToObject(o, "sdpMLineIndex", 0);
		cJSON_AddStringToObject(o, "usernameFragment", ufrag);
		txt = cJSON_PrintUnformatted(o);
		cJSON_Delete(o);
		if (txt) {
			cJSON_AddItemToArray(lista, cJSON_CreateString(txt));
			free(txt);
		}
	}

	raiz = cJSON_CreateObject();
	cJSON_AddStringToObject(raiz, "messageType", "iceCandidate");
	cJSON_AddItemToObject(raiz, "candidate", lista);
	cuerpo = cJSON_PrintUnformatted(raiz);
	cJSON_Delete(raiz);

	if (cuerpo == NULL) { sfail("sin memoria para el /ice"); return -1; }

	snprintf(path, sizeof(path), "%s/ice", info.path);

	for (;;) {
		if (!ses_running || stop_req) { free(cuerpo); return -1; }
		r = ses_call("POST", path, cuerpo, &root, &res, "candidatos ICE");
		if (r != -2) break;
		usleep(200000);
	}

	free(cuerpo);
	if (root) cJSON_Delete(root);

	if (r != 0) return -1;

	slog_("mandados %d candidatos propios (ufrag %s)", n, ufrag);
	return 0;
}

/* Un candidato del servidor, ya normalizado, a libpeer.
 *
 * Si la direccion es IPv6 solo sirve cuando es Teredo, y entonces salen
 * DOS candidatos: el puerto que va dentro de la direccion y el 9002, que
 * es por donde xCloud tambien escucha. Los dos con typ host, porque para
 * nosotros ya son direcciones directas. */
static int ice_meter(const char *cand, int *reales)
{
	char dir[80], ipv4[16], nuevo[160];
	int puerto;

	if (!candDireccion(cand, dir, sizeof(dir), &puerto)) return 0;

	if (strchr(dir, ':') == NULL) {
		wrtcSesionCandidato(cand);
		slog_("  <- %s", cand);
		if (candPrioridad(cand) > 1000) (*reales)++;
		return 1;
	}

	if (!teredoDecode(dir, ipv4, sizeof(ipv4), &puerto)) {
		slog_("  (ipv6 que no es Teredo, se ignora: %.60s)", dir);
		return 0;
	}

	{
		static int fundacion = 20;
		int puertos[2], i, met = 0;

		puertos[0] = puerto;
		puertos[1] = 9002;

		for (i = 0; i < 2; i++) {
			snprintf(nuevo, sizeof(nuevo),
			         "candidate:%d 1 UDP 1 %s %d typ host",
			         fundacion++, ipv4, puertos[i]);
			wrtcSesionCandidato(nuevo);
			slog_("  <- (Teredo) %s", nuevo);
			met++;
		}
		(*reales)++;
		return met;
	}
}

/* Recoge los candidatos del servidor hasta que haya alguno bueno.
 *
 * NO basta con que llegue el primero: xCloud manda un 13.104.x de
 * prioridad 100 que no contesta a STUN, y el de verdad --el Teredo-- puede
 * tardar segundos. Quedarse con el relleno es esperar quince segundos a
 * una pared. */
static int step_ice_poll(void)
{
	char path[256];
	u64 limite = now_us() + 15000000ull;
	int total = 0, reales = 0, fin = 0, callados = 0;

	snprintf(path, sizeof(path), "%s/ice", info.path);

	while (!fin && now_us() < limite) {
		cJSON *root = NULL, *ex;
		tlsResult res;
		int st = 0, r, antes = total;

		if (!ses_running || stop_req) return -1;

		r = ses_call("GET", path, NULL, &root, &res, "sondeo ICE");
		if (r == -2) { usleep(200000); continue; }
		if (r != 0)  { if (root) cJSON_Delete(root); usleep(300000); continue; }

		/* LA RESPUESTA CRUDA AL LOG, antes de quedarnos con nada.
		 *
		 * La primera version solo registraba los candidatos que
		 * aceptaba y los que descartaba, y cuando xCloud mando algo que
		 * no esperabamos --una IPv6 nativa de Azure en vez de la Teredo
		 * que dice green-nx-- hubo que deducirlo del mensaje de
		 * descarte. Registrar la FUENTE y no lo que uno guarda es una
		 * de las normas viejas de este proyecto; aqui faltaba. */
		{
			const cJSON *raw = PICK(root, "exchangeResponse",
			                        "ExchangeResponse");
			if (cJSON_IsString(raw) && raw->valuestring &&
			    raw->valuestring[0])
				slog_("  /ice crudo: %.170s", raw->valuestring);
		}

		ex = ses_exchange(root, &st);
		cJSON_Delete(root);

		if (st != 204 && ex) {
			const cJSON *arr = cJSON_IsArray(ex) ? ex
			                 : PICK(ex, "candidates", "Candidates");
			const cJSON *it;

			cJSON_ArrayForEach(it, arr) {
				const cJSON *obj = it;
				cJSON *suelto = NULL;
				const cJSON *c;
				char cand[256];

				/* Los elementos pueden venir como objeto o como cadena
				 * con un objeto dentro. Las dos formas se han visto. */
				if (cJSON_IsString(it) && it->valuestring) {
					suelto = cJSON_Parse(it->valuestring);
					if (suelto) obj = suelto;
				}

				c = cJSON_IsObject(obj) ? PICK(obj, "candidate", "Candidate")
				                        : obj;

				if (cJSON_IsString(c) && c->valuestring) {
					if (strstr(c->valuestring, "end-of-candidates"))
						fin = 1;
					else if (candNormaliza(c->valuestring, cand, sizeof(cand)))
						total += ice_meter(cand, &reales);
				}

				if (suelto) cJSON_Delete(suelto);
			}
		}
		if (ex) cJSON_Delete(ex);

		callados = (total == antes) ? callados + 1 : 0;

		/* Sin marca de final pero han dejado de llegar: se da por
		 * completo -- pero NUNCA mientras lo unico que haya sea el
		 * relleno. */
		if (reales > 0 && callados >= 4) break;
		if (!fin) usleep(300000);
	}

	slog_("candidatos del servidor: %d en total, %d utiles%s",
	      total, reales, fin ? " (con marca de final)" : "");

	/* SIN CANDIDATO BUENO SE INTENTA IGUAL, y se dice.
	 *
	 * La primera version se rendia aqui. Y rendirse cuesta la sesion
	 * entera: hay que volver a la cola de xCloud para averiguar lo
	 * siguiente. Si hay AL MENOS una direccion IPv4 a la que mandar,
	 * mejor gastar quince segundos de comprobaciones y que el log diga
	 * que paso, que quedarse sin datos.
	 *
	 * Lo que no se puede es seguir sin ninguna: ahi no hay a quien
	 * llamar y ICE no tendria ni un par que probar. */
	if (total == 0) {
		sfail("el servidor no ha dado ni una direccion IPv4");
		return -1;
	}

	if (reales == 0)
		slog_("!! solo hay relleno (prioridad 100). green-nx dice que "
		      "esa no contesta a STUN. Se prueba igual para ver que "
		      "hace ICE, pero lo normal es que no conecte.");

	return 0;
}


/* La negociacion entera, en el orden que impone libpeer.
 *
 * Se llama con la sesion ya provisionada y /configuration pedida. A
 * partir de aqui GR33N deja de hablar por HTTPS salvo para el keepalive:
 * lo que queda lo bombea el hilo de webrtc.c. */
static void run_webrtc(void)
{
	static char answer[16 * 1024];
	const char *oferta;

	slog_("--- WebRTC: negociando ---");

	/* El decodificador se abre AHORA, no cuando llegue el primer
	 * fotograma. vdecOpen tarda, y arrancarlo con video ya entrando
	 * significa tirar las primeras unidades -- justo las que traen el
	 * IDR. Que espere el es gratis; que espere el video, no. */
	if (decStartLive() != 0)
		slog_("!! el decodificador no arranca; se negocia igual y se vera "
		      "en el log si llega video");

	/* Y el audio, por lo mismo y aun mas: el puerto de la PS3 empieza a
	 * consumir bloques en cuanto se abre, asi que abrirlo tarde no es
	 * "empezar tarde", es empezar con el buffer del sistema lleno de lo
	 * que hubiera antes. Que suena a zumbido.
	 *
	 * Si falla, se sigue: un stream mudo es mucho mejor que ningun
	 * stream, y el motivo queda en el log. */
	if (audStart() != 0)
		slog_("!! el audio no arranca; se juega igual, en silencio");

	/* EL SERVIDOR STUN, ANTES DE CREAR NADA.
	 *
	 * Se me olvido en la primera version y el resultado fue una oferta
	 * con UN solo candidato: "192.168.1.150 typ host". Una direccion de
	 * LAN, o sea nada a lo que un servidor de Azure pueda contestar. Sin
	 * candidato srflx no le estamos dando a xCloud ninguna forma de
	 * llegar hasta aqui.
	 *
	 * Este es literalmente el que usa green-nx (engine.cpp:983).
	 * /configuration NO trae servidores ICE -- se ha mirado el volcado
	 * entero: keepAlivePulseInSeconds, timeoutForNoConnectionSeconds,
	 * serverDetails y clientStreamingConfigOverrides, y nada mas-- asi
	 * que no hay unos "de xCloud" que usar en su lugar. */
	wrtcSesionStun("stun:stun.l.google.com:19302");

	if (wrtcSesionCrear() != 0) {
		sfail("no se pudo crear la conexion WebRTC");
		goto recoger;   /* el decodificador ya esta abierto: hay que cerrarlo */
	}

	oferta = wrtcSesionOferta();
	if (oferta == NULL) {
		sfail("libpeer no ha dado oferta");
		goto recoger;
	}

	if (step_sdp_offer(oferta) != 0)                 goto recoger;

	/* Los nuestros van despues de la oferta y antes de sondear los suyos:
	 * el servidor no empieza a soltar los buenos hasta que sabe a donde
	 * contestarnos. */
	(void)step_ice_post(oferta);

	if (step_sdp_answer(answer, sizeof(answer)) != 0) goto recoger;

	/* PRIMERO los candidatos, DESPUES la respuesta. libpeer arma los pares
	 * dentro de set_remote_description y no vuelve a mirarlos. */
	if (step_ice_poll() != 0)                        goto recoger;

	if (wrtcSesionRespuesta(answer) != 0)            goto recoger;

	wrtcSesionBombear(1);
	slog_("--- WebRTC: bombeando, esperando a que ICE conecte ---");

	/* Y a partir de aqui este hilo solo hace de latido.
	 *
	 * El keepalive es un HTTPS bloqueante de hasta quince segundos y NO
	 * puede correr en el hilo que bombea los sockets: green-nx midio que
	 * pararlo desborda la cola de recepcion UDP, y eso se ve como un
	 * tiron de video y un PLI cada quince segundos clavados. Aqui no hay
	 * problema porque el bombeo esta en el hilo de webrtc.c y este solo
	 * espera. */
	{
		u32 ka = info.keepalive_s ? info.keepalive_s : 15;
		u32 t = 0;
		int visto = -2;

		while (ses_running && !stop_req) {
			int e = wrtcSesionIce();

			if (e != visto) {
				visto = e;
				slog_("ICE: %s", wrtcIceTexto(e));
			}

			/* Se mira stop_req cada 100 ms y no cada 500: al salir de
			 * la aplicacion, sesShutdown espera cinco segundos a que
			 * la maquina se suelte, y cada decima que se tarde en
			 * notar la peticion sale de ese presupuesto. */
			usleep(100000);
			if (++t < ka * 10) continue;
			t = 0;

			/* Y no se EMPIEZA un keepalive si ya nos vamos: es una
			 * peticion HTTPS entera que se pondria por delante del
			 * DELETE. */
			if (!ses_running || stop_req) break;

			{
				char path[256];
				cJSON *root = NULL;
				tlsResult res;

				snprintf(path, sizeof(path), "%s/keepalive", info.path);
				if (ses_call("POST", path, "", &root, &res,
				             "keepalive") == 0)
					info.polls++;
				if (root) cJSON_Delete(root);
			}
		}
	}

	/* UN SOLO SITIO QUE RECOJA, y todos los caminos pasan por aqui.
	 *
	 * Antes cada fallo hacia su propio wrtcSesionSoltar() y volvia, y en
	 * los cuatro faltaba decStop(): el decodificador se quedaba abierto
	 * con 32 MB reservados y su SPU cogido hasta el siguiente arranque.
	 * No es una maquina de xCloud, pero es el mismo error de fondo:
	 * equilibrar a mano lo que puede garantizar la estructura. Ya nos
	 * costo lo mismo con tlsHold en run_session. */
recoger:
	wrtcSesionBombear(0);
	wrtcSesionSoltar();
	decStop();
	audStop();
}

/* LA PRIORIDAD SE COGE Y SE SUELTA AQUI, y en ningun otro sitio.
 *
 * La primera version la soltaba en los ocho caminos de salida de la
 * funcion de dentro. Funcionaba, y era exactamente el patron que llevamos
 * toda la sesion quitando de otros sitios: equilibrar a mano lo que puede
 * garantizar la estructura. Basta con que alguien anada un `return` dentro
 * para que el catalogo se quede apartado el resto de la sesion, sin que
 * nada lo diga.
 *
 * Asi hay UNA cogida y UNA soltada, y no hay camino de salida que se las
 * salte. */
static void run_session(void)
{
	tlsHold(1);
	run_session_inner();
	tlsHold(0);
}

static void ses_thread(void *arg)
{
	(void)arg;

	while (ses_running) {
		if (stop_req) {
			stop_req = 0;
			step_delete();
			if (info.state != SES_FAILED) info.state = SES_IDLE;
			continue;
		}

		if (start_req) {
			start_req = 0;
			run_session();
			continue;
		}

		usleep(100000);
	}

	sysThreadExit(0);
}

/* --------------------------------------------------------------------- */

int sesInit(void)
{
	memset(&info, 0, sizeof(info));
	info.queue_pos = -1;

	ses_running = 1;
	if (sysThreadCreate(&ses_tid, ses_thread, NULL, SES_THREAD_PRIO,
	                    SES_THREAD_STACK, THREAD_JOINABLE,
	                    "GR33N sesion") != 0) {
		ses_running = 0;
		sfail("sysThreadCreate del hilo de sesion de juego fallo");
		return -1;
	}

	ses_started = 1;
	return 0;
}

void sesShutdown(void)
{
	if (!ses_started) return;

	/* La maquina se suelta ANTES de parar el hilo, y desde aqui, que es
	 * el camino de salida de la aplicacion.
	 *
	 * Y SE ESPERA A QUE SALGA DE VERDAD. Aqui habia un usleep de 300 ms
	 * fijo, y no daban: el hilo tarda hasta 100 ms en ver stop_req, y el
	 * DELETE es una conexion y una negociacion TLS enteras, unos 340 ms
	 * medidos. El tlsAbort de la linea siguiente llegaba a mitad del
	 * saludo y la maquina se quedaba reservada CADA VEZ que se salia de la
	 * aplicacion con una sesion abierta.
	 *
	 * Cinco segundos de techo. Salir un poco mas lento es infinitamente
	 * mejor que dejarle a Microsoft una maquina colgada con tu nombre. */
	if (info.path[0]) {
		int i;

		stop_req = 1;

		for (i = 0; i < 50 && info.path[0]; i++)
			usleep(100000);

		if (info.path[0])
			printf("[ses] cinco segundos y el DELETE no ha salido; "
			       "la maquina puede quedar reservada\n");
	}

	ses_running = 0;
	tlsAbort();

	{
		u64 rv = 0;
		sysThreadJoin(ses_tid, &rv);
	}

	ses_started = 0;
}

int sesStart(const char *title_id)
{
	if (!ses_started || !title_id || !title_id[0]) return -1;
	if (strlen(title_id) >= sizeof(want_title)) return -1;

	/* Una cada vez. Pedir dos maquinas a la vez es pedirle a Microsoft
	 * que te bloquee. */
	if (info.state != SES_IDLE && info.state != SES_FAILED) return -1;

	snprintf(want_title, sizeof(want_title), "%s", title_id);
	start_req = 1;
	return 0;
}

void sesStop(void)
{
	if (!ses_started) return;
	stop_req = 1;
}

/* Instantanea, como authStatus y tlsStatus. El hilo de dibujo lee esto
 * sesenta veces por segundo mientras el de la sesion escribe err[] y
 * server_state[] con vsnprintf: prestar el puntero es enseñar media frase. */
const sesInfo *sesStatus(void)
{
	static sesInfo snap;

	snap = info;
	return &snap;
}
