/* GR33N - TLS sobre PSL1GHT. La cola entre mbedTLS y la consola. */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <ppu-types.h>
#include <ppu-asm.h>
#include <sys/systime.h>
#include <sys/thread.h>
#include <sys/mutex.h>
#include <net/net.h>
#include <lv2/system.h>

#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/platform.h"
#include "mbedtls/platform_time.h"
#include "mbedtls/error.h"
#include "mbedtls/debug.h"

#include "gr33n.h"
#include "link.h"
#include "net_tls.h"
#include "ca_bundle.h"

#define TLS_THREAD_PRIO   1002
#define TLS_THREAD_STACK  (192 * 1024)   /* verificar una cadena RSA come pila */

/* Nivel de cháchara de mbedTLS al log remoto. 0 = nada, 4 = todo.
 * Con 1 salen los errores y el resumen de la negociacion, que es lo que
 * hace falta para saber por que ha fallado un apreton de manos. */
#define TLS_DEBUG_LEVEL   1

/* --------------------------------------------------------------------- */

static sys_mutex_t mtx;
static int mtx_ok = 0;

#define LOCK()    do { if (mtx_ok) sysMutexLock(mtx, 0); } while (0)
#define UNLOCK()  do { if (mtx_ok) sysMutexUnlock(mtx); } while (0)

static tlsInfo info;

static mbedtls_entropy_context  entropy;
static mbedtls_ctr_drbg_context drbg;
static mbedtls_x509_crt         cacert;
static int inited = 0;
static int busy = 0;

/* Donde cae la respuesta CRUDA: linea de estado, cabeceras y cuerpo, todo
 * seguido tal y como llega del socket.
 *
 * Hay dos punteros y no uno porque el avatar del perfil no cabe aqui. Un
 * PNG de Xbox son cientos de kilobytes y este buffer son 32 KB: quien
 * necesite mas trae el suyo en tlsRequestTo() y esto se queda para el JSON,
 * que es lo que pide el 95% de las peticiones. */
static char  body[TLS_BODY_MAX];

static char *sink     = body;
static u32   sink_cap = TLS_BODY_MAX;

/* El buffer de la peticion EN CURSO. Se pone despues de coger el turno y
 * se suelta al terminar, dentro del mismo claim/release.
 *
 * Antes era un "sitio para la siguiente peticion" que se ponia con
 * tlsSetSink() ANTES de pedir el turno, y ahi estaba el fallo: si el turno
 * no se conseguia -porque el hilo de sesion estaba en mitad de la cadena
 * de Xbox Live- el que llamaba se llevaba un -2, liberaba su buffer, y
 * dejaba este puntero apuntando a memoria liberada. La SIGUIENTE peticion
 * del otro hilo lo cogia, escribia sink[0] = '\0' encima, y volcaba ahi la
 * respuesta entera.
 *
 * Se vio como un byte: "formato desconocido: empieza por 00 d8 ff e0". Un
 * JPEG empieza por ff d8 ff e0. Ese 00 es este sink[0] = '\0' cayendo
 * sobre una caratula ya reservada para otra cosa. Y el crasheo, unos
 * segundos despues.
 *
 * La suposicion que lo permitia esta escrita en el comentario de claim():
 * "con el flujo actual - o pruebas TLS, o inicio de sesion, nunca los dos
 * - sobra". Dejo de ser verdad el dia que catalog.c se llevo su propio
 * hilo, y el comentario se quedo. */
static char *req_sink = NULL;
static u32   req_sink_cap = 0;

/* Donde empieza el cuerpo dentro de sink, y cuanto mide YA sin cabeceras.
 * info.body_len es lo que se lee desde fuera; raw_len es el total con
 * cabeceras, solo para los mensajes de diagnostico. */
static u32 body_off = 0;

/* Lo ULTIMO que cayo en el buffer de serie, y solo lo de ahi.
 *
 * tlsBody() lo lee el hilo de DIBUJO, sesenta veces por segundo, para la
 * pantalla de pruebas TLS. Devolvia "sink + body_off", o sea el buffer de
 * quien hubiera pedido lo ultimo - que puede ser un malloc del hilo del
 * catalogo a punto de liberarse. Tercera cara del mismo fallo.
 *
 * body[] es estatico y no se va a ninguna parte, asi que apuntando solo lo
 * suyo, tlsBody() se puede leer desde donde sea. Cuando la ultima peticion
 * traia su propio buffer, devuelve NULL: es la verdad, ese cuerpo no es
 * suyo para enseñarlo. */
static u32 dbg_off = 0, dbg_len = 0;
static u32 raw_len  = 0;

static sys_ppu_thread_t tls_tid;
static volatile int tls_running = 0;
static int tls_started = 0;

static volatile int req_go = 0;
static char req_host[96];
/* La URL del avatar lleva otra URL dentro, escapada. 256 se quedaba corto
 * y snprintf habria cortado por la mitad sin decir nada. */
static char req_path[1024];
static const char *req_method = "GET";
static const char *req_ctype  = NULL;
static const char *req_body   = NULL;
static const char *req_hdrs   = NULL;

static u64 tb_hz = 79800000ull;

/* Techo de una peticion entera y bandera de abandono.
 *
 * La primera version llamaba a netRecv a pelo, que bloquea. Cuando el
 * servidor se quedo esperando un cuerpo que nunca llegaba, el hilo se
 * quedo 40 segundos dentro de una llamada al sistema: cancelar no hacia
 * nada, y salir de la aplicacion tampoco, porque sysThreadJoin esperaba a
 * un hilo que no podia contestar. La consola parecia colgada y no lo
 * estaba - estaba esperando muy bien.
 *
 * Ahora se espera en trozos con netPoll y entre trozo y trozo se mira el
 * reloj y la bandera. Una espera que no se puede interrumpir es un
 * cuelgue con otro nombre. */
#define TLS_DEADLINE_US  25000000ull   /* 25 s por peticion completa */
#define TLS_POLL_MS      200

static u64 deadline_us = 0;
static volatile int tls_abort = 0;

/* --------------------------------------------------------------------- */

static void tlog(const char *fmt, ...)
{
	char buf[192];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	printf("[tls] %s\n", buf);
	linkLog("[tls] %s", buf);
}

static void fail(const char *fmt, ...)
{
	va_list ap;

	LOCK();
	info.state = TLS_FAILED;
	va_start(ap, fmt);
	vsnprintf(info.err, sizeof(info.err), fmt, ap);
	va_end(ap);
	UNLOCK();

	printf("[tls] %s\n", info.err);
	linkLog("[tls] FALLO: %s", info.err);
}

/* mbedTLS devuelve codigos negativos que no dicen nada por si solos.
 * mbedtls_strerror los traduce, y esa traduccion vale su peso en oro
 * cuando un apreton de manos falla a las tres de la manana. */
static void fail_mbed(const char *que, int ret)
{
	char txt[128];

	mbedtls_strerror(ret, txt, sizeof(txt));
	fail("%s: -0x%04x (%s)", que, (unsigned)(-ret), txt);
}

static u64 now_us(void)
{
	u64 sec = 0, nsec = 0;
	sysGetCurrentTime(&sec, &nsec);
	return sec * 1000000ull + nsec / 1000ull;
}

/* --------------------------------------------------------------------- */
/* Lo que mbedTLS espera del sistema y aqui no hay                       */
/* --------------------------------------------------------------------- */

/* Reloj MONOTONO, en milisegundos. No es "que hora es": es "cuanto ha
 * pasado", y tiene que subir siempre aunque alguien cambie la hora de la
 * consola. El registro de timebase de la PPE es exactamente eso: un
 * contador de hardware que solo avanza.
 *
 * La division se hace en dos pasos a proposito. Con t*1000 directo, el
 * u64 se desborda a los siete anos de encendido. No va a pasar, pero
 * escribirlo bien cuesta lo mismo. */
mbedtls_ms_time_t mbedtls_ms_time(void)
{
	u64 t = __gettime();

	return (mbedtls_ms_time_t)((t / tb_hz) * 1000ull +
	                           ((t % tb_hz) * 1000ull) / tb_hz);
}

/* Entropia. Esto es lo mas delicado del fichero: de aqui salen las claves
 * de sesion, y un generador predecible convierte todo el TLS en teatro.
 *
 * lv2 tiene generador de verdad (sysGetRandomNumber), asi que no hay que
 * inventarse nada raro. Dos cautelas:
 *
 *   1. Se pide en multiplos de 8. La syscall lo espera asi y pedirle 5
 *      bytes puede devolver error o escribir de mas.
 *
 *   2. NO se confia en el codigo de retorno como unica senal. Se
 *      comprueba tambien que lo devuelto no sea todo ceros. Si algun dia
 *      la syscall es un hueco que no hace nada, el codigo de retorno
 *      podria ser 0 y el buffer quedarse limpio: eso hay que detectarlo
 *      aqui y no descubrirlo cuando alguien nos adivine una clave. */
int mbedtls_hardware_poll(void *data, unsigned char *output,
                          size_t len, size_t *olen)
{
	u8 tmp[64] __attribute__((aligned(16)));
	size_t done = 0;

	(void)data;

	while (done < len) {
		size_t chunk = len - done;
		size_t ask;
		u32 ret;
		int all_zero = 1;
		size_t i;

		if (chunk > sizeof(tmp)) chunk = sizeof(tmp);
		ask = (chunk + 7u) & ~(size_t)7u;

		memset(tmp, 0, sizeof(tmp));
		ret = sysGetRandomNumber(tmp, (u64)ask);

		for (i = 0; i < ask; i++)
			if (tmp[i]) { all_zero = 0; break; }

		if (all_zero) {
			tlog("!! sysGetRandomNumber devolvio %u y solo ceros - "
			     "sin entropia no hay TLS", (unsigned)ret);
			return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
		}

		memcpy(output + done, tmp, chunk);
		done += chunk;
	}

	*olen = len;
	return 0;
}

/* "Que hora es", para comprobar caducidad de certificados. */
static mbedtls_time_t ps3_time(mbedtls_time_t *t)
{
	u64 sec = 0, nsec = 0;

	sysGetCurrentTime(&sec, &nsec);

	if (t) *t = (mbedtls_time_t)sec;
	return (mbedtls_time_t)sec;
}

/* --------------------------------------------------------------------- */
/* Transporte: mbedTLS habla por aqui, no por sockets BSD                */
/* --------------------------------------------------------------------- */

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
	s32 sock = (s32)(intptr_t)ctx;
	ssize_t n = netSend(sock, buf, len, 0);

	if (n < 0) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
	return (int)n;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
	s32 sock = (s32)(intptr_t)ctx;
	struct pollfd pfd;
	ssize_t n;
	int r;

	pfd.fd      = sock;
	pfd.events  = POLLIN;
	pfd.revents = 0;

	r = netPoll(&pfd, 1, TLS_POLL_MS);

	if (r == 0 || (r > 0 && !(pfd.revents & POLLIN))) {
		/* Nada todavia. Aqui es donde se mira si seguimos teniendo
		 * permiso para esperar. */
		if (tls_abort) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
		if (deadline_us && now_us() > deadline_us)
			return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
		return MBEDTLS_ERR_SSL_WANT_READ;
	}

	if (r < 0) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;

	n = netRecv(sock, buf, len, 0);

	if (n == 0)  return MBEDTLS_ERR_SSL_CONN_EOF;
	if (n < 0)   return MBEDTLS_ERR_SSL_INTERNAL_ERROR;

	return (int)n;
}

static void dbg_cb(void *ctx, int level, const char *file,
                   int line, const char *str)
{
	char clean[160];
	size_t n;

	(void)ctx;
	(void)level;
	(void)file;
	(void)line;

	snprintf(clean, sizeof(clean), "%s", str);
	n = strlen(clean);
	while (n && (clean[n-1] == '\n' || clean[n-1] == '\r')) clean[--n] = '\0';

	if (n) linkLog("[tls:mbed] %s", clean);
}

/* --------------------------------------------------------------------- */
/* Resolucion de nombres                                                 */
/* --------------------------------------------------------------------- */

/* OJO CON ESTA ESTRUCTURA. net_hostent guarda los punteros como u32, no
 * como punteros:
 *
 *     struct net_hostent { u32 h_name; u32 h_aliases; s32 h_addrtype;
 *                          s32 h_length; u32 h_addr_list; };
 *
 * Es el ABI de lv2 asomando: el kernel devuelve direcciones de 32 bits y
 * PSL1GHT las deja tal cual. h_addr_list apunta a un array de u32, y cada
 * uno de esos apunta a los 4 bytes de una direccion. Tratarlo como un
 * struct hostent normal compila y lee basura. */
static int resolve(const char *host, u32 *out_addr)
{
	struct net_hostent *he;
	u32 *list, first;

	he = netGetHostByName(host);
	if (!he) return -1;

	if (he->h_addrtype != AF_INET || he->h_length != 4) return -2;
	if (he->h_addr_list == 0) return -3;

	list = (u32*)(uintptr_t)he->h_addr_list;
	if (list[0] == 0) return -4;

	first = list[0];
	*out_addr = *(u32*)(uintptr_t)first;

	return 0;
}

/* --------------------------------------------------------------------- */
/* La peticion, entera, dentro del hilo                                  */
/* --------------------------------------------------------------------- */

static int  claim(void);
static void release(void);

static void set_state(tlsState st)
{
	LOCK();
	info.state = st;
	UNLOCK();
}

/* Busca una secuencia dentro de un bloque binario. memmem no existe aqui y
 * strstr no sirve: la respuesta puede llevar ceros. */
static int find_bytes(const char *hay, u32 hlen, const char *pat, u32 plen)
{
	u32 i;

	if (plen == 0 || hlen < plen) return -1;

	for (i = 0; i <= hlen - plen; i++)
		if (memcmp(hay + i, pat, plen) == 0) return (int)i;

	return -1;
}

/* Compara sin distinguir mayusculas. Las cabeceras HTTP no las distinguen y
 * cada servidor escribe como le apetece: "Transfer-Encoding", "transfer-
 * encoding" y "TRANSFER-ENCODING" son la misma. */
static int hdr_has(const char *h, u32 hlen, const char *needle)
{
	u32 nlen = (u32)strlen(needle);
	u32 i, j;

	if (hlen < nlen) return 0;

	for (i = 0; i <= hlen - nlen; i++) {
		for (j = 0; j < nlen; j++) {
			char a = h[i + j], b = needle[j];
			if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
			if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
			if (a != b) break;
		}
		if (j == nlen) return 1;
	}

	return 0;
}

/* Localiza una cabecera y devuelve donde empieza su valor, o NULL.
 * El nombre se pasa en minusculas y con sus dos puntos. */
static const char *find_header(const char *hdrs, const char *name)
{
	u32 hlen = (u32)strlen(hdrs);
	u32 nlen = (u32)strlen(name);
	u32 i, j;

	if (hlen < nlen) return NULL;

	for (i = 0; i <= hlen - nlen; i++) {
		/* Solo al principio de linea, o "x-content-length" colaria. */
		if (i && hdrs[i - 1] != '\n') continue;

		for (j = 0; j < nlen; j++) {
			char a = hdrs[i + j], b = name[j];
			if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
			if (a != b) break;
		}

		if (j == nlen) {
			const char *v = hdrs + i + nlen;
			while (*v == ' ' || *v == '\t') v++;
			return v;
		}
	}

	return NULL;
}

/* Deshace el troceado de Transfer-Encoding: chunked, en el sitio.
 *
 * Devuelve el tamano del cuerpo ya limpio, o -1 si el troceado esta mal
 * formado. Mueve los bytes hacia atras encima de las cabeceras de cada
 * trozo, asi que no hace falta un segundo buffer.
 *
 *   1a4\r\n <420 bytes> \r\n 0\r\n\r\n
 *
 * Sin esto, cJSON_Parse se encuentra "1a4" delante del JSON y dice que la
 * respuesta no es JSON. Que es verdad: no lo es todavia. */
static int dechunk(char *p, u32 len)
{
	u32 in = 0, out = 0;

	for (;;) {
		u32 sz = 0;
		int digits = 0;

		/* tamano en hexadecimal, hasta CR o ';' (hay servidores que
		 * cuelgan extensiones detras del tamano) */
		while (in < len) {
			char c = p[in];
			int v;

			if (c >= '0' && c <= '9')      v = c - '0';
			else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
			else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
			else break;

			sz = sz * 16 + (u32)v;
			in++;
			digits++;
		}

		if (!digits) return -1;

		while (in < len && p[in] != '\n') in++;   /* resto de la linea */
		if (in >= len) return -1;
		in++;                                     /* el \n */

		if (sz == 0) break;                       /* trozo final */

		if (in > len || len - in < sz) return -1;

		memmove(p + out, p + in, sz);
		out += sz;
		in  += sz;

		/* CRLF de cierre del trozo */
		while (in < len && p[in] != '\n') in++;
		if (in < len) in++;
	}

	return (int)out;
}

/* Separa cabeceras de cuerpo y deja info.body_len valiendo lo que vale de
 * verdad: SOLO el cuerpo.
 *
 * Antes tlsBody() devolvia la respuesta entera, cabeceras incluidas. El
 * flujo del codigo de dispositivo funcionaba de milagro porque json_str
 * buscaba con strstr y encontraba el JSON igual, mas abajo. En cuanto
 * parse_body() empezo a usar cJSON_Parse de verdad, la misma respuesta
 * pasaba a ser "no es JSON": lo primero que ve es "HTTP/1.1 200 OK".
 *
 * Devuelve 0 si la respuesta tiene forma de respuesta HTTP. */
static int split_response(u32 n)
{
	int pos = find_bytes(sink, n, "\r\n\r\n", 4);
	const char *h;
	u32 hlen, blen;
	int status = 0;

	raw_len  = n;
	body_off = 0;

	/* Cabe: el bucle de lectura corta en sink_cap - 1, asi que la posicion
	 * n siempre esta dentro. atoi de mas abajo necesita el cero. */
	sink[n] = '\0';

	if (n > 12 && memcmp(sink, "HTTP/1.", 7) == 0)
		status = atoi(sink + 9);

	LOCK();
	info.http_status = status;
	UNLOCK();

	if (pos < 0) {
		/* Sin separador: o la respuesta viene cortada o no es HTTP. Se
		 * entrega tal cual y que el que llame decida; el codigo de
		 * estado ya dice si esto tiene sentido. */
		LOCK();
		info.body_len = n;
		UNLOCK();
		return -1;
	}

	h    = sink;
	hlen = (u32)pos;

	body_off = (u32)pos + 4;
	blen     = n - body_off;

	if (hdr_has(h, hlen, "transfer-encoding: chunked")) {
		int clean = dechunk(sink + body_off, blen);

		if (clean < 0) {
			tlog("cuerpo troceado mal formado, se deja crudo");
		} else {
			blen = (u32)clean;
			tlog("cuerpo troceado: %u bytes limpios", (unsigned)clean);
		}
	}

	/* Cabe siempre: el bucle de lectura nunca pasa de sink_cap - 1, y el
	 * cuerpo solo puede encoger al quitarle las cabeceras. */
	sink[body_off + blen] = '\0';

	LOCK();
	info.body_len = blen;
	UNLOCK();

	return 0;
}

static void do_request(void)
{
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config conf;
	struct sockaddr_in sa;
	const mbedtls_x509_crt *peer;
	u32 addr = 0, flags;
	u64 t0;
	s32 sock = -1;
	int ret, done = 0;
	u32 req_len = 0;

	/* 2 KB y no 512. El primer intento se quedo corto y el fallo fue
	 * silencioso de la peor manera: el cuerpo NO cabia, memcpy se
	 * saltaba, y se enviaba una cabecera con Content-Length prometiendo
	 * 700 bytes que nunca llegaban. El servidor se quedaba esperando
	 * educadamente 40 segundos. Ahora cabe, y si algun dia no cupiera,
	 * FALLA en vez de mentir. */
	static char req[16384];

	LOCK();
	memset(&info, 0, sizeof(info));
	info.state = TLS_RESOLVING;
	snprintf(info.host, sizeof(info.host), "%s", req_host);
	UNLOCK();

	/* El buffer de ESTA peticion, el que trajo quien la pidio, o el de
	 * serie. Lo pone tlsRequestTo con el turno ya cogido. */
	if (req_sink) {
		sink     = req_sink;
		sink_cap = req_sink_cap;
	} else {
		sink     = body;
		sink_cap = TLS_BODY_MAX;
	}

	sink[0]  = '\0';
	body_off = 0;
	raw_len  = 0;

	/* Y lo que ve el panel de pruebas, tambien. Solo se rellenaba al
	 * terminar bien, asi que cualquier salida por `goto out` -DNS,
	 * conexion, apreton de manos, plazo agotado- dejaba en pantalla el
	 * cuerpo de la peticion ANTERIOR como si fuera de esta. */
	LOCK();
	dbg_off = 0;
	dbg_len = 0;
	UNLOCK();

	/* El techo crece con el buffer. 25 s sobran para un JSON de 3 KB y se
	 * quedan cortos para bajarse un catalogo de varios megabytes por WiFi
	 * de 2007: a unos 5 Mbit medidos, 6 MB son diez segundos largos y
	 * cualquier bache los convierte en veinte.
	 *
	 * Sigue siendo un TECHO, no una espera: entre trozo y trozo se mira la
	 * bandera de abandono, asi que un numero grande no es un cuelgue
	 * grande. */
	deadline_us = now_us() + TLS_DEADLINE_US +
	              ((u64)(sink_cap / (128 * 1024)) * 1000000ull);

	mbedtls_ssl_init(&ssl);
	mbedtls_ssl_config_init(&conf);

	/* --- 1. nombre -> direccion --- */

	tlog("1/4 resolviendo %s...", req_host);
	t0 = now_us();

	ret = resolve(req_host, &addr);
	if (ret != 0) {
		fail("no se pudo resolver %s (%d)", req_host, ret);
		goto out;
	}

	LOCK();
	info.ms_resolve = (u32)((now_us() - t0) / 1000);
	snprintf(info.ip, sizeof(info.ip), "%u.%u.%u.%u",
	         (unsigned)((addr >> 24) & 0xff), (unsigned)((addr >> 16) & 0xff),
	         (unsigned)((addr >> 8) & 0xff),  (unsigned)(addr & 0xff));
	UNLOCK();

	tlog("1/4 ok: %s en %u ms", info.ip, (unsigned)info.ms_resolve);

	/* --- 2. socket --- */

	set_state(TLS_CONNECTING);
	tlog("2/4 conectando al 443...");
	t0 = now_us();

	sock = netSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock < 0) { fail("netSocket fallo (%d)", (int)sock); goto out; }

	memset(&sa, 0, sizeof(sa));
	sa.sin_len    = sizeof(sa);
	sa.sin_family = AF_INET;
	sa.sin_port   = htons(443);
	sa.sin_addr.s_addr = addr;

	if (netConnect(sock, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
		fail("netConnect fallo, errno %d", (int)net_errno);
		goto out;
	}

	LOCK();
	info.ms_connect = (u32)((now_us() - t0) / 1000);
	UNLOCK();
	tlog("2/4 ok en %u ms", (unsigned)info.ms_connect);

	/* --- 3. TLS --- */

	set_state(TLS_HANDSHAKE);
	tlog("3/4 negociando TLS...");
	t0 = now_us();

	ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
	                                  MBEDTLS_SSL_TRANSPORT_STREAM,
	                                  MBEDTLS_SSL_PRESET_DEFAULT);
	if (ret != 0) { fail_mbed("ssl_config_defaults", ret); goto out; }

	/* REQUIRED, no OPTIONAL. Un cliente que se conecta igual cuando el
	 * certificado no valida no esta usando TLS, esta usando un socket
	 * caro. Si falla, queremos que falle. */
	mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
	mbedtls_ssl_conf_ca_chain(&conf, &cacert, NULL);
	mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
	mbedtls_ssl_conf_dbg(&conf, dbg_cb, NULL);

	ret = mbedtls_ssl_setup(&ssl, &conf);
	if (ret != 0) { fail_mbed("ssl_setup", ret); goto out; }

	/* El nombre va en el saludo (SNI) y ademas se comprueba contra el
	 * certificado. Sin esto, cualquier certificado valido de cualquier
	 * sitio serviria. */
	ret = mbedtls_ssl_set_hostname(&ssl, req_host);
	if (ret != 0) { fail_mbed("set_hostname", ret); goto out; }

	mbedtls_ssl_set_bio(&ssl, (void*)(intptr_t)sock, bio_send, bio_recv, NULL);

	while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
		if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
		    ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
		if (tls_abort) { fail("cancelado"); goto out; }
		if (deadline_us && now_us() > deadline_us) {
			fail("el apreton de manos no termino en %u s",
			     (unsigned)(TLS_DEADLINE_US / 1000000ull));
			goto out;
		}
		fail_mbed("apreton de manos", ret);
		goto out;
	}

	LOCK();
	info.ms_handshake = (u32)((now_us() - t0) / 1000);
	snprintf(info.version, sizeof(info.version), "%s",
	         mbedtls_ssl_get_version(&ssl));
	snprintf(info.cipher, sizeof(info.cipher), "%s",
	         mbedtls_ssl_get_ciphersuite(&ssl));
	UNLOCK();

	flags = mbedtls_ssl_get_verify_result(&ssl);

	LOCK();
	info.verify_flags = flags;
	if (flags == 0) snprintf(info.verify_txt, sizeof(info.verify_txt),
	                         "cadena valida");
	else            mbedtls_x509_crt_verify_info(info.verify_txt,
	                                             sizeof(info.verify_txt),
	                                             "", flags);
	UNLOCK();

	peer = mbedtls_ssl_get_peer_cert(&ssl);
	if (peer) {
		LOCK();
		mbedtls_x509_dn_gets(info.peer, sizeof(info.peer), &peer->subject);
		UNLOCK();
	}

	tlog("3/4 ok en %u ms: %s / %s", (unsigned)info.ms_handshake,
	     info.version, info.cipher);
	tlog("3/4 certificado: %s", info.peer);

	/* --- 4. GET --- */

	set_state(TLS_REQUEST);
	t0 = now_us();

	{
		size_t blen = req_body ? strlen(req_body) : 0;
		int n;

		/* SNPRINTF DEVUELVE LO QUE HABRIA ESCRITO, NO LO QUE ESCRIBIO.
		 *
		 * Esto se encadenaba con `n += snprintf(req + n, sizeof(req) - n,
		 * ...)` y la comprobacion de que cabia estaba DETRAS de las cuatro
		 * llamadas. En cuanto la primera se pasaba, `n` era mayor que el
		 * buffer, `req + n` apuntaba fuera, y `sizeof(req) - n` daba la
		 * vuelta y se convertia en un numero enorme: escritura libre en
		 * memoria estatica, con un mensaje de error educado detras
		 * explicando que no cabia.
		 *
		 * Y era alcanzable: auth.c monta una cabecera de 20 KB con el XSTS
		 * dentro, que puede pasar de 16 KB el solo. La ruta de la sesion se
		 * salvaba de milagro porque su cabecera esta limitada a 9 KB.
		 *
		 * La regla, ahora explicita: DESPUES DE CADA TROZO se comprueba, y
		 * `n` nunca puede pasarse del buffer porque no se le deja. */
		#define PON(...) do { \
			int _k = snprintf(req + n, sizeof(req) - (size_t)n, __VA_ARGS__); \
			if (_k < 0 || (size_t)_k >= sizeof(req) - (size_t)n) { \
				fail("la peticion no cabe en %u bytes de cabecera", \
				     (unsigned)sizeof(req)); \
				goto out; \
			} \
			n += _k; \
		} while (0)

		n = 0;

		PON("%s %s HTTP/1.1\r\n"
		    "Host: %s\r\n"
		    "User-Agent: GR33N/" GR33N_VERSION " (PlayStation 3)\r\n"
		    "Accept: */*\r\n"
		    "Connection: close\r\n",
		    req_method, req_path, req_host);

		if (req_ctype && blen)
			PON("Content-Type: %s\r\n"
			    "Content-Length: %u\r\n",
			    req_ctype, (unsigned)blen);

		/* Cabeceras adicionales, ya con sus CRLF. Xbox Live no da un
		 * token sin Authorization ni x-xbl-contract-version, asi que
		 * esto no es un extra: sin ello no hay perfil. */
		if (req_hdrs && req_hdrs[0])
			PON("%s", req_hdrs);

		PON("\r\n");

		#undef PON

		if ((size_t)n + blen >= sizeof(req)) {
			fail("la peticion no cabe: %u cabecera + %u cuerpo > %u",
			     (unsigned)n, (unsigned)blen, (unsigned)sizeof(req));
			goto out;
		}

		if (blen) memcpy(req + n, req_body, blen);

		/* La longitud es la CALCULADA, no strlen(). Tras el memcpy el
		 * cuerpo no lleva nulo, asi que strlen leeria pila sin
		 * inicializar hasta encontrar un cero por casualidad. Que el
		 * primer GET funcionara fue justo eso: casualidad. */
		req_len = (u32)n + (u32)blen;
	}

	{
		size_t sent = 0, total = req_len;

		while (sent < total) {
			ret = mbedtls_ssl_write(&ssl,
			                        (const unsigned char*)req + sent,
			                        total - sent);
			if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
			    ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
				if (tls_abort) { fail("cancelado"); goto out; }
				if (deadline_us && now_us() > deadline_us) {
					fail("la respuesta no llego en %u s",
					     (unsigned)(TLS_DEADLINE_US / 1000000ull));
					goto out;
				}
				continue;
			}
			if (ret <= 0) { fail_mbed("ssl_write", ret); goto out; }
			sent += (size_t)ret;
		}
	}

	/* Se lee hasta que el servidor cierra. Connection: close nos ahorra
	 * interpretar Content-Length y trozos troceados, que para una prueba
	 * de transporte es complejidad que no aporta nada. */
	{
		u32 n = 0;
		int hdr_seen = 0;
		u32 announced = 0;

		for (;;) {
			ret = mbedtls_ssl_read(&ssl, (unsigned char*)sink + n,
			                       sink_cap - 1 - n);

			if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
			    ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
				if (tls_abort) { fail("cancelado"); goto out; }
				if (deadline_us && now_us() > deadline_us) {
					fail("la respuesta no llego en %u s",
					     (unsigned)(TLS_DEADLINE_US / 1000000ull));
					goto out;
				}
				continue;
			}
			/* Las tres formas de "ya no hay mas": cierre educado,
			 * cierre a secas, y cero bytes. Le hemos pedido
			 * Connection: close, asi que el final de la respuesta ES
			 * el cierre. Ninguna de las tres es un error. */
			if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) break;
			if (ret == MBEDTLS_ERR_SSL_CONN_EOF) break;
			if (ret == 0) break;
			if (ret < 0) { fail_mbed("ssl_read", ret); goto out; }

			n += (u32)ret;

			/* EN CUANTO ESTAN LAS CABECERAS, decir cuanto ha
			 * prometido el servidor.
			 *
			 * Las cabeceras llegan primero, siempre. Sacar de ahi el
			 * Content-Length convierte un "no cabe, mas de un mega"
			 * -que es una adivinanza- en "el servidor dijo 2.347.891
			 * bytes", que es el numero exacto que hace falta para
			 * dimensionar el buffer. Costaba una busqueda y nos
			 * habriamos ahorrado un viaje entero a la consola. */
			if (!hdr_seen) {
				int pos = find_bytes(sink, n, "\r\n\r\n", 4);

				if (pos >= 0) {
					const char *cl;

					hdr_seen = 1;
					sink[pos] = '\0';   /* se restaura abajo */

					cl = find_header(sink, "content-length:");
					if (cl) {
						announced = (u32)strtoul(cl, NULL, 10);
						tlog("el servidor anuncia %u bytes de cuerpo",
						     (unsigned)announced);
					} else {
						tlog("sin Content-Length (troceado o hasta cierre)");
					}

					sink[pos] = '\r';
				}
			}

			/* Llenar el buffer NO es "ya esta". Antes se salia del
			 * bucle en silencio y el que llamaba recibia media
			 * respuesta creyendola entera. Media respuesta es peor
			 * que ninguna: parece que funciona. */
			if (n >= sink_cap - 1) {
				if (announced)
					fail("la respuesta no cabe: el servidor anuncia %u bytes "
					     "y el buffer son %u",
					     (unsigned)announced, (unsigned)(sink_cap - 1));
				else
					fail("la respuesta no cabe: mas de %u bytes y sin "
					     "Content-Length",
					     (unsigned)(sink_cap - 1));
				goto out;
			}
		}

		/* Fuera del candado: split_response escribe en el registro y
		 * no se tiene un candado cogido mientras se hace printf. El
		 * hilo de dibujo lee tlsStatus() sesenta veces por segundo. */
		split_response(n);

		LOCK();
		info.ms_request = (u32)((now_us() - t0) / 1000);
		UNLOCK();
	}

	LOCK();
	if (sink == body) { dbg_off = body_off; dbg_len = info.body_len; }
	else              { dbg_off = 0;        dbg_len = 0; }
	UNLOCK();

	tlog("4/4 HTTP %d, %u bytes de cuerpo (%u en total) en %u ms",
	     info.http_status, (unsigned)info.body_len,
	     (unsigned)raw_len, (unsigned)info.ms_request);

	mbedtls_ssl_close_notify(&ssl);
	done = 1;

out:
	if (sock >= 0) netClose(sock);
	mbedtls_ssl_free(&ssl);
	mbedtls_ssl_config_free(&conf);

	if (done) set_state(TLS_DONE);
}

static void tls_thread(void *arg)
{
	(void)arg;

	while (tls_running) {
		if (req_go) {
			req_go = 0;
			do_request();
			release();
			continue;
		}
		usleep(5000);
	}

	sysThreadExit(0);
}

/* --------------------------------------------------------------------- */
/* API                                                                   */
/* --------------------------------------------------------------------- */

int tlsInit(void)
{
	sys_mutex_attr_t attr;
	mbedtls_time_t now;
	int ret;

	if (inited) return 0;

	memset(&info, 0, sizeof(info));
	info.state = TLS_IDLE;

	sysMutexAttrInitialize(attr);
	if (sysMutexCreate(&mtx, &attr) == 0) mtx_ok = 1;

	tb_hz = sysGetTimebaseFrequency();
	if (tb_hz == 0) tb_hz = 79800000ull;

	mbedtls_platform_set_time(ps3_time);

	/* Si el reloj de la consola esta en cualquier sitio, TODOS los
	 * certificados van a parecer invalidos y el mensaje de error no
	 * apuntara a la causa. Mejor decirlo aqui. */
	now = ps3_time(NULL);
	if ((u64)now < 1600000000ull) {
		tlog("!! el reloj de la consola dice %llu (antes de 2020). "
		     "La comprobacion de certificados va a fallar entera.",
		     (unsigned long long)now);
	} else {
		tlog("reloj %llu s desde epoch", (unsigned long long)now);
	}

	mbedtls_entropy_init(&entropy);
	mbedtls_ctr_drbg_init(&drbg);
	mbedtls_x509_crt_init(&cacert);

#if defined(MBEDTLS_DEBUG_C)
	mbedtls_debug_set_threshold(TLS_DEBUG_LEVEL);
#endif

	/* La cadena de personalizacion no es un adorno: separa el estado del
	 * generador del de cualquier otro cacharro que use la misma semilla. */
	ret = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
	                            (const unsigned char*)"GR33N-PS3", 9);
	if (ret != 0) { fail_mbed("ctr_drbg_seed", ret); return -1; }

	tlog("generador sembrado");

	/* sizeof incluye el nulo final, y mbedtls_x509_crt_parse lo EXIGE
	 * para PEM. Pasarle strlen da un error que no dice por que. */
	ret = mbedtls_x509_crt_parse(&cacert,
	                             (const unsigned char*)ca_bundle_pem,
	                             sizeof(ca_bundle_pem));
	if (ret < 0) { fail_mbed("x509_crt_parse", ret); return -1; }
	if (ret > 0) tlog("!! %d certificados del paquete no se pudieron leer", ret);

	{
		int n = 0;
		const mbedtls_x509_crt *c = &cacert;
		while (c) { n++; c = c->next; }
		tlog("%d certificados raiz cargados", n);
	}

	tls_running = 1;
	if (sysThreadCreate(&tls_tid, tls_thread, NULL, TLS_THREAD_PRIO,
	                    TLS_THREAD_STACK, THREAD_JOINABLE, "GR33N tls") != 0) {
		tls_running = 0;
		fail("sysThreadCreate del hilo TLS fallo");
		return -1;
	}
	tls_started = 1;

	inited = 1;
	return 0;
}

void tlsShutdown(void)
{
	/* Primero se avisa, luego se espera. Al reves es como se cuelga una
	 * aplicacion al salir. */
	tls_abort = 1;

	if (tls_started) {
		u64 rv = 0;
		tls_running = 0;
		sysThreadJoin(tls_tid, &rv);
		tls_started = 0;
	}

	if (inited) {
		mbedtls_x509_crt_free(&cacert);
		mbedtls_ctr_drbg_free(&drbg);
		mbedtls_entropy_free(&entropy);
		inited = 0;
	}

	if (mtx_ok) { sysMutexDestroy(mtx); mtx_ok = 0; }
}

/* Una peticion a la vez. No es una limitacion de diseno pensada: es que
 * el estado y el buffer de respuesta son unicos, y dos peticiones a la
 * vez se pisarian sin avisar. Con el flujo actual - o pruebas TLS, o
 * inicio de sesion, nunca los dos - sobra. */
static int claim(void)
{
	int ok;

	LOCK();
	ok = !busy;
	if (ok) busy = 1;
	UNLOCK();

	return ok;
}

static void release(void)
{
	LOCK();
	busy = 0;
	UNLOCK();
}

int tlsStartGet(const char *host, const char *path)
{
	if (!inited || !host || !path) return -1;
	if (!claim()) return -2;

	/* LA BANDERA DE ABANDONO SE BAJA AQUI, igual que en tlsRequestTo.
	 *
	 * Faltaba, y se notaba sin necesidad de ninguna carrera: entras en
	 * Iniciar sesion y sales -authCancel llama a tlsAbort- y despues
	 * entras en la prueba de TLS. El primer bio_recv ve tls_abort a 1 y
	 * la pantalla dice "cancelado" sin haber intentado nada. */
	tls_abort = 0;

	snprintf(req_host, sizeof(req_host), "%s", host);
	snprintf(req_path, sizeof(req_path), "%s", path);
	req_method = "GET";
	req_ctype  = NULL;
	req_body   = NULL;
	req_hdrs   = NULL;

	/* El buffer de serie, y dicho aqui en vez de por omision: es la unica
	 * invariante que sostiene todo el arreglo del sink. */
	req_sink     = NULL;
	req_sink_cap = 0;

	/* req_go se publica DESPUES de los parametros, y con el candado, que
	 * es lo unico que ordena de verdad las escrituras entre los dos hilos
	 * del PPU. volatile no ordena nada en PowerPC: el otro hilo podia ver
	 * req_go a 1 con req_host a medio escribir, o con el metodo y el
	 * cuerpo de la peticion anterior. */
	LOCK();
	req_go = 1;
	UNLOCK();

	return 0;
}

/* BLOQUEA. Solo desde un hilo de trabajo, nunca desde el de dibujo.
 *
 * Cada llamada abre un socket y negocia TLS de cero: no se reutiliza la
 * conexion. Son unos 340 ms por peticion, que para sondear cada cinco
 * segundos es perfectamente asumible y ahorra toda la maquinaria de
 * mantener una conexion viva. Cuando haga falta, se cambia. */
int tlsRequestTo(const char *host, const char *path, const char *method,
                 const char *ctype, const char *reqbody, const char *headers,
                 void *sink_buf, u32 sink_buf_cap, tlsResult *out)
{
	int ok;

	/* El motivo se deja escrito ANTES de cada salida, no uno generico al
	 * principio: "sin respuesta de X: sin intentar" para una ruta que no
	 * cabe es un mensaje que manda a mirar la red cuando el fallo esta en
	 * una constante del programa. */
	if (out) memset(out, 0, sizeof(*out));

	if (!inited || !host || !path) {
		if (out) snprintf(out->err, sizeof(out->err),
		                  "TLS sin arrancar o peticion vacia");
		return -1;
	}

	/* Se comprueba ANTES de coger el turno. Una ruta cortada es una
	 * peticion a otro sitio, y "a otro sitio" contesta cosas raras que
	 * luego cuesta media hora entender. Ya van tres cortes silenciosos en
	 * este proyecto; este no. */
	if (strlen(host) >= sizeof(req_host) || strlen(path) >= sizeof(req_path)) {
		if (out) snprintf(out->err, sizeof(out->err),
		                  "el servidor o la ruta no caben (%u + %u)",
		                  (unsigned)strlen(host), (unsigned)strlen(path));
		return -3;
	}

	/* EL BUFFER SE PONE DESPUES DEL TURNO, y esa es toda la diferencia.
	 * Si no hay turno se vuelve sin haber tocado nada global, asi que
	 * quien llama puede liberar su buffer tranquilo. */
	if (!claim()) {
		if (out) snprintf(out->err, sizeof(out->err),
		                  "el modulo esta ocupado");
		return -2;
	}

	req_sink     = (sink_buf && sink_buf_cap >= 64) ? (char*)sink_buf : NULL;
	req_sink_cap = req_sink ? sink_buf_cap : 0;

	tls_abort = 0;
	snprintf(req_host, sizeof(req_host), "%s", host);
	snprintf(req_path, sizeof(req_path), "%s", path);
	req_method = method ? method : "GET";
	req_ctype  = ctype;
	req_body   = reqbody;
	req_hdrs   = headers;

	do_request();

	ok = (info.state == TLS_DONE);

	/* EL RESULTADO SE COPIA AQUI, con el turno todavia cogido. Un
	 * milisegundo despues esto ya puede ser de otro. */
	if (out) {
		out->ok          = ok;
		out->http_status = info.http_status;
		out->len         = info.body_len;
		out->body        = (ok && info.body_len) ? (sink + body_off) : NULL;
		snprintf(out->err, sizeof(out->err), "%s",
		         info.err[0] ? info.err : (ok ? "" : "(sin motivo)"));
	}

	/* Y se suelta ANTES que el turno: quien venga detras no puede heredar
	 * un puntero que no es suyo. */
	req_sink     = NULL;
	req_sink_cap = 0;

	release();

	return ok ? 0 : -1;
}

/* ¿Hay una peticion en marcha?
 *
 * Para no montar los preparativos -un malloc de dos megabytes en el caso
 * de la tienda- cuando ya se sabe que va a salir -2. Es una foto: entre
 * preguntar y pedir, el otro hilo puede coger el turno igual, asi que el
 * que llama tiene que seguir tratando el -2. Esto solo evita el trabajo
 * tonto en el caso comun. */
int tlsBusy(void)
{
	int b;

	LOCK();
	b = busy;
	UNLOCK();

	return b;
}

/* PRIORIDAD, porque "libre" no es lo mismo que "para ti".
 *
 * MEDIDO: al pedir una sesion de juego, el hilo de la sesion pedia turno
 * cada 200 ms y NUNCA lo conseguia. El barrido del catalogo encadena lotes
 * de tienda de dos segundos y vuelve a coger el turno en cuanto suelta el
 * anterior, asi que entre lote y lote no queda ni una rendija. Cientos de
 * "POST .../play" en el log sin una sola peticion enviada.
 *
 * Turnarse no basta cuando uno de los dos vuelve a la cola al instante. Lo
 * que hace falta es que el trabajo de fondo se aparte del todo mientras
 * haya algo urgente, y eso no lo puede decidir el que espera: lo dice el
 * urgente al coger la prioridad.
 *
 * NO afecta a claim(): quien tiene la prioridad pide el turno como todo el
 * mundo. Lo que hace es que los de fondo, que consultan tlsHeld() antes de
 * montar nada, se aparten solos. */
/* ES UNA CUENTA, NO UN INTERRUPTOR.
 *
 * Con un booleano, cualquier funcion que cogiera la prioridad por su cuenta
 * la soltaba entera al terminar, aunque quien la llamo la siguiera
 * queriendo. Justo lo que pasa con step_delete, que se llama desde dentro
 * de una sesion (que ya tiene prioridad) y tambien desde el hilo al salir
 * (que no la tiene): con un interruptor, la primera forma se quedaba sin
 * prioridad a mitad.
 *
 * Contando, anidar sale bien solo: cada quien pide la suya y la suelta, y
 * el trabajo de fondo se aparta mientras quede alguna viva. */
static volatile int hold = 0;

void tlsHold(int on)
{
	LOCK();
	if (on) {
		hold++;
	} else if (hold > 0) {
		hold--;
	}
	UNLOCK();
}

int tlsHeld(void)
{
	int h;

	LOCK();
	h = (hold > 0);
	UNLOCK();

	return h;
}

/* Corta la peticion en curso. La ve bio_recv en la siguiente vuelta de
 * netPoll, o sea como mucho 200 ms despues. */
void tlsAbort(void)
{
	tls_abort = 1;
}

const tlsInfo *tlsStatus(void)
{
	static tlsInfo snap;

	LOCK();
	snap = info;
	UNLOCK();

	return &snap;
}

/* Parte https://servidor/lo/que/sea en sus dos trozos.
 *
 * No es un analizador de URL completo: es lo justo para lo que devuelven
 * los servicios de Xbox. Vive aqui y no en cada modulo porque la necesitan
 * el avatar y las caratulas, y dos copias de esto es como acaban
 * divergiendo en el detalle que importa.
 *
 *   0  bien       -1  esquema desconocido
 *  -2  con puerto (no soportado)   -3  la ruta no cabe */
int tlsSplitUrl(const char *url, char *host, u32 hmax, char *path, u32 pmax)
{
	const char *p = url;
	const char *slash;
	u32 hlen;

	if (!url || !host || !path || hmax < 2 || pmax < 2) return -1;

	if (strncmp(p, "https://", 8) == 0)      p += 8;
	else if (strncmp(p, "http://", 7) == 0)  p += 7;   /* se sube a TLS */
	else if (strncmp(p, "//", 2) == 0)       p += 2;   /* sin esquema */
	else return -1;

	slash = strchr(p, '/');
	hlen  = slash ? (u32)(slash - p) : (u32)strlen(p);

	if (hlen == 0 || hlen >= hmax) return -1;
	if (memchr(p, ':', hlen))      return -2;

	memcpy(host, p, hlen);
	host[hlen] = '\0';

	if (!slash) { snprintf(path, pmax, "/"); return 0; }
	if (strlen(slash) >= pmax) return -3;

	snprintf(path, pmax, "%s", slash);
	return 0;
}

/* SOLO el cuerpo. Las cabeceras se quedan delante, en el mismo buffer,
 * pero fuera de lo que se entrega. */
/* Para el panel de pruebas y poco mas. Quien pida con su propio buffer
 * recoge el cuerpo del tlsResult, que se copia con el turno cogido. */
const char *tlsBody(u32 *len)
{
	u32 off, n;

	/* Con una peticion en marcha se devuelve NULL. body[] se esta
	 * reescribiendo justo ahora, y el hilo de dibujo lee esto sesenta
	 * veces por segundo: sin esto, el panel de pruebas pinta el JSON de
	 * antes mezclado con la respuesta de ahora. */
	LOCK();
	off = busy ? 0 : dbg_off;
	n   = busy ? 0 : dbg_len;
	UNLOCK();

	if (len) *len = n;
	return n ? body + off : NULL;
}


