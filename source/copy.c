/* GR33N - copia y relleno rapidos para la PPE.
 *
 * Tres trucos, por orden de lo que aportan:
 *
 *   dcbz  Escribir en memoria cacheada obliga a la cache a LEER la linea
 *         antes de modificarla, aunque la vayas a pisar entera. Es la
 *         mitad del trafico de un volcado de pantalla, tirado a la basura.
 *         dcbz declara la linea como propia y a cero sin leerla. Solo vale
 *         en memoria cacheada: sobre la memoria local del RSX levanta una
 *         excepcion de alineamiento. De ahi copyInit().
 *
 *   VMX   La PPE lleva unidad AltiVec: 16 bytes por instruccion. vec_st
 *         IGNORA los cuatro bits bajos de la direccion, asi que mal
 *         alineado no es lento, es incorrecto. Solo se usa cuando origen y
 *         destino estan los dos alineados a 16, y eso se comprueba, no se
 *         supone.
 *
 *   64b   -mcpu=cell activa las instrucciones de 64 bits aunque el ABI sea
 *         de 32. Un unsigned long long se mueve en una instruccion en vez
 *         de dos. Si el compilador no las tuviera, el codigo sigue siendo
 *         correcto: solo se parte en dos.
 *
 * El bloque de 128 bytes no es arbitrario: es el tamano de linea de cache
 * de la PPE en L1 y en L2, y por tanto lo que abarca un dcbz. El bucle
 * escribe los 128 bytes completos detras de cada dcbz, asi que aunque el
 * bloque fuese mas pequeno de lo que creo, el resultado sigue siendo
 * correcto - solo se ahorraria menos.
 */

#include <string.h>
#include <stdint.h>

#include <ppu-types.h>

#include "copy.h"

/* Compila con -DGR33N_NO_VMX si alguna vez hay que descartar la unidad
 * vectorial como sospechosa. El camino de 64 bits queda, y sigue siendo
 * mas rapido que memcpy. */
#if defined(__ALTIVEC__) && !defined(GR33N_NO_VMX)
#include <altivec.h>
#define COPY_HAVE_VMX 1
#else
#define COPY_HAVE_VMX 0
#endif

#define BLOCK      128u   /* linea de cache de la PPE = alcance de dcbz */
#define SMALL_MIN  256u   /* por debajo de esto, memcpy ya esta bien */

static int use_dcbz = 0;

/* RA=0 literal, RB = el registro con la direccion. Aunque el compilador
 * meta la direccion en r0, sigue siendo correcto: RB siempre es contenido
 * de registro.
 *
 * El #if es para poder compilar este fichero en el PC y pasarle una
 * bateria de pruebas de alineamiento: fuera de PowerPC no hay dcbz, pero
 * la aritmetica de cabeza/bloques/cola es la misma y es donde viven los
 * errores que corrompen memoria. */
#if defined(__powerpc__) || defined(__PPC__) || defined(__powerpc64__)
static inline void dcbz_line(void *p)
{
	__asm__ __volatile__("dcbz 0,%0" : : "r" (p) : "memory");
}
#else
static inline void dcbz_line(void *p) { (void)p; }
#endif

void copyInit(int dst_cacheable)
{
	use_dcbz = dst_cacheable ? 1 : 0;
}

const char *copyMode(void)
{
#if COPY_HAVE_VMX
	return use_dcbz ? "vmx+dcbz" : "vmx";
#else
	return use_dcbz ? "64b+dcbz" : "64b";
#endif
}

/* --------------------------------------------------------------------- */
/* Relleno                                                               */
/* --------------------------------------------------------------------- */

void fillWords(u32 *dst, u32 value, u32 count)
{
	u32 *p = dst;
	u32 *end = dst + count;
	u32 blocks;

	if (!dst || !count) return;

	/* Cabeza: palabra a palabra hasta pisar frontera de bloque. Como
	 * mucho 31 palabras, y solo la primera vez. */
	while (p < end && ((uintptr_t)p & (BLOCK - 1)))
		*p++ = value;

	blocks = (u32)(end - p) / (BLOCK / 4);

	if (blocks) {
#if COPY_HAVE_VMX
		union { u32 w[4]; __vector unsigned int v; } splat;
		__vector unsigned int vv;

		splat.w[0] = splat.w[1] = splat.w[2] = splat.w[3] = value;
		vv = splat.v;

		if (use_dcbz) {
			while (blocks--) {
				dcbz_line(p);
				vec_st(vv, 0,  (unsigned int*)p);
				vec_st(vv, 16, (unsigned int*)p);
				vec_st(vv, 32, (unsigned int*)p);
				vec_st(vv, 48, (unsigned int*)p);
				vec_st(vv, 64, (unsigned int*)p);
				vec_st(vv, 80, (unsigned int*)p);
				vec_st(vv, 96, (unsigned int*)p);
				vec_st(vv, 112,(unsigned int*)p);
				p += BLOCK / 4;
			}
		} else {
			while (blocks--) {
				vec_st(vv, 0,  (unsigned int*)p);
				vec_st(vv, 16, (unsigned int*)p);
				vec_st(vv, 32, (unsigned int*)p);
				vec_st(vv, 48, (unsigned int*)p);
				vec_st(vv, 64, (unsigned int*)p);
				vec_st(vv, 80, (unsigned int*)p);
				vec_st(vv, 96, (unsigned int*)p);
				vec_st(vv, 112,(unsigned int*)p);
				p += BLOCK / 4;
			}
		}
#else
		u64 v64 = ((u64)value << 32) | (u64)value;
		u64 *q = (u64*)p;
		int i;

		while (blocks--) {
			if (use_dcbz) dcbz_line(q);
			for (i = 0; i < (int)(BLOCK / 8); i++)
				q[i] = v64;
			q += BLOCK / 8;
		}
		p = (u32*)q;
#endif
	}

	while (p < end)
		*p++ = value;
}

/* --------------------------------------------------------------------- */
/* Copia                                                                 */
/* --------------------------------------------------------------------- */

void copyBytes(void *dst, const void *src, u32 bytes)
{
	u8 *d = (u8*)dst;
	const u8 *s = (const u8*)src;
	u32 head, blocks, rest;

	if (!d || !s || !bytes) return;

	if (bytes < SMALL_MIN) {
		memcpy(d, s, bytes);
		return;
	}

	/* Se alinea el DESTINO, no el origen: dcbz trabaja sobre lineas
	 * completas del destino y es donde esta el ahorro. */
	head = (u32)((BLOCK - ((uintptr_t)d & (BLOCK - 1))) & (BLOCK - 1));
	if (head) {
		memcpy(d, s, head);
		d += head; s += head; bytes -= head;
	}

	blocks = bytes / BLOCK;
	rest   = bytes % BLOCK;

	/* El origen tiene que estar alineado por su cuenta: aqui ya no van
	 * los dos del brazo. Si no lo esta, se queda con memcpy - que al
	 * menos ahora escribe sobre lineas que dcbz ha reclamado. */
#if COPY_HAVE_VMX
	if (((uintptr_t)s & 15) == 0) {
		const unsigned int *vs = (const unsigned int*)(const void*)s;
		unsigned int *vd = (unsigned int*)(void*)d;

		while (blocks--) {
			__vector unsigned int v0 = vec_ld(0,   vs);
			__vector unsigned int v1 = vec_ld(16,  vs);
			__vector unsigned int v2 = vec_ld(32,  vs);
			__vector unsigned int v3 = vec_ld(48,  vs);
			__vector unsigned int v4 = vec_ld(64,  vs);
			__vector unsigned int v5 = vec_ld(80,  vs);
			__vector unsigned int v6 = vec_ld(96,  vs);
			__vector unsigned int v7 = vec_ld(112, vs);

			if (use_dcbz) dcbz_line(vd);

			vec_st(v0, 0,   vd);
			vec_st(v1, 16,  vd);
			vec_st(v2, 32,  vd);
			vec_st(v3, 48,  vd);
			vec_st(v4, 64,  vd);
			vec_st(v5, 80,  vd);
			vec_st(v6, 96,  vd);
			vec_st(v7, 112, vd);

			vs += BLOCK / 4;   /* 32 palabras = 128 bytes */
			vd += BLOCK / 4;
		}

		d = (u8*)(void*)vd;
		s = (const u8*)(const void*)vs;
		if (rest) memcpy(d, s, rest);
		return;
	}
#endif

	if (((uintptr_t)s & 7) == 0) {
		u64 *q = (u64*)(void*)d;
		const u64 *r = (const u64*)(const void*)s;

		while (blocks--) {
			if (use_dcbz) dcbz_line(q);

			q[0]  = r[0];  q[1]  = r[1];  q[2]  = r[2];  q[3]  = r[3];
			q[4]  = r[4];  q[5]  = r[5];  q[6]  = r[6];  q[7]  = r[7];
			q[8]  = r[8];  q[9]  = r[9];  q[10] = r[10]; q[11] = r[11];
			q[12] = r[12]; q[13] = r[13]; q[14] = r[14]; q[15] = r[15];

			q += BLOCK / 8;
			r += BLOCK / 8;
		}

		d = (u8*)(void*)q;
		s = (const u8*)(const void*)r;
		if (rest) memcpy(d, s, rest);
		return;
	}

	/* Origen desalineado: al menos se le quita a la cache la lectura
	 * previa de cada linea del destino. */
	while (blocks--) {
		if (use_dcbz) dcbz_line(d);
		memcpy(d, s, BLOCK);
		d += BLOCK;
		s += BLOCK;
	}
	if (rest) memcpy(d, s, rest);
}
