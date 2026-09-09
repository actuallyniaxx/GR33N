/* GR33N - cliente de streaming para PS3
 *
 * Definiciones comunes. Todo lo que sea configuracion global vive aqui.
 */

#ifndef GR33N_H
#define GR33N_H

#include <ppu-types.h>

#define GR33N_NAME     "GR33N"
#define GR33N_VERSION  "1.0.1"

/* Superficie de trabajo: el buffer ARGB8888 donde acabara el frame
 * decodificado cuando exista decodificador. De momento la rellenamos a
 * mano con un patron de prueba.
 *
 * 1280x720 no es casualidad: es el formato objetivo del stream
 * (H.264 720p60, igual que cell-stream). Si esto no va a 60 fps
 * pintado a pelo desde la PPU, tampoco ira con video real.
 */
#define GR33N_SURFACE_W  1280
#define GR33N_SURFACE_H  720

/* Pixel ARGB8888 tal y como lo espera GCM_TEXTURE_FORMAT_A8R8G8B8
 * leido como u32 en big-endian: 0xAARRGGBB. */
#define GR33N_ARGB(a,r,g,b) \
	((((u32)(a)&0xff)<<24)|(((u32)(r)&0xff)<<16)|(((u32)(g)&0xff)<<8)|((u32)(b)&0xff))

#define GR33N_RGB(r,g,b) GR33N_ARGB(0xff,(r),(g),(b))

#endif /* GR33N_H */
