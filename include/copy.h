/* GR33N - copia y relleno rapidos para la PPE
 *
 * El benchmark de arranque en hardware real dio esto moviendo 3,5 MB:
 *
 *     ram -> ram            465 MB/s   (memcpy, origen y destino cacheados)
 *     ram -> superficie     735 MB/s   (memcpy)
 *     bucle -> superficie  1148 MB/s   (for (i..) p[i] = 0)
 *
 * Un bucle de palabras escrito a mano es 2,5 veces mas rapido que el
 * memcpy de la libreria contra el mismo destino. Eso no es un problema de
 * memoria: es que el memcpy de newlib para PowerPC mueve palabras de 4
 * bytes y ya. Aqui se hace lo que la PPE sabe hacer.
 *
 * Todo lo de este modulo es correcto sea cual sea el compilador o el
 * destino; lo que cambia es lo rapido que va. copyMode() dice que camino
 * quedo activo, para que el log lo cuente en vez de suponerlo.
 */

#ifndef GR33N_COPY_H
#define GR33N_COPY_H

#include <ppu-types.h>

/* dst_cacheable: 1 si el destino habitual (la superficie) vive en memoria
 * principal, 0 si esta en memoria local del RSX.
 *
 * Importa porque dcbz - la instruccion que evita que la cache lea una
 * linea que vas a pisar entera - solo es valida sobre memoria cacheada.
 * Sobre memoria write-combined levanta una excepcion de alineamiento, que
 * en PS3 significa cuelgue. Se decide una vez, en el arranque. */
void copyInit(int dst_cacheable);

/* Copia de bloques grandes. Para menos de 256 bytes llama a memcpy, que
 * para eso ya vale. */
void copyBytes(void *dst, const void *src, u32 bytes);

/* Rellena count palabras de 32 bits. La operacion mas comun de toda la
 * interfaz: fondos, paneles, barras. */
void fillWords(u32 *dst, u32 value, u32 count);

/* "vmx+dcbz", "64b+dcbz", "64b"... para el encabezado de sesion. */
const char *copyMode(void);

#endif /* GR33N_COPY_H */
