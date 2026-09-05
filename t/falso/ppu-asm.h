/* Falsa. __gettime() es el contador de la PPE; en el PC vale el reloj
 * monotono en "ticks" de 1 us y tb_hz = 1000000. */
#ifndef FALSO_PPU_ASM_H
#define FALSO_PPU_ASM_H
#include <stdint.h>
uint64_t __gettime(void);
#endif
