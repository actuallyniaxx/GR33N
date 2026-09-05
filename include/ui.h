/* GR33N - interfaz
 *
 * Pestanas arriba, lista a la izquierda, detalle a la derecha. La forma
 * viene de green-vita, que a su vez la copia del cliente de xCloud.
 *
 * La UI es duena de la maquina de estados: o estas navegando (UI_MENU) o
 * hay un titulo en marcha a pantalla completa (UI_STREAM). main.c
 * pregunta en que modo esta y pinta una cosa u otra.
 */

#ifndef GR33N_UI_H
#define GR33N_UI_H

#include <ppu-types.h>
#include "video.h"
#include "input.h"

typedef enum {
	UI_MENU = 0,
	UI_STREAM
} uiMode;

typedef enum {
	TITLE_TESTCARD = 0,  /* generado en la propia consola */
	TITLE_H264,          /* clip H.264 empotrado, decodificado por cellVdec */
	TITLE_TLS,           /* peticion HTTPS real contra Microsoft */
	TITLE_AUTH,          /* inicio de sesion por codigo de dispositivo */
	TITLE_WEBRTC,        /* despertar libpeer y cronometrarlo */
	TITLE_REMOTE         /* lo que llegue del servidor */
} uiTitleKind;

typedef struct {
	const char *id;
	const char *name;
	const char *publisher;
	const char *desc;

	/* La version en ingles, cuando el texto es NUESTRO.
	 *
	 * Las entradas del catalogo dejan estos tres en NULL a proposito: su
	 * texto viene de Microsoft y YA llega en el idioma que pedimos en la
	 * peticion a la tienda. Traducirlo aqui seria traducir dos veces, y
	 * peor. tr() con NULL devuelve el original, que es justo lo que hace
	 * falta. */
	const char *name_en;
	const char *publisher_en;
	const char *desc_en;

	uiTitleKind kind;
	u32 accent;
} uiTitle;

void uiInit(void);

/* Navegacion y toggles. Una vez por frame, antes de dibujar. */
void uiUpdate(const gr33nPad *pad);

/* Pinta el menu completo. Solo tiene sentido en UI_MENU. */
void uiDraw(gr33nSurface *s);

/* Aviso de "SELECT + O para volver" al entrar en un titulo. Lo pinta
 * main.c encima del stream, y se apaga solo a los pocos segundos. */
void uiDrawStreamHint(gr33nSurface *s);

uiMode uiGetMode(void);

/* Titulo en marcha, o NULL si estamos en el menu. */
const uiTitle *uiActiveTitle(void);

/* Ajuste Debug. Cuando esta activo: se pinta el HUD de estadisticas y
 * aparecen los titulos de prueba en la biblioteca. */
int uiDebug(void);

/* Umbral por debajo del cual el stick se considera centrado. Un DS3 con
 * anos no vuelve a cero solo; el usuario lo calibra en Ajustes viendo la
 * lectura en vivo. */
int uiDeadzone(void);

/* Aceptar y cancelar, ya resuelto el intercambio X/O. Usalo en vez de
 * GR33N_BTN_CROSS a pelo, o el ajuste no servira de nada. */
u32 uiBtnOk(void);
u32 uiBtnBack(void);

/* Abre la pantalla de cuenta directamente. Para el primer arranque. */
void uiOpenAuth(void);

/* 1 si no habia fichero de ajustes al arrancar. */
int  uiFirstRun(void);

/* Perfil de Xbox Live para la barra superior. Con gamertag vacio o NULL
 * se deja de pintar. La foto vendra despues: hace falta decodificar PNG y
 * eso es otra conversacion. */
void uiSetProfile(const char *gamertag, u32 gamerscore);

/* Avatar ya decodificado a ARGB. El buffer tiene que seguir vivo: la
 * interfaz guarda el puntero, no una copia. */
void uiSetProfilePic(const u32 *argb, u32 w, u32 h);

/* Ajustes del panel de estadisticas: escala 1..3 y esquina 0..3. */
int  uiHudScale(void);
int  uiHudPos(void);

/* Region de xCloud elegida, ya en castellano si se conoce el nombre.
 * "Automatico" quiere decir la que Microsoft marca por defecto segun de
 * donde vengas, no la mas rapida de las medidas: eso ultimo seria decidir
 * por el usuario y ademas cambiaria solo entre partida y partida. */
const char *uiServerName(void);

/* Ajustes en disco. Cargar una vez al arrancar, guardar al salir (y la
 * interfaz guarda sola al abandonar la pestana). */
/* Hay un juego en vivo a pantalla completa.
 *
 * Con esto puesto la interfaz deja de tocar el mando: cualquier boton es
 * del juego, y solo la combinacion elegida en Ajustes sale. Sin esto, la
 * ficha del juego sigue debajo y Atras la cierra -- o sea que un boton
 * suelto sacaba de la partida. */
void uiSetLive(int on);

/* 1 UNA sola vez, cuando el jugador ha hecho la combinacion de salida.
 * Se limpia al leerlo, asi que quien lo lea es quien tiene que parar la
 * sesion. */
int  uiLiveWantsExit(void);

/* La combinacion que cierra la aplicacion, o 0 si el usuario la ha
 * desactivado en Ajustes. */
u32  uiQuitMask(void);

void uiLoadSettings(void);
void uiSaveSettings(void);
int  uiSettingsDirty(void);

#endif /* GR33N_UI_H */
