#pragma once
#include <ppu-types.h>
#define SYSMODULE_PNGDEC 0x18
#define SYSMODULE_ERR_DUPLICATE 0x80012002
s32 sysModuleLoad(u32); s32 sysModuleUnload(u32);
#define SYSMODULE_JPGDEC 0x0f
