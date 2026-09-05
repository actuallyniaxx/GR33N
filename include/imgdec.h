/* GR33N - PNG a pixeles, con el decodificador de la consola
 *
 * La PS3 trae un decodificador de PNG en firmware (cellPngDec) que puede
 * usar SPUs. Podriamos meter libpng, pero seria traer un descompresor
 * entero para descomprimir una foto de perfil cuando la consola ya sabe
 * hacerlo.
 *
 * De momento solo se usa para el avatar de Xbox Live. Mas adelante valdra
 * igual para las caratulas de los juegos, que son la misma operacion
 * repetida veinte veces.
 *
 * EL RESULTADO ES ARGB DE 32 BITS, igual que la superficie de dibujo y
 * igual que lo que suelta el decodificador de video. Que las tres cosas
 * hablen el mismo formato no es casualidad: es lo que permite que gfxBlit
 * sea diez lineas y no un conversor de espacios de color.
 */

#ifndef GR33N_IMGDEC_H
#define GR33N_IMGDEC_H

#include <ppu-types.h>

/* Tope de lo que se acepta decodificar.
 *
 * El limite existe porque el buffer de salida es fijo. Un PNG dice su
 * tamano en la cabecera, ANTES de decodificar: si no cabe, se rechaza ahi
 * y no se llama al decodificador. Con cellVdec ya aprendimos lo que pasa
 * cuando se le da a una biblioteca del sistema un buffer mas pequeno de lo
 * que va a escribir: no devuelve un error, se lleva la consola por delante.
 *
 * Los avatares de Xbox son de 424x424 o 1080x1080 segun el tamano que
 * pidas. 512 cubre el primero con holgura; el segundo se rechaza con un
 * mensaje claro en vez de reventar. */
#define IMG_MAX_W  512
#define IMG_MAX_H  512

typedef struct {
	const u32 *px;    /* ARGB, w*h palabras. Valido hasta la siguiente
	                   * llamada a imgDecodePNG. */
	u32 w, h;

	u32 src_bytes;    /* lo que ocupaba el PNG        */
	u32 ms;           /* lo que costo decodificarlo   */
	char err[128];
} imgImage;

/* ¿Esto tiene pinta de imagen? Solo mira los bytes magicos, sin
 * decodificar y sin tocar el candado.
 *
 * Sirve para no guardar basura en la cache del disco. caratulas.bin solo
 * crece y no se reescribe una entrada que ya este, asi que meter ahi una
 * respuesta cortada o unas cabeceras HTTP la envenena para siempre. */
int  imgLooksLikeImage(const void *data, u32 len);

/* Carga el modulo del sistema. Una vez, al arrancar. */
int  imgInit(void);
void imgShutdown(void);

/* Imagen en memoria -> ARGB. Devuelve 0 y rellena out, o -1 con el motivo
 * en out->err.
 *
 * imgDecode mira los primeros bytes y elige. Es lo que hay que llamar: las
 * caratulas de la tienda vienen en PNG o en JPEG segun el juego y la URL no
 * siempre lo dice.
 *
 * BLOQUEAN. Solo desde un hilo de trabajo.
 *
 * Diferencia util entre los dos: el decodificador de JPEG sabe REDUCIR
 * mientras descomprime, asi que acepta imagenes mas grandes que el buffer
 * (se piden a la mitad, a la cuarta parte...). El de PNG no: lo que no
 * quepa en IMG_MAX_W x IMG_MAX_H se rechaza. */
int  imgDecode(const void *data, u32 len, imgImage *out);
int  imgDecodePNG(const void *png, u32 len, imgImage *out);
int  imgDecodeJPG(const void *jpg, u32 len, imgImage *out);

/* DECODIFICA Y COPIA, y es lo que hay que llamar desde un hilo.
 *
 * Los tres de arriba escriben en un unico buffer interno y usan un unico
 * manejador del firmware, asi que dos hilos decodificando a la vez se
 * pisan: en el arranque se solapan de verdad, el avatar contra las
 * caratulas. Esto lo hace todo con el candado cogido -decodificar y
 * reducir a dst- y lo suelta con la copia ya hecha, asi que el buffer
 * compartido no sale de imgdec.c.
 *
 * dst == NULL decodifica sin copiar, para medir. En ese caso, y en
 * cualquier otro, `info->px` vuelve a NULL: esos pixeles son de quien
 * tenga el candado y al volver ya no lo tienes. */
int  imgDecodeTo(const void *data, u32 len, u32 *dst, u32 dw, u32 dh,
                 imgImage *info);

/* Reduce una imagen ARGB a otra mas pequena promediando bloques.
 *
 * No es lo mismo que hace gfxBlit. gfxBlit coge el pixel mas cercano, que
 * al pintar 44x44 desde 424x424 tira el 99% de la imagen y deja el avatar
 * hecho un cristal roto. Esto promedia, se hace UNA vez al descargar, y
 * deja una copia pequena que ya se pinta sin perder nada.
 *
 * dst tiene que tener sitio para dw*dh palabras. */
void imgShrink(u32 *dst, u32 dw, u32 dh,
               const u32 *src, u32 sw, u32 sh);

#endif /* GR33N_IMGDEC_H */
