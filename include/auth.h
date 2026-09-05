/* GR33N - inicio de sesion en la cuenta de Microsoft
 *
 * Flujo de CODIGO DE DISPOSITIVO, que es el que usan los clientes de
 * salon: la consola pide un codigo corto, lo enseña en la tele, y el
 * usuario lo escribe en el movil. Mientras tanto la consola pregunta cada
 * pocos segundos "¿ya?" hasta que le dan un token.
 *
 * Es la unica forma sensata aqui: no hay teclado, y pedirle a alguien que
 * escriba una contrasena con el mando es una forma de perder usuarios.
 * Y de paso, la contrasena nunca pasa por la consola.
 *
 * Lo que se guarda al terminar es el REFRESH TOKEN, no la contrasena ni
 * el token de acceso. Sirve para pedir tokens nuevos sin repetir el baile,
 * caduca, y se puede revocar desde la cuenta.
 */

#ifndef GR33N_AUTH_H
#define GR33N_AUTH_H

#include <ppu-types.h>

typedef enum {
	AUTH_IDLE = 0,
	AUTH_REQUESTING,   /* pidiendo el codigo                     */
	AUTH_WAITING,      /* codigo en pantalla, sondeando          */
	AUTH_OK,
	AUTH_FAILED
} authState;

typedef struct {
	authState state;

	char user_code[32];     /* lo que el usuario teclea en el movil  */
	char verify_uri[96];    /* donde lo teclea                       */

	u32  interval_s;        /* cada cuanto sondear, lo dice el servidor */
	u32  expires_s;         /* cuanto vale el codigo                 */
	u32  elapsed_s;
	u32  polls;

	int  http_status;       /* del ultimo sondeo, para diagnosticar  */
	char err[224];   /* los mensajes AADSTS son largos y valen la pena */
} authInfo;

/* --------------------------------------------------------------------- */
/* Perfil de Xbox Live                                                   */
/*                                                                       */
/* Sacarlo NO es un adorno para la barra superior. Hay que recorrer una   */
/* cadena de cuatro peticiones, y el token del ultimo paso es el MISMO    */
/* que abre una sesion de xCloud: solo cambia a quien se lo pides. Asi    */
/* que el gamertag es la forma barata de comprobar que la cadena entera   */
/* funciona antes de jugarnosla con el streaming.                        */
/*                                                                       */
/*   refresh_token                                                       */
/*      -> access_token   (Microsoft)                                    */
/*      -> user_token     (Xbox Live)                                    */
/*      -> XSTS + uhs     (autorizacion para un servicio concreto)       */
/*      -> perfil                                                        */
/* --------------------------------------------------------------------- */

typedef enum {
	PROF_NONE = 0,
	PROF_WORKING,
	PROF_OK,
	PROF_FAILED
} profState;

/* El avatar se guarda ya reducido a este tamano, que es EXACTAMENTE el del
 * hueco de la barra superior. Dos motivos:
 *
 *   - Se promedia una sola vez, al descargar. Guardar los 424x424 que manda
 *     Xbox para reducirlos en cada fotograma seria pagar el escalado
 *     sesenta veces por segundo.
 *   - Al coincidir con el hueco, gfxBlit copia uno a uno y no vuelve a
 *     remuestrear. Un segundo escalado por vecino mas cercano encima de uno
 *     bueno se nota: deja la cara con los bordes rotos.
 *
 * Si algun dia hace falta el avatar mas grande, se sube este numero y
 * gfxBlit se encarga del resto. */
#define AVATAR_PX  44

typedef struct {
	profState state;

	char gamertag[24];
	u32  gamerscore;

	/* La URL trae otra URL dentro, escapada, asi que es larga de verdad.
	 * 256 se quedaba corto y snprintf habria cortado en silencio. */
	char pic_url[512];

	/* El avatar va aparte del estado de la cadena: el perfil puede estar
	 * bien y la imagen no haber llegado todavia, o no llegar nunca. Un
	 * gamertag sin foto se pinta perfectamente. */
	int  pic_ready;

	/* 160, que es lo que mide tlsResult.err. Estaba en 128 y ppu-gcc lo
	 * canto con un -Wformat-truncation en cuanto empezo a copiarse ahi el
	 * motivo entero de la peticion.
	 *
	 * Es la tercera vez en este proyecto que un buffer de mensaje se queda
	 * corto, y en las tres el aviso llega ANTES del final del texto: se
	 * pierde justo la parte que dice por que. Se dimensiona por el origen,
	 * no a ojo. */
	char pic_err[160];

	/* 224, igual que authInfo.err, no 160. Aqui se copia tal cual el
	 * motivo del fallo de la cadena, y esos mensajes son largos a
	 * proposito: el XErr y la explicacion de Xbox van al final, que es
	 * justo lo que cortaba. Recortar el error a la mitad para ahorrar 64
	 * bytes es tirar la unica pista que hay. */
	char err[224];
	u32  ms_total;       /* lo que costo la cadena entera */
} xblProfile;

/* --------------------------------------------------------------------- */
/* xCloud                                                                */
/*                                                                       */
/* El mismo user_token, pero pidiendo el XSTS para gssv.xboxlive.com en   */
/* vez de para xboxlive.com, y despues presentandolo en la puerta de      */
/* entrada del servicio de streaming.                                    */
/*                                                                       */
/* LO QUE ESTO COMPRUEBA, que es el motivo de que exista antes que nada   */
/* del video: si Microsoft da un token de streaming a esta cuenta con la  */
/* cadena que ya tenemos, o si exige el baile completo de SISU (token de  */
/* dispositivo, token de titulo y peticiones firmadas con una clave       */
/* P-256). Las dos respuestas valen; lo que no vale es seguir sin saberlo */
/* y descubrirlo con medio cliente de WebRTC escrito.                     */
/* --------------------------------------------------------------------- */

typedef enum {
	XC_NONE = 0,
	XC_WORKING,
	XC_OK,
	XC_FAILED
} xcloudState;

typedef struct {
	xcloudState state;

	char region[64];      /* la ELEGIDA ahora mismo        */
	char base_uri[192];   /* con quien se habla a partir de ahora */
	u32  regions_n;
	u32  expires_s;       /* lo que dura el gsToken        */
	u32  token_len;

	char err[224];
	u32  ms_total;
} xcloudInfo;

const xcloudInfo *authXCloud(void);

/* --------------------------------------------------------------------- */
/* Las regiones                                                          */
/*                                                                       */
/* Microsoft las manda TODAS en la respuesta del login, dentro de         */
/* offeringSettings.regions, y marca una con isDefault segun de donde     */
/* vengas. Hasta ahora solo se guardaba esa y las demas se escribian en   */
/* el log y se tiraban, que es tanto como no tenerlas.                    */
/*                                                                       */
/* Se guardan las N para dos cosas: medirles la latencia (ping.c) y       */
/* dejar elegir. El limite es de sobra -- Microsoft publica alrededor de  */
/* una docena -- y si algun dia manda mas, sobran las de mas y se dice    */
/* en el log en vez de pisar memoria.                                    */
/* --------------------------------------------------------------------- */

#define XC_REGIONS_MAX 20

typedef struct {
	char name[64];      /* como lo dice Microsoft: "WestEurope"      */
	char base_uri[192]; /* "https://weu.gssv-play-prod.xboxlive.com" */
	char host[128];     /* el host suelto, que es lo que se mide     */
	int  is_default;    /* la que eligio Microsoft por geolocalizar  */
} xcRegion;

u32 authRegionsN(void);

/* Copia la region i. Devuelve 0, o negativo si i esta fuera. SE COPIA
 * porque el hilo de autenticacion puede reescribir la lista entera al
 * renovar el token, y el del ping la esta recorriendo. */
int authRegionCopy(int i, xcRegion *out);

/* El indice de la marcada por defecto, o 0 si no hay ninguna. */
int authRegionDefault(void);

/* El indice de la que se esta usando, o -1 si todavia no hay ninguna. */
int authRegionActual(void);

/* Busca por nombre exacto. -1 si no esta. Es lo que usa la interfaz para
 * restaurar el ajuste guardado: guardar el INDICE seria elegir otra
 * region distinta el dia que Microsoft cambie el orden de la lista. */
int authRegionPorNombre(const char *name);

/* Cambia a que region se juega: escribe region[] y base_uri[]. Devuelve 0,
 * o negativo si el indice no vale.
 *
 * NO AFECTA A UNA SESION YA ABIERTA, y a proposito: session.c fotografia
 * el base_uri al arrancar y hace hasta el DELETE final contra esa foto.
 * Sin eso, cambiar de region con una partida viva mandaria el borrado al
 * sitio equivocado y la maquina se quedaria reservada. */
int authSetRegion(int i);

/* El gsToken, para las peticiones a los servidores de region.
 *
 * SE COPIA, no se presta: lo leen el hilo del catalogo y el de la sesion de
 * juego, y lo reescribe el de autenticacion cada vez que renueva. Devuelve
 * la longitud copiada, o 0 si no hay token o no cabe en `max`. */
u32 authGsTokenCopy(char *out, u32 max);

/* Arranca la cadena. No bloquea. Sin token guardado no hace nada. */
int  authFetchProfile(void);

const xblProfile *authProfile(void);

/* El avatar ya decodificado, en ARGB de AVATAR_PX de lado. NULL mientras no
 * este listo. El puntero no cambia en toda la sesion. */
const u32 *authAvatar(void);

int  authInit(void);
void authShutdown(void);

/* Arranca el flujo. No bloquea. */
int  authStart(void);

/* Corta un flujo en marcha. */
void authCancel(void);

const authInfo *authStatus(void);

/* 1 si hay un refresh token guardado de una sesion anterior. NO dice si
 * sigue siendo valido: eso solo lo sabe el servidor. */
int  authHaveToken(void);

/* Olvida el token guardado. */
void authForget(void);

/* El token de traspaso que pide POST {sessionPath}/connect.
 *
 * NO es el gsToken ni el XSTS: es un token MSA con permiso de "traspaso de
 * consola en la nube", que emite el portal antiguo de Passport a cambio del
 * mismo refresh token que ya tenemos. Ver el comentario largo en auth.c.
 *
 * `sink` es el buffer donde cae la respuesta HTTP y lo pone QUIEN LLAMA,
 * por la misma razon que en tlsRequestTo: esto se llama desde el hilo de la
 * sesion de juego y los buffers de auth.c son de otro hilo.
 *
 * Devuelve 0, -1 con el motivo en `err`, o -2 si el modulo TLS esta ocupado
 * -que no es fallo: hay que reintentar-. */
int authPassportToken(char *out, u32 max, void *sink, u32 sink_cap,
                      char *err, u32 err_max);

#endif /* GR33N_AUTH_H */
