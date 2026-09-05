/* GR33N - fuente 5x7 empotrada y primitivas de dibujo.
 *
 * Cada glifo son 5 bytes, uno por columna. En cada byte, el bit 0 es la
 * fila de arriba. ASCII 32..122 (espacio .. 'z').
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include <ppu-types.h>

#include "gr33n.h"
#include "video.h"
#include "text.h"
#include "copy.h"

#define FONT_FIRST 32
#define FONT_LAST  122

/* Glifos de mas alla del ASCII.
 *
 * La fuente cubria 32..122 y ya. En cuanto una descripcion decia "enseña"
 * o "año", la ñ desaparecia o salia partida: en UTF-8 son DOS bytes, y el
 * bucle de dibujo los trataba como dos caracteres sueltos que no estaban
 * en la tabla. De ahi que a veces se viera una n suelta y a veces nada.
 *
 * Arreglarlo tiene dos mitades y hacen falta las dos: decodificar UTF-8
 * al recorrer la cadena, y tener el dibujo del caracter. Esto es la
 * segunda mitad.
 *
 * Las minusculas ocupan las filas 2..6, asi que la 0 y la 1 quedan libres
 * para tildes. Las mayusculas ocupan las siete, asi que hay que bajarlas
 * una fila para hacer sitio. */
typedef struct { u32 cp; u8 col[5]; } fontExtra;

static const fontExtra font_extra[] = {
	/* n con la virgulilla ondeando: filas 0 y 1 alternas */
	{ 0xF1, {0x7E,0x09,0x05,0x05,0x7A} },   /* ñ */
	{ 0xD1, {0x7C,0x09,0x11,0x21,0x7C} },   /* Ñ (N bajada dos filas) */

	/* Vocales con tilde: el acento se inclina, fila 1 y luego fila 0 */
	{ 0xE1, {0x20,0x54,0x56,0x55,0x78} },   /* a */
	{ 0xE9, {0x38,0x54,0x56,0x55,0x18} },   /* e */
	{ 0xED, {0x00,0x46,0x7C,0x41,0x00} },   /* i */
	{ 0xF3, {0x38,0x44,0x46,0x45,0x38} },   /* o */
	{ 0xFA, {0x3C,0x40,0x42,0x21,0x7C} },   /* u */
	{ 0xFC, {0x3C,0x41,0x40,0x21,0x7C} },   /* u con dieresis */

	/* Mayusculas acentuadas: bajadas una fila, con el acento arriba */
	{ 0xC1, {0x7C,0x12,0x13,0x12,0x7C} },   /* A */
	{ 0xC9, {0x7E,0x4A,0x4B,0x4A,0x42} },   /* E */
	{ 0xCD, {0x00,0x42,0x7F,0x41,0x00} },   /* I */
	{ 0xD3, {0x3C,0x42,0x43,0x42,0x3C} },   /* O */
	{ 0xDA, {0x3E,0x40,0x41,0x40,0x3E} },   /* U */

	/* Apertura de interrogacion y exclamacion, giradas 180 grados */
	{ 0xBF, {0x30,0x48,0x45,0x40,0x20} },   /* ¿ */
	{ 0xA1, {0x00,0x00,0x7D,0x00,0x00} },   /* ¡ */

	{ 0xB0, {0x00,0x07,0x05,0x07,0x00} }    /* grado */
};
#define FONT_EXTRA_N ((int)(sizeof(font_extra)/sizeof(font_extra[0])))

static const u8 font5x7[FONT_LAST - FONT_FIRST + 1][5] = {
	{0x00,0x00,0x00,0x00,0x00}, /*   */
	{0x00,0x00,0x5F,0x00,0x00}, /* ! */
	{0x00,0x07,0x00,0x07,0x00}, /* " */
	{0x14,0x7F,0x14,0x7F,0x14}, /* # */
	{0x24,0x2A,0x7F,0x2A,0x12}, /* $ */
	{0x23,0x13,0x08,0x64,0x62}, /* % */
	{0x36,0x49,0x55,0x22,0x50}, /* & */
	{0x00,0x05,0x03,0x00,0x00}, /* ' */
	{0x00,0x1C,0x22,0x41,0x00}, /* ( */
	{0x00,0x41,0x22,0x1C,0x00}, /* ) */
	{0x14,0x08,0x3E,0x08,0x14}, /* * */
	{0x08,0x08,0x3E,0x08,0x08}, /* + */
	{0x00,0x50,0x30,0x00,0x00}, /* , */
	{0x08,0x08,0x08,0x08,0x08}, /* - */
	{0x00,0x60,0x60,0x00,0x00}, /* . */
	{0x20,0x10,0x08,0x04,0x02}, /* / */
	{0x3E,0x51,0x49,0x45,0x3E}, /* 0 */
	{0x00,0x42,0x7F,0x40,0x00}, /* 1 */
	{0x42,0x61,0x51,0x49,0x46}, /* 2 */
	{0x21,0x41,0x45,0x4B,0x31}, /* 3 */
	{0x18,0x14,0x12,0x7F,0x10}, /* 4 */
	{0x27,0x45,0x45,0x45,0x39}, /* 5 */
	{0x3C,0x4A,0x49,0x49,0x30}, /* 6 */
	{0x01,0x71,0x09,0x05,0x03}, /* 7 */
	{0x36,0x49,0x49,0x49,0x36}, /* 8 */
	{0x06,0x49,0x49,0x29,0x1E}, /* 9 */
	{0x00,0x36,0x36,0x00,0x00}, /* : */
	{0x00,0x56,0x36,0x00,0x00}, /* ; */
	{0x08,0x14,0x22,0x41,0x00}, /* < */
	{0x14,0x14,0x14,0x14,0x14}, /* = */
	{0x00,0x41,0x22,0x14,0x08}, /* > */
	{0x02,0x01,0x51,0x09,0x06}, /* ? */
	{0x32,0x49,0x79,0x41,0x3E}, /* @ */
	{0x7E,0x11,0x11,0x11,0x7E}, /* A */
	{0x7F,0x49,0x49,0x49,0x36}, /* B */
	{0x3E,0x41,0x41,0x41,0x22}, /* C */
	{0x7F,0x41,0x41,0x22,0x1C}, /* D */
	{0x7F,0x49,0x49,0x49,0x41}, /* E */
	{0x7F,0x09,0x09,0x09,0x01}, /* F */
	{0x3E,0x41,0x49,0x49,0x7A}, /* G */
	{0x7F,0x08,0x08,0x08,0x7F}, /* H */
	{0x00,0x41,0x7F,0x41,0x00}, /* I */
	{0x20,0x40,0x41,0x3F,0x01}, /* J */
	{0x7F,0x08,0x14,0x22,0x41}, /* K */
	{0x7F,0x40,0x40,0x40,0x40}, /* L */
	{0x7F,0x02,0x0C,0x02,0x7F}, /* M */
	{0x7F,0x04,0x08,0x10,0x7F}, /* N */
	{0x3E,0x41,0x41,0x41,0x3E}, /* O */
	{0x7F,0x09,0x09,0x09,0x06}, /* P */
	{0x3E,0x41,0x51,0x21,0x5E}, /* Q */
	{0x7F,0x09,0x19,0x29,0x46}, /* R */
	{0x46,0x49,0x49,0x49,0x31}, /* S */
	{0x01,0x01,0x7F,0x01,0x01}, /* T */
	{0x3F,0x40,0x40,0x40,0x3F}, /* U */
	{0x1F,0x20,0x40,0x20,0x1F}, /* V */
	{0x3F,0x40,0x38,0x40,0x3F}, /* W */
	{0x63,0x14,0x08,0x14,0x63}, /* X */
	{0x07,0x08,0x70,0x08,0x07}, /* Y */
	{0x61,0x51,0x49,0x45,0x43}, /* Z */
	{0x00,0x7F,0x41,0x41,0x00}, /* [ */
	{0x02,0x04,0x08,0x10,0x20}, /* \ */
	{0x00,0x41,0x41,0x7F,0x00}, /* ] */
	{0x04,0x02,0x01,0x02,0x04}, /* ^ */
	{0x40,0x40,0x40,0x40,0x40}, /* _ */
	{0x00,0x01,0x02,0x04,0x00}, /* ` */
	{0x20,0x54,0x54,0x54,0x78}, /* a */
	{0x7F,0x48,0x44,0x44,0x38}, /* b */
	{0x38,0x44,0x44,0x44,0x20}, /* c */
	{0x38,0x44,0x44,0x48,0x7F}, /* d */
	{0x38,0x54,0x54,0x54,0x18}, /* e */
	{0x08,0x7E,0x09,0x01,0x02}, /* f */
	{0x0C,0x52,0x52,0x52,0x3E}, /* g */
	{0x7F,0x08,0x04,0x04,0x78}, /* h */
	{0x00,0x44,0x7D,0x40,0x00}, /* i */
	{0x20,0x40,0x44,0x3D,0x00}, /* j */
	{0x7F,0x10,0x28,0x44,0x00}, /* k */
	{0x00,0x41,0x7F,0x40,0x00}, /* l */
	{0x7C,0x04,0x18,0x04,0x78}, /* m */
	{0x7C,0x08,0x04,0x04,0x78}, /* n */
	{0x38,0x44,0x44,0x44,0x38}, /* o */
	{0x7C,0x14,0x14,0x14,0x08}, /* p */
	{0x08,0x14,0x14,0x18,0x7C}, /* q */
	{0x7C,0x08,0x04,0x04,0x08}, /* r */
	{0x48,0x54,0x54,0x54,0x20}, /* s */
	{0x04,0x3F,0x44,0x40,0x20}, /* t */
	{0x3C,0x40,0x40,0x20,0x7C}, /* u */
	{0x1C,0x20,0x40,0x20,0x1C}, /* v */
	{0x3C,0x40,0x30,0x40,0x3C}, /* w */
	{0x44,0x28,0x10,0x28,0x44}, /* x */
	{0x0C,0x50,0x50,0x50,0x3C}, /* y */
	{0x44,0x64,0x54,0x4C,0x44}  /* z */
};

/* --------------------------------------------------------------------- */
/* Primitivas                                                            */
/* --------------------------------------------------------------------- */

/* Antes esto montaba una fila en un buffer y la volcaba con memcpy, que
 * escribe cada pixel dos veces. fillWords escribe una sola vez y ademas
 * usa la unidad vectorial y dcbz. Para rectangulos estrechos degrada sola
 * a un bucle de palabras, asi que no hace falta el caso especial. */
void gfxFillRect(gr33nSurface *s, int x, int y, int w, int h, u32 color)
{
	int py;
	u32 stride;

	if (!s || !s->pixels) return;

	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > (int)s->width)  w = (int)s->width  - x;
	if (y + h > (int)s->height) h = (int)s->height - y;
	if (w <= 0 || h <= 0) return;

	stride = s->pitch / 4;

	for (py = y; py < y + h; py++)
		fillWords(s->pixels + (u32)py * stride + (u32)x, color, (u32)w);
}

/* Vuelca una imagen ARGB escalandola por vecino mas cercano.
 *
 * Es el mismo escalado tosco que usa el resto del proyecto y aqui sobra:
 * el avatar llega a 424x424 y se dibuja a 44x44, asi que lo que hace
 * falta es reducir mucho, no interpolar fino. */
void gfxBlit(gr33nSurface *s, int x, int y, int w, int h,
             const u32 *src, u32 sw, u32 sh)
{
	u32 stride;
	int px, py;

	if (!s || !s->pixels || !src || !sw || !sh || w <= 0 || h <= 0) return;

	stride = s->pitch / 4;

	for (py = 0; py < h; py++) {
		int dy = y + py;
		const u32 *row;
		u32 *dst;

		if (dy < 0 || dy >= (int)s->height) continue;

		row = src + (u32)((u64)py * sh / (u32)h) * sw;
		dst = s->pixels + (u32)dy * stride;

		for (px = 0; px < w; px++) {
			int dx = x + px;
			if (dx < 0 || dx >= (int)s->width) continue;
			dst[dx] = row[(u32)((u64)px * sw / (u32)w)];
		}
	}
}

void gfxRect(gr33nSurface *s, int x, int y, int w, int h, int thickness, u32 color)
{
	if (thickness <= 0) return;
	gfxFillRect(s, x, y, w, thickness, color);
	gfxFillRect(s, x, y + h - thickness, w, thickness, color);
	gfxFillRect(s, x, y, thickness, h, color);
	gfxFillRect(s, x + w - thickness, y, thickness, h, color);
}

void gfxVGradient(gr33nSurface *s, int x, int y, int w, int h, u32 c0, u32 c1)
{
	int i;
	int r0 = (int)((c0 >> 16) & 0xff), g0 = (int)((c0 >> 8) & 0xff), b0 = (int)(c0 & 0xff);
	int r1 = (int)((c1 >> 16) & 0xff), g1 = (int)((c1 >> 8) & 0xff), b1 = (int)(c1 & 0xff);

	if (h <= 0) return;

	for (i = 0; i < h; i++) {
		int r = r0 + (r1 - r0) * i / h;
		int g = g0 + (g1 - g0) * i / h;
		int b = b0 + (b1 - b0) * i / h;
		gfxFillRect(s, x, y + i, w, 1, GR33N_RGB(r, g, b));
	}
}

/* --------------------------------------------------------------------- */
/* Texto                                                                 */
/* --------------------------------------------------------------------- */

/* Un glifo son hasta 35 celdas. Antes cada celda era una llamada a
 * gfxFillRect con su recorte, sus divisiones y su prologo: con el HUD
 * lleno eso son decenas de miles de llamadas por frame para pintar unos
 * pocos miles de pixeles.
 *
 * Ahora se va por filas: se monta la mascara de la fila (5 bits) y se
 * escribe directamente en la superficie. El recorte se resuelve UNA vez
 * por glifo, no una vez por celda, porque el caso normal es que el glifo
 * quepa entero. */
/* Siguiente punto de codigo de una cadena UTF-8, avanzando el puntero.
 *
 * Solo se resuelven las secuencias de dos bytes (U+0080..U+07FF), que es
 * donde vive todo lo que necesita el castellano. Las de tres y cuatro se
 * saltan enteras y se devuelve 0: mejor un hueco que media letra y el
 * resto de la frase desplazada. */
static u32 utf8_next(const char **pp)
{
	const u8 *p = (const u8*)*pp;
	u32 cp;

	if (p[0] < 0x80) { *pp = (const char*)(p + 1); return p[0]; }

	if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
		cp = ((u32)(p[0] & 0x1F) << 6) | (u32)(p[1] & 0x3F);
		*pp = (const char*)(p + 2);
		return cp;
	}

	if ((p[0] & 0xF0) == 0xE0) { *pp = (const char*)(p + 3); return 0; }
	if ((p[0] & 0xF8) == 0xF0) { *pp = (const char*)(p + 4); return 0; }

	/* Byte suelto que no es UTF-8 valido: se traga uno y a seguir. */
	*pp = (const char*)(p + 1);
	return 0;
}

/* Dibujo del caracter, o NULL si no lo tenemos. */
static const u8 *glyph_for(u32 cp)
{
	int i;

	if (cp >= FONT_FIRST && cp <= FONT_LAST)
		return font5x7[cp - FONT_FIRST];

	for (i = 0; i < FONT_EXTRA_N; i++)
		if (font_extra[i].cp == cp) return font_extra[i].col;

	return NULL;
}

static void drawGlyph(gr33nSurface *s, int x, int y, int scale, u32 color, u32 cp)
{
	const u8 *g;
	u32 stride;
	int col, row, i, j;

	g = glyph_for(cp);
	if (!g) return;

	/* Fuera de la superficie: no se toca nada. */
	if (x >= (int)s->width || y >= (int)s->height) return;
	if (x + TEXT_GLYPH_W * scale <= 0 || y + TEXT_GLYPH_H * scale <= 0) return;

	/* A caballo del borde: camino lento por celdas, que es raro. */
	if (x < 0 || y < 0 ||
	    x + TEXT_GLYPH_W * scale > (int)s->width ||
	    y + TEXT_GLYPH_H * scale > (int)s->height) {
		for (col = 0; col < TEXT_GLYPH_W; col++) {
			u8 bits = g[col];
			if (!bits) continue;
			for (row = 0; row < TEXT_GLYPH_H; row++)
				if (bits & (1u << row))
					gfxFillRect(s, x + col * scale, y + row * scale,
					            scale, scale, color);
		}
		return;
	}

	stride = s->pitch / 4;

	for (row = 0; row < TEXT_GLYPH_H; row++) {
		u32 mask = 0;

		for (col = 0; col < TEXT_GLYPH_W; col++)
			if (g[col] & (1u << row))
				mask |= 1u << col;

		if (!mask) continue;

		for (j = 0; j < scale; j++) {
			u32 *p = s->pixels
			       + (u32)(y + row * scale + j) * stride + (u32)x;

			for (col = 0; col < TEXT_GLYPH_W; col++) {
				if (!(mask & (1u << col))) continue;
				for (i = 0; i < scale; i++)
					p[col * scale + i] = color;
			}
		}
	}
}

void textDraw(gr33nSurface *s, int x, int y, int scale, u32 color, const char *str)
{
	int cx = x;

	if (!s || !str) return;
	if (scale < 1) scale = 1;

	while (*str) {
		u32 cp = utf8_next(&str);

		if (cp == '\n') {
			cx = x;
			y += (TEXT_GLYPH_H + 2) * scale;
		} else {
			drawGlyph(s, cx, y, scale, color, cp);
			cx += TEXT_ADVANCE * scale;
		}
	}
}

void textPrintf(gr33nSurface *s, int x, int y, int scale, u32 color, const char *fmt, ...)
{
	char buf[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	textDraw(s, x, y, scale, color, buf);
}

/* CARACTERES, no bytes. Una cadena con enes con virgulilla ocupa mas
 * bytes que caracteres, y medirla por bytes descentra todo lo que se
 * centre o se alinee a la derecha. */
/* Copia una ventana de CODEPOINTS, no de bytes.
 *
 * Existe para el desplazamiento del nombre en la rejilla, y existe por el
 * mismo motivo por el que textWrap cuenta codepoints: cortar por bytes
 * parte una secuencia UTF-8 por la mitad y deja medio caracter en
 * pantalla. Con nombres que llevan acentos eso pasa constantemente.
 *
 * Devuelve cuantos codepoints se han copiado de verdad. */
int textSlice(char *out, u32 outsize, const char *src, int from_cp, int n_cp)
{
	const char *p = src;
	const char *start;
	u32 used = 0;
	int i, got = 0;

	if (!out || outsize == 0) return 0;
	out[0] = '\0';
	if (!src || n_cp <= 0) return 0;

	for (i = 0; i < from_cp && *p; i++) utf8_next(&p);

	start = p;

	while (*p && got < n_cp) {
		const char *before = p;
		u32 len;

		utf8_next(&p);
		len = (u32)(p - before);

		/* Cabe entero o no se copia: media secuencia no vale de nada. */
		if (used + len + 1 > outsize) break;

		used += len;
		got++;
	}

	memcpy(out, start, used);
	out[used] = '\0';

	return got;
}

/* Cuantos codepoints tiene la cadena. */
int textLen(const char *str)
{
	int n = 0;

	if (!str) return 0;
	while (*str) { utf8_next(&str); n++; }

	return n;
}

int textWidth(int scale, const char *str)
{
	int n = 0;

	if (!str) return 0;
	if (scale < 1) scale = 1;

	while (*str) { utf8_next(&str); n++; }

	return n * TEXT_ADVANCE * scale;
}

/* Ajuste de linea contando CARACTERES para el ancho y BYTES para copiar.
 *
 * Hay que llevar las dos cuentas a la vez. Con solo bytes, una linea con
 * enes con virgulilla se corta antes de tiempo; y peor: partir una
 * palabra larga por un numero de bytes puede cortar una secuencia UTF-8
 * por la mitad y dejar medio caracter, que es como se ven esas cajitas
 * raras en las interfaces mal hechas. */
/* Como textWrap pero con ventana: salta las primeras `skip` lineas y
 * DEVUELVE cuantas lineas necesita el texto entero.
 *
 * Existe porque la descripcion de un juego se recorria por BYTES: el stick
 * derecho movia el texto caracter a caracter, saltaba a mitad de palabra y
 * podia partir una secuencia UTF-8. Un texto se recorre por lineas, que es
 * la unidad que ve el que lee.
 *
 * Con s = NULL no dibuja nada y solo cuenta, que es como la interfaz sabe
 * hasta donde puede bajar. */
int textWrapView(gr33nSurface *s, int x, int y, int scale, u32 color,
                 int max_w, int line_h, int max_lines, const char *str,
                 int skip)
{
	char line[256];
	int total = 0, drawn = 0;
	int lbytes = 0, lcols = 0;
	int per_line;

	if (!str) return 0;
	if (scale < 1) scale = 1;

	per_line = max_w / (TEXT_ADVANCE * scale);
	if (per_line < 1) return 0;

	if (skip < 0) skip = 0;

	while (*str) {
		const char *word = str;
		const char *w = str;
		int wbytes = 0, wcols = 0;
		int brk = 0;

		if (*w == '\n') { str = w + 1; brk = 1; }

		if (!brk) {
			while (*w && *w != ' ' && *w != '\n') {
				const char *prev = w;
				utf8_next(&w);
				wbytes += (int)(w - prev);
				wcols++;
			}

			if (wcols > per_line) {
				const char *q = word;
				int b = 0, c = 0;

				while (c < per_line && *q && *q != ' ' && *q != '\n') {
					const char *prev = q;
					utf8_next(&q);
					b += (int)(q - prev);
					c++;
				}
				wbytes = b;
				wcols  = c;
			}
		}

		/* Salto de linea explicito: la descripcion de la tienda los usa
		 * para separar parrafos y titulares. Respetarlos es la
		 * diferencia entre un texto y un ladrillo. */
		if (brk || (lcols && lcols + 1 + wcols > per_line)) {
			line[lbytes] = '\0';
			if (s && total >= skip && drawn < max_lines) {
				textDraw(s, x, y + drawn * line_h, scale, color, line);
				drawn++;
			}
			total++;
			lbytes = lcols = 0;
			if (brk) continue;
		}

		if (lcols && lbytes < (int)sizeof(line) - 1) {
			line[lbytes++] = ' ';
			lcols++;
		}

		if (wbytes < (int)sizeof(line) - lbytes) {
			memcpy(line + lbytes, word, (size_t)wbytes);
			lbytes += wbytes;
			lcols  += wcols;
		}

		str = word + wbytes;
		while (*str == ' ') str++;
	}

	if (lbytes) {
		line[lbytes] = '\0';
		if (s && total >= skip && drawn < max_lines)
			textDraw(s, x, y + drawn * line_h, scale, color, line);
		total++;
	}

	return total;
}

int textWrap(gr33nSurface *s, int x, int y, int scale, u32 color,
             int max_w, int line_h, int max_lines, const char *str)
{
	char line[256];
	int lines = 0;
	int lbytes = 0, lcols = 0;
	int per_line;

	if (!s || !str) return 0;
	if (scale < 1) scale = 1;

	per_line = max_w / (TEXT_ADVANCE * scale);
	if (per_line < 1) return 0;

	while (*str && lines < max_lines) {
		const char *word = str;
		const char *w = str;
		int wbytes = 0, wcols = 0;

		while (*w && *w != ' ' && *w != '\n') {
			const char *prev = w;
			utf8_next(&w);
			wbytes += (int)(w - prev);
			wcols++;
		}

		/* Palabra mas larga que la linea entera: se parte, pero por
		 * frontera de caracter. */
		if (wcols > per_line) {
			const char *q = word;
			int b = 0, c = 0;

			while (c < per_line && *q && *q != ' ' && *q != '\n') {
				const char *prev = q;
				utf8_next(&q);
				b += (int)(q - prev);
				c++;
			}
			wbytes = b;
			wcols  = c;
		}

		if (lcols && lcols + 1 + wcols > per_line) {
			line[lbytes] = '\0';
			textDraw(s, x, y + lines * line_h, scale, color, line);
			lines++;
			lbytes = lcols = 0;
			if (lines >= max_lines) break;
		}

		if (lcols && lbytes < (int)sizeof(line) - 1) {
			line[lbytes++] = ' ';
			lcols++;
		}

		if (wbytes < (int)sizeof(line) - lbytes) {
			memcpy(line + lbytes, word, (size_t)wbytes);
			lbytes += wbytes;
			lcols  += wcols;
		}

		str = word + wbytes;

		if (*str == '\n') {
			line[lbytes] = '\0';
			textDraw(s, x, y + lines * line_h, scale, color, line);
			lines++;
			lbytes = lcols = 0;
			str++;
		} else if (*str == ' ') {
			str++;
		}
	}

	if (lbytes && lines < max_lines) {
		line[lbytes] = '\0';
		textDraw(s, x, y + lines * line_h, scale, color, line);
		lines++;
	}

	return lines;
}
