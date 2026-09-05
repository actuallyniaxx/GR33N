/* GR33N - capa de video
 *
 * Envuelve RSX/gcm en la minima API que necesita un cliente de streaming:
 *
 *     videoSurface()  -> te da un buffer ARGB donde escribir el frame
 *     videoPresent()  -> lo sube a pantalla como quad a pantalla completa
 *
 * Esa es exactamente la forma que tendra el path de video cuando el frame
 * lo produzca cellVdec en vez de un bucle de CPU. Nada de esto hay que
 * reescribirlo luego.
 */

#ifndef GR33N_VIDEO_H
#define GR33N_VIDEO_H

#include <ppu-types.h>
#include <rsx/rsx.h>

typedef struct {
	u32 *pixels;   /* ARGB8888, en memoria local del RSX */
	u32  width;
	u32  height;
	u32  pitch;    /* en BYTES, no en pixeles */
} gr33nSurface;

/* Contexto gcm global. Lo exponemos porque el callback de salida del XMB
 * tiene que poder pararlo desde fuera. */
extern gcmContextData *rsxctx;

int  videoInit(void);
void videoShutdown(void);

/* Devuelve la superficie de trabajo. BLOQUEA hasta que el RSX ha terminado
 * de leer el frame anterior: sin esta espera la CPU pisa la textura que el
 * RSX aun esta muestreando y salen artefactos que parecen "tearing" pero no
 * lo son. */
gr33nSurface *videoSurface(void);

/* Sube la superficie, dibuja el quad y hace flip con vsync. */
void videoPresent(void);

u32 videoDisplayWidth(void);
u32 videoDisplayHeight(void);

/* 1 si la superficie vive en memoria principal (escrituras cacheadas),
 * 0 si esta en memoria local del RSX (write-combined, lenta para
 * escrituras dispersas). */
int videoSurfaceInMain(void);

#endif /* GR33N_VIDEO_H */
