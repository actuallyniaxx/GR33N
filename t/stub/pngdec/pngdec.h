/* Copia LITERAL de las definiciones que pego el usuario desde
 * $PS3DEV/ppu/include/pngdec/pngdec.h, solo para comprobar tipos. */
#ifndef __LV2_PNGDEC_H__
#define __LV2_PNGDEC_H__
#include <ppu-types.h>
#define PNGDEC_ERROR_OK 0
typedef struct _pngdec_stream_info pngDecStreamInfo;
typedef struct _pngdec_stream_param pngDecStreamParam;
typedef struct _pngdec_disp_info pngDecDispInfo;
typedef struct _pngdec_disp_param pngDecDispParam;
typedef void* (*pngCbCtrlMalloc)(u32 size,void *cbCtrlArg);
typedef void (*pngCbCtrlFree)(void *ptr,void *cbCtrlArg);
typedef enum { PNGDEC_SPU_THREAD_DISABLE = 0, PNGDEC_SPU_THREAD_ENABLE = 1 } pngSpuThreadEna;
typedef enum { PNGDEC_FILE = 0, PNGDEC_BUFFER = 1 } pngStreamSel;
typedef enum { PNGDEC_GRAYSCALE=1, PNGDEC_RGB=2, PNGDEC_PALETTE=4,
               PNGDEC_GRAYSCALE_ALPHA=9, PNGDEC_RGBA=10, PNGDEC_ARGB=20 } pngColorSpace;
typedef enum { PNGDEC_NO_INTERLACE = 0, PNGDEC_ADAM7_INTERLACE = 1 } pngInterlaceMode;
typedef enum { PNGDEC_STATUS_FINISH = 0, PNGDEC_STATUS_STOP = 1 } pngDecodeStatus;
typedef enum { PNGDEC_CONTINUE = 0, PNGDEC_STOP = 1 } pngCommand;
typedef enum { PNGDEC_TOP_TO_BOTTOM = 0, PNGDEC_BOTTOM_TO_TOP = 1 } pngOutputMode;
typedef enum { PNGDEC_1BYTE_PER_NPIXEL = 0, PNGDEC_1BYTE_PER_1PIXEL = 1 } pngPackFlag;
typedef enum { PNGDEC_STREAM_ALPHA = 0, PNGDEC_FIX_ALPHA = 1 } pngAlphaSelect;
typedef struct _pngdec_thread_in_param {
	u32 spu_enable; u32 ppu_prio; u32 spu_prio;
	pngCbCtrlMalloc malloc_func ATTRIBUTE_PRXPTR; void *malloc_arg ATTRIBUTE_PRXPTR;
	pngCbCtrlFree free_func ATTRIBUTE_PRXPTR; void *free_arg ATTRIBUTE_PRXPTR;
} pngDecThreadInParam;
typedef struct _pngdec_thread_out_param { u32 version; } pngDecThreadOutParam;
typedef struct _pngdec_src {
	u32 stream_sel; const char *file_name ATTRIBUTE_PRXPTR; s64 file_offset;
	u32 file_size; void *stream_ptr ATTRIBUTE_PRXPTR; u32 stream_size; u32 spu_enable;
} pngDecSource;
typedef struct _pngdec_info {
	u32 width; u32 height; u32 num_comp; u32 color_space; u32 bit_depth;
	u32 interlace_mode; u32 chunk_info;
} pngDecInfo;
typedef struct _pngdec_data_info {
	u32 chunk_info; u32 num_text; u32 num_unk_chunk; u32 decode_status;
} pngDecDataInfo;
typedef struct _pngdec_in_param {
	vu32 *cmd_ptr ATTRIBUTE_PRXPTR; u32 output_mode; u32 color_space;
	u32 bit_depth; u32 pack_flag; u32 alpha_select; u32 alpha;
} pngDecInParam;
typedef struct _pngdec_out_param {
	u64 width_byte; u32 width; u32 height; u32 num_comp; u32 bit_depth;
	u32 output_mode; u32 color_space; u32 use_memory_space;
} pngDecOutParam;
typedef struct _pngdec_datactrl_param { u64 output_bytes_per_line; } pngDecDataCtrlParam;
typedef struct _pngdec_opn_info { u32 init_space_allocated; } pngDecOpnInfo;
s32 pngDecCreate(s32 *handle,pngDecThreadInParam *in,pngDecThreadOutParam *out);
s32 pngDecOpen(s32 handle,s32 *subhandle,const pngDecSource *src,pngDecOpnInfo *open_info);
s32 pngDecReadHeader(s32 handle,s32 subhandle,pngDecInfo *info);
s32 pngDecSetParameter(s32 handle,s32 subhandle,const pngDecInParam *in,pngDecOutParam *out);
s32 pngDecDecodeData(s32 handle,s32 subhandle,u8 *data,const pngDecDataCtrlParam *p,pngDecDataInfo *info);
s32 pngDecClose(s32 handle,s32 subhandle);
s32 pngDecDestroy(s32 handle);
#endif
