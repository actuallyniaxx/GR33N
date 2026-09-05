/* Copia LITERAL de $PS3DEV/ppu/include/jpgdec/jpgdec.h, solo para tipos. */
#ifndef __LV2_JPGDEC_H__
#define __LV2_JPGDEC_H__
#include <ppu-types.h>
#define JPGDEC_ERROR_OK 0
typedef struct _jpgdec_strm_info jpgDecStrmInfo;
typedef struct _jpgdec_strm_param jpgDecStrmParam;
typedef struct _jpgdec_disp_info jpgDecDispInfo;
typedef struct _jpgdec_disp_param jpgDecDispParam;
typedef void* (*jpgCbCtrlMalloc)(u32 size,void *cbCtrlArg);
typedef void (*jpgCbCtrlFree)(void *ptr,void *cbCtrlArg);
typedef enum { JPGDEC_FILE=0, JPGDEC_BUFFER=1 } jpgStreamSel;
typedef enum { JPGDEC_SPU_THREAD_DISABLE=0, JPGDEC_SPU_THREAD_ENABLE=1 } jpgSpuThreadEna;
typedef enum { JPGDEC_GRAYSCALE=1, JPGDEC_RGB=2, JPGDEC_YCBCR=3, JPGDEC_RGBA=10,
               JPGDEC_UPSTREAM=11, JPGDEC_ARGB=20, } jpgColorSpace;
typedef enum { JPGDEC_STATUS_FINISH=0, JPGDEC_STATUS_STOP=1 } jpgDecodeStatus;
typedef enum { JPGDEC_CONTINUE=0, JPGDEC_STOP=1 } jpgCommand;
typedef enum { JPGDEC_QUALITY=0, JPGDEC_FAST=5 } jpgMethod;
typedef enum { JPGDEC_TOP_TO_BOTTOM=0, JPGDEC_BOTTOM_TO_TOP=1 } jpgOutputMode;
typedef struct _jpgdec_thread_in_param {
	u32 spu_enable; u32 ppu_prio; u32 spu_prio;
	jpgCbCtrlMalloc malloc_func ATTRIBUTE_PRXPTR; void *malloc_arg ATTRIBUTE_PRXPTR;
	jpgCbCtrlFree free_func ATTRIBUTE_PRXPTR; void *free_arg ATTRIBUTE_PRXPTR;
} jpgDecThreadInParam;
typedef struct _jpgdec_thread_out_param { u32 version; } jpgDecThreadOutParam;
typedef struct _jpgdec_src {
	u32 stream_sel; const char *file_name ATTRIBUTE_PRXPTR; s64 file_offset;
	u32 file_size; void *stream_ptr ATTRIBUTE_PRXPTR; u32 stream_size; u32 spu_enable;
} jpgDecSource;
typedef struct _jpgdec_info { u32 width; u32 height; u32 num_comp; u32 color_space; } jpgDecInfo;
typedef struct _jpgdec_data_info { f32 value; u32 output_lines; u32 decode_status; } jpgDecDataInfo;
typedef struct _jpgdec_opn_info { u32 init_space_allocated; } jpgDecOpnInfo;
typedef struct _jpgdec_in_param {
	vu32 *cmd_ptr ATTRIBUTE_PRXPTR; u32 down_scale; u32 quality_mode;
	u32 output_mode; u32 color_space; u8 alpha; u8 pad[3];
} jpgDecInParam;
typedef struct _jpgdec_out_param {
	u64 width_bytes; u32 width; u32 height; u32 num_comp; u32 output_mode;
	u32 color_space; u32 down_scale; u32 use_memory_space;
} jpgDecOutParam;
typedef struct _jpgdec_datactrl_param { u64 output_bytes_per_line; } jpgDecDataCtrlParam;
s32 jpgDecCreate(s32 *handle,jpgDecThreadInParam *in,jpgDecThreadOutParam *out);
s32 jpgDecOpen(s32 handle,s32 *subhandle,const jpgDecSource *src,jpgDecOpnInfo *openInfo);
s32 jpgDecReadHeader(s32 handle,s32 subhandle,jpgDecInfo *info);
s32 jpgDecSetParameter(s32 handle,s32 subhandle,const jpgDecInParam *in,jpgDecOutParam *out);
s32 jpgDecDecodeData(s32 handle,s32 subhandle,u8 *data,const jpgDecDataCtrlParam *p,jpgDecDataInfo *info);
s32 jpgDecClose(s32 handle,s32 subhandle);
s32 jpgDecDestroy(s32 handle);
#endif
