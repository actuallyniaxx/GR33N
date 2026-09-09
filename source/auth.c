/* GR33N - codigo de dispositivo contra Microsoft. */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ppu-types.h>
#include <sys/systime.h>
#include <sys/thread.h>
#include <sys/mutex.h>

#include "gr33n.h"
#include "link.h"
#include "store.h"
#include "net_tls.h"
#include "auth.h"
#include "imgdec.h"
#include "catalog.h"   /* CAT_MARKET: solo para compararlo con el de verdad */
#include "cJSON.h"

/* Identificador de aplicacion publico que usan los clientes abiertos de
 * xCloud para este flujo. No es un secreto y no puede serlo: un cliente
 * publico no puede guardar secretos, y por eso el flujo de codigo de
 * dispositivo esta pensado para no necesitarlos.
 *
 * Sale de green-nx, que es el cliente de xCloud para Switch: mismo
 * problema, misma solucion, ya probada contra el servicio real. */
#define AUTH_CLIENT_ID  "1f907974-e22b-4810-a9de-d9647380c97e"

/* Con espacios ya codificados. Los permisos que se piden:
 *   xboxlive.signin  entrar en Xbox Live
 *   openid profile   identidad basica
 *   offline_access   poder refrescar sin volver a pedir el codigo
 */
#define AUTH_SCOPE  "xboxlive.signin%20openid%20profile%20offline_access"

#define AUTH_HOST       "login.microsoftonline.com"
#define AUTH_PATH_CODE  "/consumers/oauth2/v2.0/devicecode"
#define AUTH_PATH_TOKEN "/consumers/oauth2/v2.0/token"

/* El portal ANTIGUO de cuentas Microsoft. Por aqui no se entra: se entra
 * por el de arriba. Pero el permiso que pide el /connect de la sesion de
 * juego es de los de Passport, con esa sintaxis de dos puntos dobles que
 * el portal nuevo no entiende, y eso solo lo emite este. Ver
 * authPassportToken() al final del fichero. */
#define MSA_HOST        "login.live.com"
#define MSA_PATH_TOKEN  "/oauth20_token.srf"

#define AUTH_FORM  "application/x-www-form-urlencoded"
#define AUTH_JSON  "application/json"

/* La cadena de Xbox Live. Tres servidores distintos, cada uno con su
 * formato, y el ultimo cambia segun a que servicio quieras entrar. */
#define XBL_USER_HOST  "user.auth.xboxlive.com"
#define XBL_USER_PATH  "/user/authenticate"
#define XSTS_HOST      "xsts.auth.xboxlive.com"
#define XSTS_PATH      "/xsts/authorize"
#define PROF_HOST      "profile.xboxlive.com"
#define PROF_PATH      "/users/me/profile/settings" \
                       "?settings=GameDisplayPicRaw,Gamertag,Gamerscore"

/* EL CAMPO QUE LO CAMBIA TODO. Un token XSTS no vale "para Xbox Live":
 * vale para UN servicio. Con xboxlive.com se lee el perfil; con
 * gssv.xboxlive.com se abre una sesion de streaming. Misma cadena, mismo
 * codigo, distinto destinatario. */
#define RP_XBOXLIVE    "http://xboxlive.com"
#define RP_GSSV        "http://gssv.xboxlive.com/"

/* La puerta de entrada de xCloud.
 *
 * El token XSTS de gssv NO va en una cabecera Authorization: va dentro del
 * cuerpo JSON, en el campo "token". Es la unica peticion de toda la cadena
 * que funciona asi, y perder media tarde con un 401 por mandarlo como
 * XBL3.0 seria muy nuestro estilo.
 *
 * offeringId elige el catalogo:
 *   xgpuweb      Game Pass Ultimate
 *   xgpuwebf2p   juegos gratuitos, en xgpuwebf2p.gssv-play-prod...
 *   xhome        streaming desde tu propia consola (otro host: xhome...)
 *
 * La respuesta trae gsToken y offeringSettings.regions[], y cada region
 * lleva su baseUri: a partir de ahi ya no se habla con este host, se habla
 * con el de la region. */
#define GSSV_HOST      "xgpuweb.gssv-play-prod.xboxlive.com"
#define GSSV_PATH      "/v2/login/user"
#define GSSV_OFFERING   "xgpuweb"
#define GSSV_CLIENT    "x-gssv-client: XboxComBrowser\r\n"

#define AUTH_THREAD_PRIO   1003
/* 192 KB y no 64. Los tokens grandes ya viven en estaticos, pero cJSON
 * analiza recursivamente y las respuestas de XSTS anidan mas de lo que
 * parece. Desbordar una pila en PS3 no da un error: da un cuelgue en un
 * sitio que no tiene nada que ver. */
#define AUTH_THREAD_STACK  (192 * 1024)

#define TOKEN_MAX  2048

/* --------------------------------------------------------------------- */

static sys_mutex_t mtx;
static int mtx_ok = 0;

#define LOCK()    do { if (mtx_ok) sysMutexLock(mtx, 0); } while (0)
#define UNLOCK()  do { if (mtx_ok) sysMutexUnlock(mtx); } while (0)

static authInfo info;

/* El device_code de Microsoft ronda los 1500 caracteres. La primera
 * version le dio 512 y json_str lo recorto EN SILENCIO, asi que
 * mandabamos un codigo a medias y el servidor contestaba, con toda la
 * razon, que no era valido.
 *
 * Segunda vez en dos horas que un buffer corto con recorte silencioso me
 * cuesta un ciclo entero de compilar-copiar-probar. Por eso json_str ya
 * no recorta: falla. */
static char device_code[2048];  /* el largo, el que NO se enseña */
static char refresh[TOKEN_MAX];

static xblProfile prof;

/* Los tokens de la cadena. Son grandes: el XSTS puede pasar de 4 KB.
 * Estaticos y no en pila porque el hilo tiene 64 KB y meter tres de
 * estos ahi es como se desborda una pila sin que nadie se entere. */
static char access_token[8192];
static char user_token[8192];
static char xsts_token[16384];
static char uhs[64];

/* El segundo XSTS, el de streaming, en su propio sitio. Comparte cadena
 * con el del perfil hasta el user_token y se separa en el ultimo paso. */
static char gssv_xsts[16384];
static char gssv_uhs[64];
static char gs_token[8192];

static xcloudInfo xc;

/* La lista de regiones, entera. La escribe el hilo de autenticacion al
 * terminar el login y la leen el del ping y el de dibujo, todo bajo el
 * mismo candado que xc. */
static xcRegion regs[XC_REGIONS_MAX];
static u32      regs_n = 0;
static int      reg_actual = -1;

/* El resultado de la ultima peticion de ESTE hilo.
 *
 * Antes se leia tlsStatus()/tlsBody() despues de volver de tlsRequest, y
 * eso es estado global de net_tls: el turno ya se ha soltado, asi que el
 * hilo del catalogo puede haber empezado otra peticion encima antes de
 * que nosotros miremos el cuerpo. Ahora la peticion nos copia lo suyo
 * mientras el turno todavia es nuestro.
 *
 * Solo lo toca el hilo de sesion, asi que no hace falta candado. */
static tlsResult ares;

/* Y su propio buffer de respuesta.
 *
 * Pasando NULL, la respuesta caia en el body[] estatico de net_tls, que es
 * COMPARTIDO: ares.body apuntaba ahi y se leia con el turno ya suelto. La
 * cabecera promete que el cuerpo del tlsResult vive en el buffer que TU
 * diste; con NULL esa promesa era mentira para siete de las ocho
 * peticiones de este fichero.
 *
 * 32 KB. La respuesta mas grande de la cadena es el XSTS y no llega a 8. */
static char abody[32 * 1024];

static volatile int prof_req = 0;
static volatile int xc_req   = 0;

/* ¿Hay token guardado? En memoria, NO preguntandoselo al disco.
 *
 * storeHasToken() abre y cierra un fichero. Parece gratis y no lo es: la
 * barra superior llama a authHaveToken() en CADA fotograma para decidir si
 * pone "Cargando perfil..." o "Sin sesion", asi que eran sesenta aperturas
 * de fichero por segundo en el hilo de dibujo. En el log se veia la fase de
 * contenido subiendo de 5 ms a 74, y algun fotograma suelto de 675 ms.
 *
 * Aqui la respuesta cambia tres veces en toda la sesion: al arrancar, al
 * guardar un token y al olvidarlo. */
static volatile int token_present = 0;

static sys_ppu_thread_t auth_tid;
static volatile int auth_running = 0;
static int auth_started = 0;
static volatile int start_req = 0;
static volatile int cancel_req = 0;

/* --------------------------------------------------------------------- */

static u64 now_us(void)
{
	u64 sec = 0, nsec = 0;
	sysGetCurrentTime(&sec, &nsec);
	return sec * 1000000ull + nsec / 1000ull;
}

static void alog(const char *fmt, ...)
{
	char buf[192];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	printf("[auth] %s\n", buf);
	linkLog("[auth] %s", buf);
}

static void fail(const char *fmt, ...)
{
	va_list ap;

	LOCK();
	info.state = AUTH_FAILED;
	va_start(ap, fmt);
	vsnprintf(info.err, sizeof(info.err), fmt, ap);
	va_end(ap);
	UNLOCK();

	printf("[auth] %s\n", info.err);
	linkLog("[auth] FALLO: %s", info.err);
}

/* --------------------------------------------------------------------- */
/* JSON, lo justo                                                        */
/*                                                                       */
/* Un analizador de JSON de verdad son mil lineas y aqui las respuestas   */
/* son planas: una decena de campos, sin anidar, sin arrays. Esto busca   */
/* la clave y lee el valor, y ya.                                        */
/*                                                                       */
/* LIMITACIONES, que se apuntan para que nadie se confie:                 */
/*   - No entiende anidamiento. Una clave repetida dentro de un objeto    */
/*     interior se confundiria con la de fuera.                           */
/*   - De los escapes solo resuelve los que aparecen de verdad aqui.      */
/*   - No valida que el JSON sea JSON.                                    */
/*                                                                       */
/* Sirve para ESTE flujo y nada mas. Cuando entre libpeer viene cJSON     */
/* con el, y entonces esto se tira.                                       */
/* --------------------------------------------------------------------- */

static const char *find_key(const char *js, const char *key)
{
	char pat[64];
	const char *p;

	snprintf(pat, sizeof(pat), "\"%s\"", key);

	p = strstr(js, pat);
	if (!p) return NULL;

	p += strlen(pat);
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
	if (*p != ':') return NULL;
	p++;
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;

	return p;
}

/* Devuelve  0 = bien
 *          -1 = no esta la clave
 *          -2 = esta pero NO CABE
 *
 * El -2 es el importante. Recortar una cadena y devolver exito es
 * mentirle a quien llama, y la mentira sale a la luz mucho mas tarde y en
 * otro sitio: un token a medias no da error de tamano, da "credencial
 * invalida" desde un servidor a 2000 km. */
static int json_str(const char *js, const char *key, char *out, u32 max)
{
	const char *p = find_key(js, key);
	u32 n = 0;

	if (!p || *p != '"') return -1;
	p++;

	while (*p && *p != '"') {
		if (n + 1 >= max) { out[0] = '\0'; return -2; }
		if (*p == '\\') {
			p++;
			if (!*p) break;
			switch (*p) {
			case 'n': out[n++] = '\n'; break;
			case 't': out[n++] = '\t'; break;
			case 'r': break;              /* se tira */
			case 'u': /* \uXXXX: no nos llega ninguno, se salta */
				if (p[1] && p[2] && p[3] && p[4]) p += 4;
				break;
			default:  out[n++] = *p; break;   /* \" \\ \/ */
			}
			p++;
		} else {
			out[n++] = *p++;
		}
	}

	out[n] = '\0';
	return 0;
}

static int json_int(const char *js, const char *key, int *out)
{
	const char *p = find_key(js, key);

	if (!p) return -1;
	if (*p != '-' && (*p < '0' || *p > '9')) return -1;

	*out = atoi(p);
	return 0;
}

/* --------------------------------------------------------------------- */
/* El flujo                                                              */
/* --------------------------------------------------------------------- */

static int request_code(void)
{
	static char form[256];
	const char *js;
	int iv = 5, exp = 900;

	snprintf(form, sizeof(form), "client_id=%s&scope=%s",
	         AUTH_CLIENT_ID, AUTH_SCOPE);

	alog("1/2 pidiendo codigo a %s...", AUTH_HOST);

	if (tlsRequestTo(AUTH_HOST, AUTH_PATH_CODE, "POST", AUTH_FORM, form, NULL,
	                 abody, sizeof(abody), &ares) != 0) {
		fail("no se pudo pedir el codigo: %s", ares.err);
		return -1;
	}

	js = ares.body;

	if (ares.http_status != 200 || !js) {
		char e[96] = "";
		if (js) json_str(js, "error_description", e, sizeof(e));
		fail("HTTP %d al pedir el codigo%s%s", ares.http_status,
		     e[0] ? ": " : "", e);
		return -1;
	}

	LOCK();
	if (json_str(js, "user_code", info.user_code, sizeof(info.user_code)) != 0 ||
	    json_str(js, "verification_uri", info.verify_uri,
	             sizeof(info.verify_uri)) != 0) {
		UNLOCK();
		fail("la respuesta no trae user_code o verification_uri");
		return -1;
	}
	UNLOCK();

	{
		int r = json_str(js, "device_code", device_code, sizeof(device_code));

		if (r == -2) {
			fail("el device_code no cabe en %u bytes",
			     (unsigned)sizeof(device_code));
			return -1;
		}
		if (r != 0) {
			fail("la respuesta no trae device_code");
			return -1;
		}
	}

	json_int(js, "interval", &iv);
	json_int(js, "expires_in", &exp);

	LOCK();
	info.interval_s = (iv > 0) ? (u32)iv : 5;
	info.expires_s  = (exp > 0) ? (u32)exp : 900;
	info.elapsed_s  = 0;
	info.polls      = 0;
	info.state      = AUTH_WAITING;
	UNLOCK();

	alog("1/2 codigo %s en %s (vale %u s, sondeo cada %u s)",
	     info.user_code, info.verify_uri,
	     (unsigned)info.expires_s, (unsigned)info.interval_s);
	alog("1/2 device_code de %u bytes", (unsigned)strlen(device_code));

	return 0;
}

/* 1 = autorizado, 0 = todavia no, -1 = se acabo */
static int poll_once(void)
{
	static char form[4096];
	const char *js;
	char kind[64] = "";

	{
		int n = snprintf(form, sizeof(form),
		                 "grant_type=urn:ietf:params:oauth:grant-type:"
		                 "device_code&client_id=%s&device_code=%s",
		                 AUTH_CLIENT_ID, device_code);

		if (n < 0 || (size_t)n >= sizeof(form)) {
			fail("el formulario del sondeo no cabe (%d bytes)", n);
			return -1;
		}
	}

	if (tlsRequestTo(AUTH_HOST, AUTH_PATH_TOKEN, "POST", AUTH_FORM, form, NULL,
	                 abody, sizeof(abody), &ares) != 0) {
		/* Un fallo de red en un sondeo NO es un fallo del flujo: por
		 * WiFi se cae una peticion de cada tantas y el codigo sigue
		 * siendo valido. Se reintenta en la siguiente vuelta. */
		alog("sondeo %u: sin respuesta, se reintenta",
		     (unsigned)info.polls);
		return 0;
	}

	js = ares.body;

	LOCK();
	info.http_status = ares.http_status;
	info.polls++;
	UNLOCK();

	if (!js) return 0;

	if (ares.http_status == 200) {
		if (json_str(js, "refresh_token", refresh, sizeof(refresh)) != 0) {
			fail("autorizado pero sin refresh_token en la respuesta");
			return -1;
		}

		if (storeSaveToken(refresh, (u32)strlen(refresh)) != 0)
			alog("!! autorizado, pero el token no se pudo guardar: "
			     "habra que repetir esto en el proximo arranque");
		else {
			token_present = 1;
			alog("token guardado (%u bytes)", (unsigned)strlen(refresh));
		}

		return 1;
	}

	json_str(js, "error", kind, sizeof(kind));

	/* Los dos "sigue esperando" del estandar. slow_down significa que
	 * estamos preguntando demasiado y hay que espaciar; ignorarlo lleva
	 * a que el servidor deje de contestar. */
	if (strcmp(kind, "authorization_pending") == 0) return 0;

	if (strcmp(kind, "slow_down") == 0) {
		LOCK();
		info.interval_s += 5;
		UNLOCK();
		alog("el servidor pide calma: sondeo cada %u s",
		     (unsigned)info.interval_s);
		return 0;
	}

	if (strcmp(kind, "expired_token") == 0 ||
	    strcmp(kind, "bad_verification_code") == 0) {
		fail("el codigo ha caducado, hay que empezar de nuevo");
		return -1;
	}

	{
		char desc[128] = "";
		json_str(js, "error_description", desc, sizeof(desc));
		fail("%s%s%s", kind[0] ? kind : "error desconocido",
		     desc[0] ? ": " : "", desc);
	}

	return -1;
}

static void run_flow(void)
{
	u32 waited = 0;

	LOCK();
	memset(&info, 0, sizeof(info));
	info.state = AUTH_REQUESTING;
	UNLOCK();

	if (request_code() != 0) return;

	for (;;) {
		u32 iv, exp;
		u32 slept = 0;

		LOCK();
		iv  = info.interval_s;
		exp = info.expires_s;
		UNLOCK();

		/* Se duerme en trozos de un segundo para poder cancelar sin
		 * esperar el intervalo entero. Un boton que tarda cinco
		 * segundos en responder parece roto. */
		while (slept < iv) {
			if (cancel_req || !auth_running) {
				cancel_req = 0;
				LOCK(); info.state = AUTH_IDLE; UNLOCK();
				alog("cancelado");
				return;
			}
			usleep(1000000);
			slept++;
			waited++;

			LOCK(); info.elapsed_s = waited; UNLOCK();
		}

		if (waited >= exp) {
			fail("se agotaron los %u s del codigo", (unsigned)exp);
			return;
		}

		{
			int r = poll_once();

			if (r < 0) return;
			if (r > 0) {
				LOCK();
				info.state = AUTH_OK;
				UNLOCK();
				alog("2/2 sesion iniciada");
				return;
			}
		}
	}
}


/* --------------------------------------------------------------------- */
/* La cadena de Xbox Live                                                */
/*                                                                       */
/* Aqui SI hace falta un analizador de JSON de verdad. Las respuestas de  */
/* XSTS traen DisplayClaims.xui[0].uhs - objeto dentro de objeto dentro   */
/* de array - y el perfil viene como una lista de pares id/valor. Mi      */
/* extractor plano no lee nada de eso, y ya avise cuando lo escribi que   */
/* aqui es donde se rompia. Por eso entra cJSON.                         */
/* --------------------------------------------------------------------- */

/* Guarda una cadena de un JSON en un buffer, fallando si no cabe. Misma
 * disciplina que json_str: recortar en silencio es como perdimos una
 * tarde con el device_code. */
static int take_str(const cJSON *node, char *out, u32 max, const char *what)
{
	const char *v;

	if (!cJSON_IsString(node) || !node->valuestring) {
		fail("%s: no viene en la respuesta", what);
		return -1;
	}

	v = node->valuestring;

	if (strlen(v) + 1 > max) {
		fail("%s no cabe: %u bytes, hay sitio para %u",
		     what, (unsigned)strlen(v), (unsigned)max - 1);
		return -1;
	}

	snprintf(out, max, "%s", v);
	return 0;
}

/* Lee la respuesta de la ultima peticion y la analiza. NULL si algo va
 * mal; el motivo queda en info.err. */
static cJSON *parse_body(const char *what, int want_status)
{
	const char *js = ares.body;
	cJSON *root;

	if (!js) { fail("%s: sin respuesta", what); return NULL; }

	root = cJSON_Parse(js);
	if (!root) {
		fail("%s: la respuesta no es JSON (HTTP %d)", what,
		     ares.http_status);
		return NULL;
	}

	if (ares.http_status != want_status) {
		/* Los servicios de Xbox devuelven el motivo en XErr, un numero
		 * que no dice nada por si solo pero que se busca bien. */
		cJSON *xerr = cJSON_GetObjectItemCaseSensitive(root, "XErr");
		cJSON *desc = cJSON_GetObjectItemCaseSensitive(root, "Message");

		fail("%s: HTTP %d%s%.0f%s%s", what, ares.http_status,
		     cJSON_IsNumber(xerr) ? " XErr " : "",
		     cJSON_IsNumber(xerr) ? xerr->valuedouble : 0.0,
		     (cJSON_IsString(desc) && desc->valuestring[0]) ? " - " : "",
		     (cJSON_IsString(desc) && desc->valuestring[0]) ? desc->valuestring : "");

		cJSON_Delete(root);
		return NULL;
	}

	return root;
}

/* 1/4 - el refresh token guardado a cambio de uno de acceso, que dura
 * una hora. */
static int step_access_token(void)
{
	static char form[4096];
	cJSON *root, *tok;
	int n;

	n = snprintf(form, sizeof(form),
	             "client_id=%s&grant_type=refresh_token&refresh_token=%s",
	             AUTH_CLIENT_ID, refresh);
	if (n < 0 || (size_t)n >= sizeof(form)) {
		fail("el formulario de refresco no cabe");
		return -1;
	}

	if (tlsRequestTo(AUTH_HOST, AUTH_PATH_TOKEN, "POST",
	                 AUTH_FORM, form, NULL, abody, sizeof(abody), &ares) != 0) {
		fail("1/4 sin respuesta al refrescar el token");
		return -1;
	}

	root = parse_body("1/4 refrescar token", 200);
	if (!root) return -1;

	tok = cJSON_GetObjectItemCaseSensitive(root, "access_token");
	if (take_str(tok, access_token, sizeof(access_token), "access_token") != 0) {
		cJSON_Delete(root);
		return -1;
	}

	/* Microsoft puede rotar el refresh token en cada uso. Si manda uno
	 * nuevo hay que guardarlo: el viejo deja de valer, y no darse cuenta
	 * significa que la sesion se cae sola dentro de unos dias. */
	{
		cJSON *nr = cJSON_GetObjectItemCaseSensitive(root, "refresh_token");

		if (cJSON_IsString(nr) && nr->valuestring &&
		    strlen(nr->valuestring) + 1 <= sizeof(refresh) &&
		    strcmp(nr->valuestring, refresh) != 0) {
			snprintf(refresh, sizeof(refresh), "%s", nr->valuestring);
			if (storeSaveToken(refresh, (u32)strlen(refresh)) == 0)
				token_present = 1;
			alog("1/4 el servidor roto el token de refresco, guardado el nuevo");
		}
	}

	cJSON_Delete(root);
	alog("1/4 access_token de %u bytes", (unsigned)strlen(access_token));
	return 0;
}

/* 2/4 - el token de Microsoft a cambio de uno de Xbox Live. */
static int step_user_token(void)
{
	static char body[12288];
	cJSON *root, *tok;
	int n;

	n = snprintf(body, sizeof(body),
	             "{\"Properties\":{\"AuthMethod\":\"RPS\","
	             "\"SiteName\":\"user.auth.xboxlive.com\","
	             "\"RpsTicket\":\"d=%s\"},"
	             "\"RelyingParty\":\"http://auth.xboxlive.com\","
	             "\"TokenType\":\"JWT\"}",
	             access_token);
	if (n < 0 || (size_t)n >= sizeof(body)) {
		fail("el cuerpo de user/authenticate no cabe");
		return -1;
	}

	if (tlsRequestTo(XBL_USER_HOST, XBL_USER_PATH, "POST",
	                 AUTH_JSON, body,
	                 "x-xbl-contract-version: 1\r\n",
	                 abody, sizeof(abody), &ares) != 0) {
		fail("2/4 sin respuesta de %s", XBL_USER_HOST);
		return -1;
	}

	root = parse_body("2/4 user token", 200);
	if (!root) return -1;

	tok = cJSON_GetObjectItemCaseSensitive(root, "Token");
	if (take_str(tok, user_token, sizeof(user_token), "Token de usuario") != 0) {
		cJSON_Delete(root);
		return -1;
	}

	cJSON_Delete(root);
	alog("2/4 user_token de %u bytes", (unsigned)strlen(user_token));
	return 0;
}

/* 3/4 - autorizacion para UN servicio. El relying_party decide cual. */
/* El destino va por parametro porque la MISMA cadena se recorre dos veces
 * con dos destinatarios distintos: xboxlive.com para el perfil y
 * gssv.xboxlive.com para el streaming. Con un solo buffer global, pedir el
 * segundo pisaba el primero y refrescar el perfil despues invalidaba la
 * sesion de xCloud sin que nadie lo viera venir. */
static int step_xsts(const char *relying_party,
                     char *tok_out, u32 tok_max,
                     char *uhs_out, u32 uhs_max)
{
	static char body[12288];
	cJSON *root, *tok, *claims, *xui, *first, *u;
	int n;

	n = snprintf(body, sizeof(body),
	             "{\"Properties\":{\"SandboxId\":\"RETAIL\","
	             "\"UserTokens\":[\"%s\"]},"
	             "\"RelyingParty\":\"%s\","
	             "\"TokenType\":\"JWT\"}",
	             user_token, relying_party);
	if (n < 0 || (size_t)n >= sizeof(body)) {
		fail("el cuerpo de xsts/authorize no cabe");
		return -1;
	}

	if (tlsRequestTo(XSTS_HOST, XSTS_PATH, "POST",
	                 AUTH_JSON, body,
	                 "x-xbl-contract-version: 1\r\n",
	                 abody, sizeof(abody), &ares) != 0) {
		fail("3/4 sin respuesta de %s", XSTS_HOST);
		return -1;
	}

	root = parse_body("3/4 XSTS", 200);
	if (!root) return -1;

	tok = cJSON_GetObjectItemCaseSensitive(root, "Token");
	if (take_str(tok, tok_out, tok_max, "Token XSTS") != 0) {
		cJSON_Delete(root);
		return -1;
	}

	/* DisplayClaims.xui[0].uhs. Este es el camino que mi extractor plano
	 * no sabia recorrer, y el motivo por el que entro cJSON. El uhs es
	 * la mitad de la credencial: sin el, el token no vale. */
	claims = cJSON_GetObjectItemCaseSensitive(root, "DisplayClaims");
	xui    = claims ? cJSON_GetObjectItemCaseSensitive(claims, "xui") : NULL;
	first  = cJSON_IsArray(xui) ? cJSON_GetArrayItem(xui, 0) : NULL;
	u      = first ? cJSON_GetObjectItemCaseSensitive(first, "uhs") : NULL;

	if (take_str(u, uhs_out, uhs_max, "uhs") != 0) {
		cJSON_Delete(root);
		return -1;
	}

	cJSON_Delete(root);
	alog("3/4 XSTS ok para %s (%u bytes, uhs %s)", relying_party,
	     (unsigned)strlen(tok_out), uhs_out);
	return 0;
}

/* --------------------------------------------------------------------- */
/* xCloud: el XSTS de streaming y la puerta de entrada                   */
/* --------------------------------------------------------------------- */

static void xcfail(const char *fmt, ...)
{
	va_list ap;

	LOCK();
	xc.state = XC_FAILED;
	va_start(ap, fmt);
	vsnprintf(xc.err, sizeof(xc.err), fmt, ap);
	va_end(ap);
	UNLOCK();

	alog("xcloud: %s", xc.err);
}

static int step_gssv_login(void)
{
	static char body[20480];
	cJSON *root, *tok, *dur, *settings, *regions, *r;
	int n;

	n = snprintf(body, sizeof(body),
	             "{\"token\":\"%s\",\"offeringId\":\"" GSSV_OFFERING "\"}",
	             gssv_xsts);
	if (n < 0 || (size_t)n >= sizeof(body)) {
		xcfail("el cuerpo del login de xCloud no cabe");
		return -1;
	}

	if (tlsRequestTo(GSSV_HOST, GSSV_PATH, "POST", AUTH_JSON, body,
	                 GSSV_CLIENT, abody, sizeof(abody), &ares) != 0) {
		xcfail("sin respuesta de %s: %s", GSSV_HOST, ares.err);
		return -1;
	}

	{
		const char *js = ares.body;

		/* Aqui NO se usa parse_body: ese exige 200 y da el error en
		 * formato de Xbox Live (XErr). Este servicio contesta otra
		 * cosa, y en la primera prueba lo que interesa es ver el
		 * cuerpo entero, sea lo que sea. */
		if (ares.http_status != 200) {
			xcfail("HTTP %d en %s%s%.180s", ares.http_status, GSSV_PATH,
			       js ? " - " : "", js ? js : "");
			return -1;
		}

		root = js ? cJSON_Parse(js) : NULL;
		if (!root) {
			xcfail("la respuesta del login no es JSON");
			return -1;
		}
	}

	tok = cJSON_GetObjectItemCaseSensitive(root, "gsToken");
	if (take_str(tok, gs_token, sizeof(gs_token), "gsToken") != 0) {
		xcfail("%s", info.err);
		cJSON_Delete(root);
		return -1;
	}

	dur = cJSON_GetObjectItemCaseSensitive(root, "durationInSeconds");

	/* EL MERCADO QUE DICE MICROSOFT, comparado con el que damos por hecho.
	 *
	 * No se usa todavia para nada: se mira. Si esta cuenta resulta estar
	 * en otro mercado, el catalogo que pedimos con market=ES no es el que
	 * le corresponde, y eso explicaria titulos que faltan o que salen y no
	 * arrancan. Vale mas verlo en el log una vez que buscarlo un dia con
	 * media biblioteca rara. */
	{
		cJSON *mk = cJSON_GetObjectItemCaseSensitive(root, "market");

		if (cJSON_IsString(mk) && mk->valuestring) {
			if (strcmp(mk->valuestring, CAT_MARKET) != 0)
				alog("xcloud: !! Microsoft dice market=%s y nosotros "
				     "pedimos el catalogo con market=%s",
				     mk->valuestring, CAT_MARKET);
			else
				alog("xcloud: market=%s, el que esperabamos",
				     mk->valuestring);
		} else {
			alog("xcloud: el login no trae market");
		}
	}

	settings = cJSON_GetObjectItemCaseSensitive(root, "offeringSettings");
	regions  = settings ? cJSON_GetObjectItemCaseSensitive(settings, "regions")
	                    : NULL;

	LOCK();
	xc.token_len = (u32)strlen(gs_token);
	xc.expires_s = cJSON_IsNumber(dur) ? (u32)dur->valuedouble : 0;
	/* regions_n se pone ABAJO, con las que de verdad han entrado en la
	 * lista. Aqui salia el tamano del array de JSON, que cuenta tambien
	 * las entradas rotas y las que no caben: un numero que no se
	 * corresponde con nada que se pueda recorrer. */
	UNLOCK();

	/* Se GUARDAN todas las regiones, no solo la elegida.
	 *
	 * Antes se escribian en el log y se tiraban, que es tanto como no
	 * tenerlas: el ajuste de "Servidor" llevaba desde el primer dia con
	 * una lista inventada al lado de una lista de verdad que pasaba por
	 * aqui y se perdia. */
	LOCK();
	memset(regs, 0, sizeof(regs));
	regs_n = 0;
	reg_actual = -1;
	UNLOCK();

	if (cJSON_IsArray(regions)) {
		cJSON_ArrayForEach(r, regions) {
			cJSON *nm  = cJSON_GetObjectItemCaseSensitive(r, "name");
			cJSON *uri = cJSON_GetObjectItemCaseSensitive(r, "baseUri");
			cJSON *def = cJSON_GetObjectItemCaseSensitive(r, "isDefault");
			int is_def = cJSON_IsTrue(def);
			xcRegion nueva;
			char ruta[128];

			if (!cJSON_IsString(nm) || !cJSON_IsString(uri)) continue;

			alog("xcloud: region %s%s -> %s", nm->valuestring,
			     is_def ? " (por defecto)" : "", uri->valuestring);

			if (regs_n >= XC_REGIONS_MAX) {
				/* Se dice, no se disimula: si algun dia Microsoft
				 * publica mas de veinte, esto sale en el log y se
				 * sube la constante. Callarse seria tener media
				 * lista sin saberlo. */
				alog("xcloud: !! mas de %d regiones, %s no cabe",
				     XC_REGIONS_MAX, nm->valuestring);
				continue;
			}

			memset(&nueva, 0, sizeof(nueva));
			snprintf(nueva.name, sizeof(nueva.name), "%s",
			         nm->valuestring);
			snprintf(nueva.base_uri, sizeof(nueva.base_uri), "%s",
			         uri->valuestring);
			nueva.is_default = is_def;

			/* El host suelto se saca AQUI y no cuando haga falta:
			 * es la unica vez que tenemos el baseUri en la mano con
			 * la certeza de que viene del servidor, y partirlo una
			 * vez es mejor que partirlo tres veces mal. */
			if (tlsSplitUrl(nueva.base_uri, nueva.host,
			                sizeof(nueva.host), ruta, sizeof(ruta)) != 0)
				nueva.host[0] = '\0';

			LOCK();
			regs[regs_n++] = nueva;
			UNLOCK();

			if (is_def || !xc.base_uri[0]) {
				LOCK();
				reg_actual = (int)regs_n - 1;
				snprintf(xc.region, sizeof(xc.region), "%s",
				         nueva.name);
				snprintf(xc.base_uri, sizeof(xc.base_uri), "%s",
				         nueva.base_uri);
				UNLOCK();
			}
		}
	}

	LOCK();
	xc.regions_n = regs_n;
	UNLOCK();

	cJSON_Delete(root);

	if (!xc.base_uri[0]) {
		xcfail("el login no devolvio ninguna region utilizable");
		return -1;
	}

	return 0;
}

static void run_xcloud(void)
{
	u64 t0 = now_us();
	authState keep;

	LOCK();
	memset(&xc, 0, sizeof(xc));
	/* Y la lista de regiones con ella. Si esto se queda con la de la vuelta
	 * anterior, la interfaz sigue enseñando regiones y pings al lado de un
	 * login que acaba de fallar. */
	memset(regs, 0, sizeof(regs));
	regs_n = 0;
	reg_actual = -1;
	xc.state = XC_WORKING;
	/* take_str y compania escriben en info.err con fail(), que de paso
	 * pone la sesion en AUTH_FAILED. Aqui eso seria mentira: la sesion
	 * esta perfectamente y lo que falla es xCloud. Se guarda el estado y
	 * se devuelve al salir. */
	keep = info.state;
	UNLOCK();

	alog("xcloud: pidiendo XSTS para %s", RP_GSSV);

	/* EL EXPERIMENTO. Se manda la misma peticion que funciona para el
	 * perfil, cambiando solo el destinatario. Los clientes de escritorio
	 * mandan ademas un token de dispositivo, uno de titulo y una firma
	 * P-256 (el flujo SISU); si Microsoft exige eso tambien aqui, este
	 * paso va a fallar con un XErr y ya sabremos lo que cuesta el
	 * siguiente trozo del proyecto. */
	if (step_xsts(RP_GSSV, gssv_xsts, sizeof(gssv_xsts),
	              gssv_uhs, sizeof(gssv_uhs)) != 0) {
		xcfail("XSTS para streaming: %s", info.err);
		LOCK(); info.state = keep; UNLOCK();
		return;
	}

	if (step_gssv_login() != 0) {
		LOCK(); info.state = keep; UNLOCK();
		return;
	}

	LOCK();
	info.state  = keep;
	xc.state    = XC_OK;
	xc.ms_total = (u32)((now_us() - t0) / 1000);
	UNLOCK();

	alog("xcloud: LISTO. gsToken de %u bytes, vale %u s, %u regiones, "
	     "region %s (%u ms)",
	     (unsigned)xc.token_len, (unsigned)xc.expires_s,
	     (unsigned)xc.regions_n, xc.region, (unsigned)xc.ms_total);
}

/* SE COPIA BAJO CANDADO, no se presta el puntero.
 *
 * Esto devolvia `gs_token` a pelo y lo lee el hilo de la sesion de juego en
 * cada peticion, mientras run_xcloud() puede estar reescribiendolo desde el
 * hilo de autenticacion al renovar el token cada cuatro horas. Un Bearer
 * partido por la mitad da un 401, y session.c tiene escrito que un 401 ahi
 * significa "el gateway pide SISU": la carrera producia exactamente la
 * conclusion equivocada que ese comentario existe para evitar.
 *
 * Devuelve la longitud copiada, o 0 si no hay token o no cabe. */
u32 authGsTokenCopy(char *out, u32 max)
{
	u32 n = 0;

	if (!out || max < 2) return 0;

	LOCK();
	n = (u32)strlen(gs_token);
	if (n && n < max) memcpy(out, gs_token, n + 1);
	else { out[0] = '\0'; n = 0; }
	UNLOCK();

	return n;
}

const xcloudInfo *authXCloud(void)
{
	static xcloudInfo snap;

	LOCK();
	snap = xc;
	UNLOCK();

	return &snap;
}

/* --------------------------------------------------------------------- */
/* Las regiones                                                          */
/* --------------------------------------------------------------------- */

u32 authRegionsN(void)
{
	u32 n;

	LOCK();
	n = regs_n;
	UNLOCK();

	return n;
}

int authRegionCopy(int i, xcRegion *out)
{
	int r = -1;

	if (!out) return -1;

	LOCK();
	if (i >= 0 && (u32)i < regs_n) {
		*out = regs[i];
		r = 0;
	}
	UNLOCK();

	if (r != 0) memset(out, 0, sizeof(*out));

	return r;
}

int authRegionDefault(void)
{
	u32 i;
	int r = 0;

	LOCK();
	for (i = 0; i < regs_n; i++)
		if (regs[i].is_default) { r = (int)i; break; }
	UNLOCK();

	return r;
}

int authRegionActual(void)
{
	int r;

	LOCK();
	r = reg_actual;
	UNLOCK();

	return r;
}

int authRegionPorNombre(const char *name)
{
	u32 i;
	int r = -1;

	if (!name || !name[0]) return -1;

	LOCK();
	for (i = 0; i < regs_n; i++)
		if (strcmp(regs[i].name, name) == 0) { r = (int)i; break; }
	UNLOCK();

	return r;
}

int authSetRegion(int i)
{
	char nombre[64];

	LOCK();
	if (i < 0 || (u32)i >= regs_n) {
		UNLOCK();
		return -1;
	}

	reg_actual = i;
	snprintf(xc.region,   sizeof(xc.region),   "%s", regs[i].name);
	snprintf(xc.base_uri, sizeof(xc.base_uri), "%s", regs[i].base_uri);
	snprintf(nombre, sizeof(nombre), "%s", regs[i].name);
	UNLOCK();

	alog("xcloud: region activa -> %s", nombre);

	return 0;
}

/* --------------------------------------------------------------------- */
/* 5/4 - el avatar                                                       */
/*                                                                       */
/* Va detras del perfil y NO forma parte de la cadena: si falla, el       */
/* gamertag y los puntos siguen ahi. Una foto que no carga es un hueco;   */
/* un perfil que no carga es no haber entrado.                           */
/* --------------------------------------------------------------------- */

/* Un PNG de Xbox de 424x424 ronda los 150 KB. 384 da margen de sobra y, si
 * algun dia no diera, la peticion FALLA con un mensaje en vez de entregar
 * media imagen: el bucle de lectura ya no corta en silencio. */
#define AVATAR_DL_MAX  (384 * 1024)

static char avatar_dl[AVATAR_DL_MAX];
static u32  avatar_px[AVATAR_PX * AVATAR_PX];

/* Parte https://servidor/lo/que/sea en sus dos trozos.
 *
 * No es un analizador de URL: es lo justo para lo que devuelve Xbox. Si
 * algun dia mandan un puerto o algo raro, esto lo dice en vez de intentar
 * adivinar. */
static int split_url(const char *url, char *host, u32 hmax,
                     char *path, u32 pmax)
{
	const char *p = url;
	const char *slash;
	u32 hlen;

	if (strncmp(p, "https://", 8) == 0)      p += 8;
	else if (strncmp(p, "http://", 7) == 0)  p += 7;   /* se sube a TLS */
	else return -1;

	slash = strchr(p, '/');
	hlen  = slash ? (u32)(slash - p) : (u32)strlen(p);

	if (hlen == 0 || hlen >= hmax) return -1;
	if (memchr(p, ':', hlen))      return -2;   /* puerto: no se soporta */

	memcpy(host, p, hlen);
	host[hlen] = '\0';

	if (!slash) {
		snprintf(path, pmax, "/");
		return 0;
	}

	if (strlen(slash) >= pmax) return -3;
	snprintf(path, pmax, "%s", slash);

	return 0;
}

static int step_avatar(void)
{
	char host[96], path[1024];
	const char *body;
	u32 blen = 0;
	imgImage img;
	int rc;

	if (!prof.pic_url[0]) {
		snprintf(prof.pic_err, sizeof(prof.pic_err),
		         "el perfil no trae GameDisplayPicRaw");
		return -1;
	}

	rc = split_url(prof.pic_url, host, sizeof(host), path, sizeof(path));
	if (rc != 0) {
		snprintf(prof.pic_err, sizeof(prof.pic_err),
		         "no entiendo la URL del avatar (%d)", rc);
		alog("avatar: %s", prof.pic_err);
		return -1;
	}

	alog("avatar: %s%s", host, path);

	/* El PNG no cabe en el buffer de serie de net_tls, asi que se le pasa
	 * el nuestro. Va en la propia llamada: apuntarlo antes por separado es
	 * lo que dejaba un puntero suelto cuando el turno no llegaba. */
	if (tlsRequestTo(host, path, "GET", NULL, NULL, NULL,
	                 avatar_dl, sizeof(avatar_dl), &ares) != 0) {
		snprintf(prof.pic_err, sizeof(prof.pic_err), "%s", ares.err);
		alog("avatar: %s", prof.pic_err);
		return -1;
	}

	if (ares.http_status != 200) {
		snprintf(prof.pic_err, sizeof(prof.pic_err),
		         "HTTP %d al bajar el avatar", ares.http_status);
		alog("avatar: %s", prof.pic_err);
		return -1;
	}

	body = ares.body;
	blen = ares.len;
	if (!body || blen == 0) {
		snprintf(prof.pic_err, sizeof(prof.pic_err), "avatar vacio");
		return -1;
	}

	/* imgDecodeTo, y no imgDecodePNG, por dos motivos.
	 *
	 * Uno: la URL dice format=png, pero fiarse de lo que dice una URL es
	 * como acabamos mirando extensiones en vez de los primeros bytes. Si
	 * algun dia mandan otra cosa, funciona igual.
	 *
	 * Dos: esto corre en el hilo de sesion y el hilo del catalogo esta
	 * decodificando caratulas al mismo tiempo. imgDecodeTo decodifica y
	 * copia con el candado cogido; las otras dos comparten un buffer
	 * global y se pisarian. */
	if (imgDecodeTo(body, blen, avatar_px, AVATAR_PX, AVATAR_PX, &img) != 0) {
		snprintf(prof.pic_err, sizeof(prof.pic_err), "%s", img.err);
		return -1;
	}

	LOCK();
	prof.pic_ready = 1;
	UNLOCK();

	alog("avatar listo: %ux%u -> %ux%u en %u ms",
	     (unsigned)img.w, (unsigned)img.h,
	     (unsigned)AVATAR_PX, (unsigned)AVATAR_PX, (unsigned)img.ms);
	return 0;
}

const u32 *authAvatar(void)
{
	return prof.pic_ready ? avatar_px : NULL;
}

/* 4/4 - el perfil. */
static int step_profile(void)
{
	static char hdr[20480];
	cJSON *root, *users, *user0, *settings, *it;
	int n;

	/* XBL3.0 x=<uhs>;<token> es el formato exacto que espera Xbox Live.
	 * Ni Bearer ni nada estandar: esto es cosa suya. */
	n = snprintf(hdr, sizeof(hdr),
	             "Authorization: XBL3.0 x=%s;%s\r\n"
	             "x-xbl-contract-version: 2\r\n"
	             "Accept-Language: es-ES\r\n",
	             uhs, xsts_token);
	if (n < 0 || (size_t)n >= sizeof(hdr)) {
		fail("la cabecera de autorizacion no cabe");
		return -1;
	}

	if (tlsRequestTo(PROF_HOST, PROF_PATH, "GET", NULL, NULL, hdr,
	                 abody, sizeof(abody), &ares) != 0) {
		fail("4/4 sin respuesta de %s", PROF_HOST);
		return -1;
	}

	root = parse_body("4/4 perfil", 200);
	if (!root) return -1;

	users    = cJSON_GetObjectItemCaseSensitive(root, "profileUsers");
	user0    = cJSON_IsArray(users) ? cJSON_GetArrayItem(users, 0) : NULL;
	settings = user0 ? cJSON_GetObjectItemCaseSensitive(user0, "settings") : NULL;

	if (!cJSON_IsArray(settings)) {
		fail("4/4 el perfil no trae la lista de ajustes");
		cJSON_Delete(root);
		return -1;
	}

	/* Viene como lista de pares {id, value}, no como objeto con campos
	 * con nombre. Hay que recorrerla. */
	cJSON_ArrayForEach(it, settings) {
		cJSON *id  = cJSON_GetObjectItemCaseSensitive(it, "id");
		cJSON *val = cJSON_GetObjectItemCaseSensitive(it, "value");

		if (!cJSON_IsString(id) || !cJSON_IsString(val)) continue;

		if (strcmp(id->valuestring, "Gamertag") == 0)
			snprintf(prof.gamertag, sizeof(prof.gamertag), "%s",
			         val->valuestring);
		else if (strcmp(id->valuestring, "Gamerscore") == 0)
			prof.gamerscore = (u32)strtoul(val->valuestring, NULL, 10);
		else if (strcmp(id->valuestring, "GameDisplayPicRaw") == 0)
			snprintf(prof.pic_url, sizeof(prof.pic_url), "%s",
			         val->valuestring);
	}

	cJSON_Delete(root);

	if (!prof.gamertag[0]) {
		fail("4/4 el perfil no trae Gamertag");
		return -1;
	}

	return 0;
}

static void run_profile(void)
{
	u64 t0 = now_us();

	LOCK();
	memset(&prof, 0, sizeof(prof));
	prof.state = PROF_WORKING;
	UNLOCK();

	if (storeLoadToken(refresh, sizeof(refresh)) == 0) {
		LOCK();
		prof.state = PROF_NONE;
		UNLOCK();
		return;
	}

	alog("cadena de Xbox Live: 4 pasos");

	if (step_access_token() != 0 ||
	    step_user_token()   != 0 ||
	    step_xsts(RP_XBOXLIVE, xsts_token, sizeof(xsts_token),
	              uhs, sizeof(uhs)) != 0 ||
	    step_profile()      != 0) {
		LOCK();
		prof.state = PROF_FAILED;
		snprintf(prof.err, sizeof(prof.err), "%s", info.err);
		UNLOCK();
		return;
	}

	LOCK();
	prof.state    = PROF_OK;
	prof.ms_total = (u32)((now_us() - t0) / 1000);
	UNLOCK();

	alog("4/4 %s  %u G  (%u ms la cadena entera)",
	     prof.gamertag, (unsigned)prof.gamerscore, (unsigned)prof.ms_total);

	/* El avatar DESPUES de dar el perfil por bueno, a proposito. El
	 * gamertag ya se esta pintando mientras la foto viaja, y si la foto no
	 * llega no pasa nada: se queda el marco vacio. */
	step_avatar();

	/* Y detras, la prueba de xCloud. Encadenada aqui a proposito MIENTRAS
	 * SEA UN EXPERIMENTO: asi el dato aparece en el log de cada arranque
	 * sin tener que ir a buscarlo por los menus. Son dos negociaciones TLS
	 * mas, casi un segundo; cuando sepamos la respuesta esto se mueve a
	 * donde toque y deja de pagarse en cada arranque. */
	xc_req = 1;
}

static void auth_thread(void *arg)
{
	(void)arg;

	while (auth_running) {
		if (start_req) {
			start_req = 0;
			run_flow();
			/* Nada mas entrar, el perfil: es la comprobacion de que la
			 * sesion sirve para algo mas que existir. */
			if (info.state == AUTH_OK) prof_req = 1;
			continue;
		}

		/* LA PRIORIDAD DE TLS SE COGE AQUI, EN LA LLAMADA.
		 *
		 * MEDIDO, no supuesto. En el log del 5 de septiembre, en el
		 * mismo milisegundo:
		 *
		 *   [auth] 1/4 el servidor roto el token de refresco, guardado
		 *   [cat]  pidiendo datos de 6 titulos a catalog.gamepass.com
		 *   [auth] FALLO: 2/4 sin respuesta de user.auth.xboxlive.com
		 *
		 * net_tls tiene UN turno. El catalogo lo cogio justo entre el
		 * paso 1 y el 2, y la cadena de auth no reintenta: se cae
		 * entera y la aplicacion arranca sin sesion. El sintoma es
		 * "Cargando perfil" para siempre y "no hay sesion de xCloud"
		 * al abrir un juego. Se arregla reiniciando, que es lo que
		 * nadie deberia tener que hacer.
		 *
		 * catalog.c ya consulta tlsHeld() en sus dos caminos --el lote
		 * de la tienda y la descarga de caratulas-- asi que con pedir
		 * la prioridad basta para que se aparte solo. El mecanismo es
		 * el mismo que usa session.c para que abrir una partida no se
		 * quede sin turno, y alli ya esta probado.
		 *
		 * VA EN LA LLAMADA Y NO DENTRO de run_profile/run_xcloud a
		 * proposito: entre las dos suman siete `return`, y una
		 * prioridad que hay que acordarse de soltar en siete sitios es
		 * una prioridad que un dia se queda puesta y deja el catalogo
		 * mudo para siempre. Aqui lo garantiza la estructura. Es lo
		 * mismo que dice el comentario de step_reap en session.c.
		 *
		 * run_flow NO se envuelve, y tampoco es un olvido: el flujo del
		 * codigo de dispositivo se pasa MINUTOS esperando a que alguien
		 * teclee el codigo en el movil, preguntando cada cinco
		 * segundos. Apartar el catalogo todo ese rato seria cambiar un
		 * fallo por una biblioteca que no carga nunca. Y ademas no le
		 * hace falta: poll_once se reintenta solo al siguiente
		 * intervalo, que es justo lo que a esta cadena le falta. */
		if (prof_req) {
			prof_req = 0;
			tlsHold(1);
			run_profile();
			tlsHold(0);
			continue;
		}

		if (xc_req) {
			xc_req = 0;
			tlsHold(1);
			run_xcloud();
			tlsHold(0);
			continue;
		}
		usleep(20000);
	}

	sysThreadExit(0);
}

/* --------------------------------------------------------------------- */
/* API                                                                   */
/* --------------------------------------------------------------------- */

int authInit(void)
{
	sys_mutex_attr_t attr;

	memset(&info, 0, sizeof(info));
	info.state = AUTH_IDLE;

	sysMutexAttrInitialize(attr);
	if (sysMutexCreate(&mtx, &attr) == 0) mtx_ok = 1;

	/* Esto DEPENDE de que storeInit se haya hecho ya. Si no, storeHasToken
	 * contesta que no hay token - no que no lo sabe - y la cadena del
	 * perfil no arranca nunca, en silencio. Por eso storeInit subio en
	 * main.c por encima de esta llamada; el log de abajo esta para que si
	 * alguien vuelve a moverlo, se vea. */
	token_present = storeHasToken();

	alog("token guardado de una sesion anterior: %s (almacen %s)",
	     token_present ? "si" : "no",
	     storePath()[0] ? storePath() : "SIN INICIAR");

	if (token_present)
		prof_req = 1;   /* se comprueba solo al arrancar */

	auth_running = 1;
	if (sysThreadCreate(&auth_tid, auth_thread, NULL, AUTH_THREAD_PRIO,
	                    AUTH_THREAD_STACK, THREAD_JOINABLE,
	                    "GR33N auth") != 0) {
		auth_running = 0;
		fail("sysThreadCreate del hilo de sesion fallo");
		return -1;
	}
	auth_started = 1;

	return 0;
}

void authShutdown(void)
{
	/* Avisar ANTES de esperar, y cortar la peticion en vuelo. Si no, el
	 * join se queda esperando a un hilo metido en una llamada de red y la
	 * aplicacion no sale. */
	cancel_req = 1;
	tlsAbort();

	if (auth_started) {
		u64 rv = 0;
		auth_running = 0;
		sysThreadJoin(auth_tid, &rv);
		auth_started = 0;
	}

	/* El token en memoria se borra a mano. En una consola domestica es
	 * casi simbolico, pero dejar credenciales tiradas en el montón
	 * cuando ya no hacen falta es un mal habito que luego se paga. */
	memset(refresh, 0, sizeof(refresh));
	memset(device_code, 0, sizeof(device_code));
	memset(access_token, 0, sizeof(access_token));
	memset(user_token, 0, sizeof(user_token));
	memset(xsts_token, 0, sizeof(xsts_token));

	if (mtx_ok) { sysMutexDestroy(mtx); mtx_ok = 0; }
}

int authStart(void)
{
	LOCK();
	if (info.state == AUTH_REQUESTING || info.state == AUTH_WAITING) {
		UNLOCK();
		return -1;
	}
	UNLOCK();

	cancel_req = 0;
	start_req = 1;
	return 0;
}

void authCancel(void)
{
	cancel_req = 1;

	/* Y cortar la peticion HTTPS si hay una en vuelo. Sin esto, cancelar
	 * mientras se sondea no hacia nada visible hasta que la peticion
	 * terminara sola: el usuario pulsa y no pasa nada, que es la
	 * definicion de roto. */
	tlsAbort();
}

const authInfo *authStatus(void)
{
	static authInfo snap;

	LOCK();
	snap = info;
	UNLOCK();

	return &snap;
}

/* Del valor en memoria. Se llama desde el hilo de dibujo, sesenta veces por
 * segundo: aqui no se toca el disco. */
int authHaveToken(void) { return token_present; }

int authFetchProfile(void)
{
	if (!token_present) return -1;
	prof_req = 1;
	return 0;
}

const xblProfile *authProfile(void)
{
	static xblProfile snap;

	LOCK();
	snap = prof;
	UNLOCK();

	return &snap;
}

void authForget(void)
{
	storeClearToken();
	token_present = 0;
	memset(refresh, 0, sizeof(refresh));
	alog("token olvidado");
}

/* --------------------------------------------------------------------- */
/* El token de traspaso, para POST {sessionPath}/connect                 */
/* --------------------------------------------------------------------- */

/* UN CUARTO TOKEN, y no se parece a los otros tres.
 *
 * Cuando la sesion llega a "ReadyToConnect" hay que mandarle a Microsoft un
 * `userToken` que NO es el gsToken, ni el XSTS, ni el access_token de la
 * cadena. Es un token MSA con un permiso especifico -"traspaso de consola
 * en la nube"- que solo emite el portal antiguo de Passport:
 *
 *     POST login.live.com/oauth20_token.srf
 *     client_id=<el mismo>&grant_type=refresh_token&refresh_token=<el nuestro>
 *     &scope=service::http://Passport.NET/purpose::PURPOSE_XBOX_CLOUD_CONSOLE_TRANSFER_TOKEN
 *
 * Que el client_id sea EL MISMO que el nuestro es la razon de que esto
 * pueda funcionar: el refresh token que guardamos se emitio para
 * 1f907974-e22b-4810-a9de-d9647380c97e, que es exactamente el que usa
 * greenlight para pedir este otro. Si no coincidiera, haria falta un
 * segundo baile de codigo de dispositivo entero.
 *
 * Lo que NO esta comprobado es si un refresh token emitido por el portal
 * nuevo (login.microsoftonline.com, que es por donde entramos) se canjea en
 * el antiguo. Los dos son la misma identidad por detras, pero eso es una
 * suposicion mia, no un hecho. Por eso el cuerpo de la respuesta se vuelca
 * entero al log pase lo que pase: si contesta "invalid_grant", esa palabra
 * es la respuesta y no hay que adivinar nada.
 *
 * EL BUFFER LO PONE QUIEN LLAMA, igual que en tlsRequestTo y por el mismo
 * motivo: esto se llama desde el hilo de la sesion de juego, y `abody`/
 * `ares` son de la cadena de autenticacion. Dos hilos, un buffer, ya
 * sabemos como acaba.
 *
 * Devuelve 0, -1 si falla, o -2 si el modulo TLS esta ocupado (no es
 * fallo: hay que volver a intentarlo). */
int authPassportToken(char *out, u32 max, void *sink, u32 sink_cap,
                      char *err, u32 err_max)
{
	static const char *const XFER_SCOPE =
		"service%3A%3Ahttp%3A%2F%2FPassport.NET%2Fpurpose%3A%3A"
		"PURPOSE_XBOX_CLOUD_CONSOLE_TRANSFER_TOKEN";

	tlsResult r;
	char *form;
	cJSON *root, *tok;
	int n, rc = -1;

	if (err && err_max) err[0] = '\0';

	if (!out || max < 2) {
		if (err) snprintf(err, err_max, "sin sitio donde dejar el token");
		return -1;
	}
	out[0] = '\0';

	if (!token_present || !refresh[0]) {
		if (err) snprintf(err, err_max, "no hay token guardado");
		return -1;
	}

	/* El refresh token ronda los 2 KB; con el formulario alrededor, 4 KB
	 * sobran. En el monton y no en la pila: este hilo tiene 192 KB y no
	 * hace falta gastarlos aqui. */
	form = malloc(4096);
	if (!form) {
		if (err) snprintf(err, err_max, "sin memoria para el formulario");
		return -1;
	}

	/* BAJO CANDADO, porque `refresh` no es nuestro.
	 *
	 * Esto corre en el hilo de la sesion de juego, y el de autenticacion
	 * reescribe refresh[] en dos sitios: al cargarlo del disco y cuando
	 * Microsoft lo rota al renovar. Sin candado, entrar en la pantalla de
	 * login mientras se abre una sesion da un token empalmado por la
	 * mitad, login.live.com contesta "invalid_grant", y la maquina ya
	 * provisionada se tira a la basura por una carrera que parece un fallo
	 * del servidor. */
	LOCK();
	n = snprintf(form, 4096,
	             "client_id=%s&grant_type=refresh_token&refresh_token=%s"
	             "&scope=%s",
	             AUTH_CLIENT_ID, refresh, XFER_SCOPE);
	UNLOCK();

	if (n < 0 || (size_t)n >= 4096) {
		if (err) snprintf(err, err_max, "el formulario no cabe");
		free(form);
		return -1;
	}

	alog("token de traspaso: pidiendolo a %s", MSA_HOST);

	n = tlsRequestTo(MSA_HOST, MSA_PATH_TOKEN, "POST", AUTH_FORM, form,
	                 NULL, sink, sink_cap, &r);

	free(form);

	if (n == -2) return -2;

	if (n != 0) {
		if (err) snprintf(err, err_max, "sin respuesta de %s: %s",
		                  MSA_HOST, r.err);
		return -1;
	}

	/* ENTERO AL LOG, salga bien o mal. Es una peticion cuya respuesta no
	 * hemos visto nunca, y el dia que falle el motivo vendra escrito
	 * dentro. */
	alog("token de traspaso: HTTP %d, %u bytes", r.http_status,
	     (unsigned)r.len);
	if (r.body && r.len) {
		static const char *const SEC[] = { "\"access_token", "\"refresh_token",
		                                   "\"id_token" };
		static char seguro[1024];
		u32 off = 0, n;

		/* El token va tapado: de esta respuesta solo hace falta ver que
		 * llego y, si fallo, por que. Ver linkRedact en link.c. */
		linkRedact(seguro, sizeof(seguro), r.body, SEC,
		           (u32)(sizeof(SEC) / sizeof(SEC[0])));
		n = (u32)strlen(seguro);

		while (off < n && off < 512) {
			alog("  %.160s", seguro + off);
			off += 160;
		}
	}

	root = r.body ? cJSON_Parse(r.body) : NULL;

	if (r.http_status != 200) {
		const cJSON *e  = root ? cJSON_GetObjectItemCaseSensitive(root, "error") : NULL;
		const cJSON *ed = root ? cJSON_GetObjectItemCaseSensitive(root, "error_description") : NULL;

		if (err)
			snprintf(err, err_max, "HTTP %d: %s%s%.100s", r.http_status,
			         cJSON_IsString(e) ? e->valuestring : "(sin codigo)",
			         cJSON_IsString(ed) ? " - " : "",
			         cJSON_IsString(ed) ? ed->valuestring : "");
		goto salir;
	}

	if (!root) {
		if (err) snprintf(err, err_max, "la respuesta no es JSON");
		goto salir;
	}

	tok = cJSON_GetObjectItemCaseSensitive(root, "access_token");
	if (!cJSON_IsString(tok) || !tok->valuestring) {
		if (err) snprintf(err, err_max, "no viene access_token");
		goto salir;
	}

	/* Cortar esto en silencio seria mandar medio token y comerse un 401
	 * que apunta a la autenticacion cuando el fallo esta en un tamaño. Ya
	 * van cuatro en este proyecto. */
	if (strlen(tok->valuestring) >= max) {
		if (err) snprintf(err, err_max, "el token no cabe: %u bytes y el "
		                  "hueco son %u", (unsigned)strlen(tok->valuestring),
		                  (unsigned)max);
		goto salir;
	}

	snprintf(out, max, "%s", tok->valuestring);
	alog("token de traspaso: LISTO, %u bytes", (unsigned)strlen(out));
	rc = 0;

salir:
	if (root) cJSON_Delete(root);
	return rc;
}
