/* GR33N - capa de mando
 *
 * ioPadGetData() solo rellena la estructura cuando HAY CAMBIO respecto a la
 * llamada anterior; si no, devuelve len==0 y ceros. Leerlo directo en el
 * bucle da un mando que "se suelta solo". Esta capa mantiene el ultimo
 * estado valido y saca flancos de pulsacion.
 */

#ifndef GR33N_INPUT_H
#define GR33N_INPUT_H

#include <ppu-types.h>

#define GR33N_BTN_SELECT    (1u<<0)
#define GR33N_BTN_L3        (1u<<1)
#define GR33N_BTN_R3        (1u<<2)
#define GR33N_BTN_START     (1u<<3)
#define GR33N_BTN_UP        (1u<<4)
#define GR33N_BTN_RIGHT     (1u<<5)
#define GR33N_BTN_DOWN      (1u<<6)
#define GR33N_BTN_LEFT      (1u<<7)
#define GR33N_BTN_L2        (1u<<8)
#define GR33N_BTN_R2        (1u<<9)
#define GR33N_BTN_L1        (1u<<10)
#define GR33N_BTN_R1        (1u<<11)
#define GR33N_BTN_TRIANGLE  (1u<<12)
#define GR33N_BTN_CIRCLE    (1u<<13)
#define GR33N_BTN_CROSS     (1u<<14)
#define GR33N_BTN_SQUARE    (1u<<15)

#define GR33N_BTN_COUNT     16

typedef struct {
	int connected;
	int port;

	u32 held;      /* mascara de botones mantenidos */
	u32 pressed;   /* flanco: pulsados en este frame */
	u32 released;  /* flanco: soltados en este frame */

	s32 lx, ly;    /* stick izquierdo, -128..127 (arriba = negativo) */
	s32 rx, ry;    /* stick derecho */

	/* LOS GATILLOS, ANALOGICOS DE VERDAD: 0..255.
	 *
	 * El DS3 mide presion en L2 y R2 y siempre la ha mandado; lo que
	 * pasaba es que aqui solo se miraba el bit de "pulsado", asi que al
	 * juego le llegaba 0 o a tope y nada en medio. Para disparar da
	 * igual; para acelerar en una curva, no.
	 *
	 * OJO: en un mando que no mida presion --un Sixaxis viejo, un
	 * adaptador raro-- estos salen 0 aunque el boton este pulsado. Por eso
	 * held sigue teniendo GR33N_BTN_L2/R2: es el respaldo, y quien los use
	 * tiene que mirar los dos. Ver eje_gatillo() en webrtc.c. */
	s32 lt, rt;
} gr33nPad;

int  inputInit(void);
void inputShutdown(void);
void inputPoll(void);

const gr33nPad *inputPad(void);

/* Nombre corto de un bit de boton, para el HUD. */
const char *inputButtonName(int bit);

#endif /* GR33N_INPUT_H */
