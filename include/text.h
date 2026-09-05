/* GR33N - texto por software y primitivas de dibujo
 *
 * En PS3 no hay consola visible: printf se va al TTY y solo lo ves si
 * arrancas por ps3load. Sin texto en pantalla, depurar es adivinar.
 * Fuente 5x7 empotrada, dibujada por CPU en el mismo buffer ARGB.
 *
 * ASCII 32..122 (espacio .. 'z'), mayusculas y minusculas. Lo que caiga
 * fuera del rango se dibuja como hueco.
 */

#ifndef GR33N_TEXT_H
#define GR33N_TEXT_H

#include <ppu-types.h>
#include "video.h"

#define TEXT_GLYPH_W  5
#define TEXT_GLYPH_H  7
#define TEXT_ADVANCE  6   /* ancho de glifo + 1 de separacion */

void textDraw(gr33nSurface *s, int x, int y, int scale, u32 color, const char *str);
void textPrintf(gr33nSurface *s, int x, int y, int scale, u32 color, const char *fmt, ...);

/* Ancho en pixeles que ocuparia la cadena. Para centrar y alinear. */
int  textWidth(int scale, const char *str);

/* Numero de codepoints (no de bytes) de la cadena. */
int  textLen(const char *str);

/* Copia n_cp codepoints a partir de from_cp. Nunca parte una secuencia
 * UTF-8 por la mitad. Devuelve cuantos copio. */
int  textSlice(char *out, u32 outsize, const char *src, int from_cp, int n_cp);

/* Dibuja con salto de linea en espacios para no pasar de max_w pixeles.
 * Devuelve el numero de lineas dibujadas. */
int  textWrap(gr33nSurface *s, int x, int y, int scale, u32 color,
              int max_w, int line_h, int max_lines, const char *str);

/* Como textWrap pero con ventana: salta `skip` lineas y devuelve cuantas
 * necesita el texto ENTERO, para poder limitar el desplazamiento. Con
 * s = NULL solo cuenta. Respeta los saltos de linea del texto. */
int  textWrapView(gr33nSurface *s, int x, int y, int scale, u32 color,
                  int max_w, int line_h, int max_lines, const char *str,
                  int skip);

/* Primitivas de relleno. */
void gfxFillRect(gr33nSurface *s, int x, int y, int w, int h, u32 color);
void gfxRect(gr33nSurface *s, int x, int y, int w, int h, int thickness, u32 color);

/* Vuelca una imagen ARGB escalandola. Para el avatar del perfil. */
void gfxBlit(gr33nSurface *s, int x, int y, int w, int h,
             const u32 *src, u32 sw, u32 sh);

/* Degradado vertical. Barato (una fila calculada por linea) y hace que la
 * UI no parezca una pantalla de depuracion. */
void gfxVGradient(gr33nSurface *s, int x, int y, int w, int h, u32 c0, u32 c1);

#endif /* GR33N_TEXT_H */
