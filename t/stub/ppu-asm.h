#pragma once
#include <ppu-types.h>
typedef struct { u32 func; u32 toc; } __opd32;
#define __get_opd32(x) (((__opd32*)(void*)(x))->func)
