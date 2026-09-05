/* Cabecera falsa para poder compilar en el PC lo que solo necesita los
 * tipos. Los tamanos son los del ABI de 32 bits del PPU. */
#ifndef FALSO_PPU_TYPES_H
#define FALSO_PPU_TYPES_H
#include <stdint.h>
typedef uint8_t  u8;   typedef int8_t  s8;
typedef uint16_t u16;  typedef int16_t s16;
typedef uint32_t u32;  typedef int32_t s32;
typedef uint64_t u64;  typedef int64_t s64;
/* Y los flotantes, que se me olvidaron: audioPortParam.level es f32 y el
 * buffer del puerto tambien. */
typedef float  f32;
typedef double f64;
#endif
