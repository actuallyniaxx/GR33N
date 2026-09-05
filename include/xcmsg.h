/* GR33N - los mensajes que van por los canales de datos de xCloud.
 *
 * Conectar no basta: xCloud no emite nada hasta que el cliente abre sus
 * canales y le cuenta quien es. Esta es esa conversacion, y toda ella es
 * construir cadenas y empaquetar bytes -- ni red ni consola-- asi que va
 * en su fichero y se prueba en el PC, como teredo.c.
 *
 * DOS FORMATOS DISTINTOS, no confundirlos:
 *
 *   - Los canales "control" y "message" llevan JSON en texto.
 *   - El canal "input" lleva BINARIO, y en LITTLE-ENDIAN.
 *
 * Y ahi esta la trampa gorda de este fichero. La referencia (green-nx) lo
 * serializa con memcpy sobre la variable y lo dice en un comentario:
 *
 *     std::memcpy(raw, &value, sizeof(T));  // little-endian hosts only
 *
 * La Switch es ARM64 little-endian y no lo nota. La PS3 es big-endian: un
 * memcpy de un uint16_t saldria al reves y el servidor leeria un tipo de
 * informe que no existe. Aqui los bytes se colocan a mano, de menos a mas
 * significativo, que es correcto en cualquier maquina.
 *
 * Con una excepcion de verdad: el ultimo campo del informe de mando son
 * cuatro bytes 00 00 00 01 en BIG-endian, tal cual, tambien en el cliente
 * de referencia. No es un descuido nuestro: es que ese campo va asi.
 */

#ifndef GR33N_XCMSG_H
#define GR33N_XCMSG_H

#include <stddef.h>
#include <ppu-types.h>

/* --- canal "message" y "control": JSON en texto ---------------------- */

/* Constantes: se devuelve el literal, no hay que copiar nada. */
const char *xcHandshake(void);            /* type Handshake, messageV1     */
const char *xcAuthorizationRequest(void); /* la accessKey del cliente web  */
const char *xcKeyframeRequested(void);    /* pide un fotograma clave       */

/* 1 si el texto es el HandshakeAck que abre la secuencia de arranque. */
int xcIsHandshakeAck(const char *payload, size_t len);

/* Escriben en `out` y devuelven la longitud, o 0 si no cabe. */
size_t xcGamepadChanged(char *out, size_t n, int indice, int anadido);
size_t xcResolution(char *out, size_t n, const char *alias);

/* Los seis mensajes de arranque, envueltos como los manda el cliente web:
 * un objeto "Message" cuyo campo `content` es OTRO objeto JSON convertido
 * a cadena. Doble codificacion, igual que en /ice.
 *
 * `idx` va de 0 a XC_STARTUP_N-1. Devuelve la longitud, o 0. */
#define XC_STARTUP_N 6
size_t xcStartup(char *out, size_t n, int idx,
                 int ancho, int alto, int kbps, int fps);

/* --- canal "input": binario, little-endian --------------------------- */

/* El estado de un mando tal como lo entiende xCloud. Los ejes van de
 * -1..1 y los gatillos de 0..1, en milesimas para no meter coma flotante
 * en un camino que se recorre sesenta veces por segundo. */
typedef struct {
	u16 botones;      /* la mascara de xcBtn* */
	s32 lx, ly;       /* -1000..1000 */
	s32 rx, ry;
	s32 lt, rt;       /* 0..1000 */
	u8  indice;       /* que mando */
} xcPad;

/* La mascara, tal cual la espera el servidor. */
#define XC_BTN_NEXUS     2
#define XC_BTN_MENU      4
#define XC_BTN_VIEW      8
#define XC_BTN_A        16
#define XC_BTN_B        32
#define XC_BTN_X        64
#define XC_BTN_Y       128
#define XC_BTN_UP      256
#define XC_BTN_DOWN    512
#define XC_BTN_LEFT   1024
#define XC_BTN_RIGHT  2048
#define XC_BTN_LB     4096
#define XC_BTN_RB     8192
#define XC_BTN_LS    16384
#define XC_BTN_RS    32768

/* Los dos informes. `sec` es el numero de secuencia, que va SIEMPRE hacia
 * arriba y sin huecos: el servidor deja de aplicar entrada en cuanto ve
 * uno saltado. Devuelven los bytes escritos, o 0 si no caben. */
#define XC_METADATA_LEN 15
#define XC_GAMEPAD_LEN  38

size_t xcInputMetadata(u8 *out, size_t n, u32 sec, u8 max_toques);
size_t xcInputGamepad(u8 *out, size_t n, u32 sec, double ms,
                      const xcPad *pad);

#endif /* GR33N_XCMSG_H */
