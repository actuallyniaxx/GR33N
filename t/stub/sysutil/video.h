/* Copia LITERAL de $PS3DEV/ppu/include/sysutil/video.h (PSL1GHT).
 * Solo se han quitado los comentarios doxygen largos. Los nombres, los
 * tipos y el ORDEN de los campos son los del toolchain: eso es lo unico
 * que hace util un stub. */
#ifndef __LV2_VIDEO_H__
#define __LV2_VIDEO_H__
#include <ppu-types.h>

#define VIDEO_STATE_DISABLED       0
#define VIDEO_STATE_ENABLED        1
#define VIDEO_STATE_BUSY           3
#define VIDEO_PRIMARY              0
#define VIDEO_SECONDARY            1
#define VIDEO_BUFFER_FORMAT_XRGB   0
#define VIDEO_BUFFER_FORMAT_XBGR   1
#define VIDEO_BUFFER_FORMAT_FLOAT  2
#define VIDEO_ASPECT_AUTO          0
#define VIDEO_ASPECT_4_3           1
#define VIDEO_ASPECT_16_9          2
#define VIDEO_RESOLUTION_UNDEFINED 0
#define VIDEO_RESOLUTION_1080      1
#define VIDEO_RESOLUTION_720       2
#define VIDEO_RESOLUTION_480       4
#define VIDEO_RESOLUTION_576       5

typedef struct _videoresolution { u16 width; u16 height; } videoResolution;

typedef struct _videodisplaymode {
	u8  resolution;
	u8  scanMode;
	u8  conversion;
	u8  aspect;
	u8  padding[2];
	u16 refreshRates;
} videoDisplayMode;

typedef struct _videostate {
	u8 state;
	u8 colorSpace;
	u8 padding[6];
	videoDisplayMode displayMode;
} videoState;

typedef struct _videoconfig {
	u8  resolution;
	u8  format;
	u8  aspect;
	u8  padding[9];
	u32 pitch;
} videoConfiguration;

s32 videoGetState(s32 videoOut, s32 deviceIndex, videoState *state);
s32 videoGetResolution(s32 resolutionId, videoResolution *resolution);
s32 videoConfigure(s32 videoOut, videoConfiguration *config, void *option, s32 blocking);
s32 videoGetResolutionAvailability(u32 videoOut, u32 resolutionId, u32 aspect, u32 option);
#endif
