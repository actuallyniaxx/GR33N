/* GR33N - capa de video sobre RSX/gcm */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>

#include <ppu-types.h>
#include <rsx/rsx.h>
#include <sysutil/video.h>

#include "gr33n.h"
#include "link.h"
#include "video.h"

/* Cabeceras generadas por el Makefile a partir de los shaders Cg
 * (shaders/fullscreen.vcg y shaders/fullscreen.fcg -> cgcomp -> bin2s) */
#include "fullscreen_vpo.h"
#include "fullscreen_fpo.h"

/* AL TTY Y AL LOG REMOTO, las dos cosas.
 *
 * Este fichero solo hacia printf, y arrancando desde el XMB no hay TTY por
 * ningun lado. Cuando la consola se salia al XMB con la salida en 1080p, lo
 * que se veia era exactamente nada. linkLog encola lo que se diga antes de
 * que aparezca el servidor de log, asi que llamar a esto desde el primer
 * momento es gratis. */
static void vlog(const char *fmt, ...)
{
	char buf[192];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	printf("[video] %s\n", buf);
	linkLog("[video] %s", buf);
}

#define CB_SIZE        0x100000            /* 1 MB de command buffer */
#define HOST_SIZE      (32*1024*1024)      /* memoria principal mapeada al RSX */
#define HOST_ALIGN     (1024*1024)
#define FB_COUNT       2

/* Indices de label. El 255 lo usan casi todos los ejemplos para sincronizar
 * el arranque; el 254 nos lo quedamos para la valla de la superficie. */
#define LABEL_SYNC     255
#define LABEL_SURFACE  254

gcmContextData *rsxctx = NULL;

static void *host_addr = NULL;
static videoResolution vres;

static u32  disp_w = 0, disp_h = 0;
static u32  color_pitch = 0;
static u32 *color_buffer[FB_COUNT];
static u32  color_offset[FB_COUNT];
static u32  depth_pitch = 0;
static u32 *depth_buffer = NULL;
static u32  depth_offset = 0;

static u32  curr_fb = 0;
static int  first_flip = 1;
static u32  label_val = 1;

/* Superficie de trabajo */
static gr33nSurface surface;
static u32 surface_offset = 0;
static u32 surface_fence = 0;
static int surface_main = 0;   /* 1 = memoria principal, 0 = local del RSX */

/* Shaders */
static rsxVertexProgram   *vp = NULL;
static rsxFragmentProgram *fp = NULL;
static void *vp_ucode = NULL;
static void *fp_ucode = NULL;
static u32   fp_offset = 0;
static rsxProgramAttrib *attr_pos = NULL;
static rsxProgramAttrib *attr_uv  = NULL;
static rsxProgramAttrib *unit_tex = NULL;
static u8 tex_unit = 0;   /* indice real de la unidad de textura */

/* Quad */
static f32 *quad_pos = NULL;
static f32 *quad_uv  = NULL;
static u32  quad_pos_offset = 0;
static u32  quad_uv_offset  = 0;

/* --------------------------------------------------------------------- */
/* Sincronizacion                                                        */
/* --------------------------------------------------------------------- */

static void waitFinish(void)
{
	rsxSetWriteBackendLabel(rsxctx, LABEL_SYNC, label_val);
	rsxFlushBuffer(rsxctx);

	while (*(vu32*)gcmGetLabelAddress(LABEL_SYNC) != label_val)
		usleep(30);

	++label_val;
}

static void waitRSXIdle(void)
{
	rsxSetWriteBackendLabel(rsxctx, LABEL_SYNC, label_val);
	rsxSetWaitLabel(rsxctx, LABEL_SYNC, label_val);
	++label_val;
	waitFinish();
}

/* --------------------------------------------------------------------- */
/* Configuracion de video                                                */
/* --------------------------------------------------------------------- */

/* Preferimos 720p porque es el formato objetivo del stream. Si el usuario
 * esta en SD, caemos a 576/480 y la superficie se escala en el RSX sin
 * coste extra: el quad ocupa la pantalla sea cual sea la resolucion. */
static const u32 res_pref[] = {
	VIDEO_RESOLUTION_720,
	VIDEO_RESOLUTION_576,
	VIDEO_RESOLUTION_480
};
#define RES_PREF_COUNT (sizeof(res_pref)/sizeof(res_pref[0]))

/* Espera a que el motor de video deje de estar cambiando de modo.
 *
 * LOS NOMBRES DE VIDEO_STATE_* DE PSL1GHT NO SON DE FIAR, y esto costo dos
 * ciclos de compilar-instalar-probar.
 *
 * La cabecera del toolchain dice:
 *
 *     VIDEO_STATE_DISABLED  0
 *     VIDEO_STATE_ENABLED   1
 *     VIDEO_STATE_BUSY      3
 *
 * Con la consola puesta en 1080p, videoGetState devuelve 0 nada mas
 * arrancar. Yo lo lei como "DISABLED", escribi "no hay salida de video" y
 * me sali al XMB... con la tele encendida enseñando el XMB delante de las
 * narices del usuario. La observacion desmiente el nombre: sea lo que sea
 * el 0, NO significa que no haya pantalla.
 *
 * (En el SDK oficial de Sony la lista es al reves - 0 ENABLED, 1 DISABLED,
 * 2 PREPARING - y los ejemplos de PSL1GHT tratan el 0 como el estado
 * bueno. O sea que lo que esta mal son los nombres de la cabecera, no el
 * firmware.)
 *
 * Asi que aqui NO se interpreta el numero. Se espera mientras valga alguno
 * de los que parecen transitorios, se registra tal cual, y se sigue pase
 * lo que pase. La unica comprobacion en la que se puede confiar es la de
 * despues: si videoGetResolution da un tamano y los framebuffers caben,
 * hay pantalla. Y si no la hubiera, fallarian esos.
 *
 * Esta es la misma leccion de cellPngDec por la puerta de al lado: leer la
 * cabecera es necesario y no es suficiente. Cuando la cabecera y el
 * hardware no se ponen de acuerdo, gana el hardware. */
static void video_wait_ready(const char *what)
{
	videoState st;
	int i;

	/* 2 segundos de techo. Un cambio de modo real tarda decimas. */
	for (i = 0; i < 200; i++) {
		if (videoGetState(VIDEO_PRIMARY, 0, &st) != 0) {
			vlog("videoGetState fallo (%s), sigo igual", what);
			return;
		}

		/* 3 es BUSY segun la cabecera y 2 es PREPARING segun el SDK
		 * oficial. Se esperan los dos: esperar de mas cuesta
		 * milisegundos, y no esperar costo una consola colgada. */
		if (st.state != 3 && st.state != 2) {
			if (i) vlog("%s: %d ms cambiando de modo (estado %u)",
			            what, i * 10, (unsigned)st.state);
			return;
		}

		usleep(10000);
	}

	vlog("%s: sigue en transicion a los 2 s (estado %u), sigo igual",
	     what, (unsigned)st.state);
}

static int initVideoConfiguration(void)
{
	videoConfiguration config;
	videoState st;
	u32 i;
	s32 res_id = 0;
	s32 rval;

	/* EN QUE MODO ESTA LA CONSOLA. Se apunta y no se juzga: ver arriba
	 * por que el numero de `state` no sirve para decidir nada. */
	if (videoGetState(VIDEO_PRIMARY, 0, &st) == 0)
		vlog("modo actual: resolucion %u, aspecto %u, estado %u",
		     (unsigned)st.displayMode.resolution,
		     (unsigned)st.displayMode.aspect,
		     (unsigned)st.state);
	else
		vlog("videoGetState fallo al arrancar, sigo igual");

	/* Por si el XMB acabara de soltar la pantalla. */
	video_wait_ready("al arrancar");

	/* QUE OFRECE LA CONSOLA, dicho entero. Cuesta cuatro llamadas y
	 * convierte "ninguna resolucion soportada" -que no explica nada- en
	 * una lista con la que se puede razonar. */
	{
		char av[96];
		int n = 0;

		av[0] = '\0';
		for (i = 0; i < RES_PREF_COUNT; i++) {
			s32 ok = videoGetResolutionAvailability(VIDEO_PRIMARY,
			                                        res_pref[i],
			                                        VIDEO_ASPECT_AUTO, 0);
			int k = snprintf(av + n, sizeof(av) - (size_t)n, "%s%u:%d",
			                 n ? " " : "", (unsigned)res_pref[i], (int)ok);
			if (k < 0 || n + k >= (int)sizeof(av)) break;
			n += k;

			if (ok == 1 && res_id == 0 &&
			    videoGetResolution(res_pref[i], &vres) == 0)
				res_id = (s32)res_pref[i];
		}

		vlog("disponibilidad (id:respuesta): %s", av);
	}

	/* SI NO NOS DA NINGUNA, NOS QUEDAMOS EN LA QUE YA HAY.
	 *
	 * Rendirse aqui era lo que sacaba al XMB, y era una rendicion tonta:
	 * la superficie de GR33N son 1280x720 fijos y el RSX la escala a lo
	 * que haya, asi que la resolucion de PANTALLA nos da bastante igual.
	 * Solo cambia lo que ocupan los framebuffers.
	 *
	 * Y ademas es mejor asi aunque hubiera alternativa: quedarse en el
	 * modo que el usuario ya tenia no le cambia el modo a la tele, no
	 * hace resincronizar el HDMI, y se salta entero el cambio de modo que
	 * es lo que colgaba la consola. */
	if (res_id == 0 && st.displayMode.resolution != 0 &&
	    videoGetResolution((s32)st.displayMode.resolution, &vres) == 0) {
		res_id = (s32)st.displayMode.resolution;
		vlog("ninguna de las mias esta disponible; me quedo en la que "
		     "hay (resolucion %d, %ux%u)",
		     (int)res_id, (unsigned)vres.width, (unsigned)vres.height);
	}

	if (res_id == 0) {
		vlog("ninguna resolucion soportada, ni la actual");
		return -1;
	}

	memset(&config, 0, sizeof(config));
	config.resolution = (u8)res_id;
	config.format     = VIDEO_BUFFER_FORMAT_XRGB;
	config.aspect     = VIDEO_ASPECT_AUTO;
	config.pitch      = (u32)vres.width * 4;

	/* EL ULTIMO PARAMETRO ES `blocking`, Y ESTABA EN 0.
	 *
	 * Eso es lo que congelaba la consola cuando la salida estaba puesta en
	 * 1080p: videoConfigure volvia enseguida, con el cambio de modo aun en
	 * marcha, y aqui debajo se reservaban los framebuffers y se empezaba a
	 * hacer flip contra un motor de video a medio reconfigurar.
	 *
	 * Desde 720p no pasaba nada porque pedir 720p estando en 720p no
	 * cambia ningun modo: no hay nada que esperar. El fallo llevaba ahi
	 * desde el primer dia y solo se veia en una configuracion concreta de
	 * la consola, que es la peor forma de tener un fallo.
	 *
	 * Y aun asi, despues se espera a mano: la propia cabecera de PSL1GHT
	 * dice "todo: verify the parameters signification" sobre este
	 * parametro, o sea que ni ellos las tienen todas consigo. */
	rval = videoConfigure(VIDEO_PRIMARY, &config, NULL, 1);
	if (rval) {
		vlog("videoConfigure fallo: %d", (int)rval);
		return -1;
	}

	video_wait_ready("tras configurar");

	/* Y AHORA SE PREGUNTA QUE MODO HAY DE VERDAD.
	 *
	 * No lo que hemos pedido: lo que el firmware dice que esta haciendo.
	 * Si nos hubiera dado otra cosa, vres seguiria teniendo el tamano que
	 * pedimos, el pitch saldria de ahi, y el RSX escribiria fuera del
	 * framebuffer. Que es otra forma de colgar la consola. */
	if (videoGetState(VIDEO_PRIMARY, 0, &st) != 0) {
		vlog("videoGetState fallo tras configurar");
		return -1;
	}

	if (st.displayMode.resolution != (u8)res_id) {
		vlog("pedi la resolucion %d y la consola dice %u; me quedo con "
		     "la suya", (int)res_id, (unsigned)st.displayMode.resolution);
		res_id = (s32)st.displayMode.resolution;
	}

	if (videoGetResolution(res_id, &vres) != 0) {
		vlog("no se que tamano tiene la resolucion %d",
		       (int)res_id);
		return -1;
	}

	disp_w = vres.width;
	disp_h = vres.height;

	if (!disp_w || !disp_h) {
		vlog("tamano de pantalla invalido: %ux%u",
		       (unsigned)disp_w, (unsigned)disp_h);
		return -1;
	}

	vlog("display %ux%u (resolucion %d)",
	       (unsigned)disp_w, (unsigned)disp_h, (int)res_id);
	return 0;
}

static void setRenderTarget(u32 index)
{
	gcmSurface sf;

	sf.colorFormat      = GCM_SURFACE_X8R8G8B8;
	sf.colorTarget      = GCM_SURFACE_TARGET_0;
	sf.colorLocation[0] = GCM_LOCATION_RSX;
	sf.colorOffset[0]   = color_offset[index];
	sf.colorPitch[0]    = color_pitch;

	sf.colorLocation[1] = GCM_LOCATION_RSX;
	sf.colorLocation[2] = GCM_LOCATION_RSX;
	sf.colorLocation[3] = GCM_LOCATION_RSX;
	sf.colorOffset[1]   = 0;
	sf.colorOffset[2]   = 0;
	sf.colorOffset[3]   = 0;
	sf.colorPitch[1]    = 64;
	sf.colorPitch[2]    = 64;
	sf.colorPitch[3]    = 64;

	sf.depthFormat      = GCM_SURFACE_ZETA_Z24S8;
	sf.depthLocation    = GCM_LOCATION_RSX;
	sf.depthOffset      = depth_offset;
	sf.depthPitch       = depth_pitch;

	sf.type             = GCM_SURFACE_TYPE_LINEAR;
	sf.antiAlias        = GCM_SURFACE_CENTER_1;

	sf.width            = disp_w;
	sf.height           = disp_h;
	sf.x                = 0;
	sf.y                = 0;

	rsxSetSurface(rsxctx, &sf);
}

/* --------------------------------------------------------------------- */
/* Shaders y geometria                                                   */
/* --------------------------------------------------------------------- */

static int initShaders(void)
{
	void *ucode = NULL;
	u32 ucode_size = 0;

	vp = (rsxVertexProgram*)fullscreen_vpo;
	fp = (rsxFragmentProgram*)fullscreen_fpo;

	/* El microcodigo del vertex program lo consume el RSX desde memoria
	 * principal; el del fragment program TIENE que estar en memoria local
	 * del RSX, asi que lo copiamos. */
	rsxVertexProgramGetUCode(vp, &vp_ucode, &ucode_size);

	rsxFragmentProgramGetUCode(fp, &ucode, &ucode_size);
	fp_ucode = rsxMemalign(64, ucode_size);
	if (!fp_ucode) {
		printf("[video] sin memoria para el fragment ucode\n");
		return -1;
	}
	memcpy(fp_ucode, ucode, ucode_size);
	rsxAddressToOffset(fp_ucode, &fp_offset);

	attr_pos = rsxVertexProgramGetAttrib(vp, "position");
	attr_uv  = rsxVertexProgramGetAttrib(vp, "texcoord");

	if (!attr_pos || !attr_uv) {
		printf("[video] atributos del vertex program no encontrados (pos=%p uv=%p)\n",
		       (void*)attr_pos, (void*)attr_uv);
		return -1;
	}

	/* La unidad de textura si la damos por 0 cuando cgcomp no conserva el
	 * nombre: solo usamos una, y arrancar con un aviso es mejor que no
	 * arrancar. Si algun dia hay varias texturas, esto hay que revisarlo. */
	unit_tex = rsxFragmentProgramGetAttrib(fp, "srcTex");
	if (unit_tex) {
		tex_unit = (u8)unit_tex->index;
	} else {
		tex_unit = 0;
		printf("[video] AVISO: 'srcTex' no encontrado en el fragment program, "
		       "usando unidad 0\n");
	}

	return 0;
}

static int initQuad(void)
{
	/* Triangle strip: TL, TR, BL, BR.
	 * Clip space tiene +Y arriba; la textura tiene V=0 arriba. */
	static const f32 pos[4*3] = {
		-1.0f,  1.0f, 0.0f,
		 1.0f,  1.0f, 0.0f,
		-1.0f, -1.0f, 0.0f,
		 1.0f, -1.0f, 0.0f
	};
	static const f32 uv[4*2] = {
		0.0f, 0.0f,
		1.0f, 0.0f,
		0.0f, 1.0f,
		1.0f, 1.0f
	};

	quad_pos = (f32*)rsxMemalign(128, sizeof(pos));
	quad_uv  = (f32*)rsxMemalign(128, sizeof(uv));
	if (!quad_pos || !quad_uv) {
		printf("[video] sin memoria para el quad\n");
		return -1;
	}

	memcpy(quad_pos, pos, sizeof(pos));
	memcpy(quad_uv,  uv,  sizeof(uv));

	rsxAddressToOffset(quad_pos, &quad_pos_offset);
	rsxAddressToOffset(quad_uv,  &quad_uv_offset);
	return 0;
}

static int initSurface(void)
{
	u32 bytes;

	surface.width  = GR33N_SURFACE_W;
	surface.height = GR33N_SURFACE_H;
	surface.pitch  = GR33N_SURFACE_W * 4;

	bytes = surface.pitch * surface.height;

	/* LA MUDANZA. Las escrituras de la PPU a memoria principal son
	 * cacheadas; contra memoria local del RSX son write-combined, y ahi
	 * las escrituras DISPERSAS se desploman.
	 *
	 * Medido en hardware real con la superficie en VRAM: la carta de
	 * ajuste, que se pinta con memcpy secuencial, da 60 fps. El menu, que
	 * pinta miles de rectangulos de 2x2 para los glifos, se queda en 34.
	 * Mismo volumen de bytes, veinte veces mas lento por estar disperso.
	 *
	 * RPCS3 nunca lo vio porque su memoria de RSX es RAM del host.
	 *
	 * gcmMapMainMemory quiere alineacion y tamano de 1 MB. */
	{
		u32 mapped = (bytes + (1024*1024) - 1) & ~((u32)(1024*1024) - 1);

		surface.pixels = (u32*)memalign(1024*1024, mapped);
		if (surface.pixels &&
		    gcmMapMainMemory(surface.pixels, mapped, &surface_offset) == 0) {
			surface_main = 1;
		} else {
			/* Si el mapeo no cuela, atras al camino conocido: mejor
			 * lento que sin arrancar. */
			if (surface.pixels) { free(surface.pixels); surface.pixels = NULL; }

			surface.pixels = (u32*)rsxMemalign(128, bytes);
			if (!surface.pixels) {
				printf("[video] sin memoria para la superficie (%u bytes)\n",
				       (unsigned)bytes);
				return -1;
			}
			rsxAddressToOffset(surface.pixels, &surface_offset);
			surface_main = 0;
		}
	}

	memset(surface.pixels, 0, bytes);

	/* Valla en 0: el primer videoSurface() no espera a nadie. */
	surface_fence = 0;
	*(vu32*)gcmGetLabelAddress(LABEL_SURFACE) = surface_fence;

	printf("[video] superficie %ux%u (%u KB) en memoria %s\n",
	       (unsigned)surface.width, (unsigned)surface.height,
	       (unsigned)(bytes/1024),
	       surface_main ? "PRINCIPAL (cacheada)" : "local del RSX");
	return 0;
}

/* --------------------------------------------------------------------- */
/* Estado de dibujo                                                      */
/* --------------------------------------------------------------------- */

static void setDrawEnv(void)
{
	f32 scale[4], offset[4];

	rsxSetColorMask(rsxctx, GCM_COLOR_MASK_B | GCM_COLOR_MASK_G |
	                        GCM_COLOR_MASK_R | GCM_COLOR_MASK_A);
	rsxSetColorMaskMrt(rsxctx, 0);

	/* scale[1] negativo = Y invertida, para que clip +1 caiga arriba. */
	scale[0]  = (f32)disp_w *  0.5f;
	scale[1]  = (f32)disp_h * -0.5f;
	scale[2]  = 0.5f;
	scale[3]  = 0.0f;
	offset[0] = (f32)disp_w * 0.5f;
	offset[1] = (f32)disp_h * 0.5f;
	offset[2] = 0.5f;
	offset[3] = 0.0f;

	rsxSetViewport(rsxctx, 0, 0, (u16)disp_w, (u16)disp_h, 0.0f, 1.0f, scale, offset);
	rsxSetScissor(rsxctx, 0, 0, (u16)disp_w, (u16)disp_h);

	/* Un quad 2D no necesita nada de esto. Apagarlo explicitamente evita
	 * heredar estado raro entre frames. */
	rsxSetDepthTestEnable(rsxctx, GCM_FALSE);
	rsxSetDepthWriteEnable(rsxctx, GCM_FALSE);
	rsxSetCullFaceEnable(rsxctx, GCM_FALSE);
	rsxSetBlendEnable(rsxctx, GCM_FALSE);
	rsxSetShadeModel(rsxctx, GCM_SHADE_MODEL_SMOOTH);
	rsxSetFrontFace(rsxctx, GCM_FRONTFACE_CCW);
	rsxSetUserClipPlaneControl(rsxctx,
		GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE,
		GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE,
		GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE);
}

static void bindSurfaceTexture(void)
{
	gcmTexture tex;

	rsxInvalidateTextureCache(rsxctx, GCM_INVALIDATE_TEXTURE);

	tex.format    = GCM_TEXTURE_FORMAT_A8R8G8B8 | GCM_TEXTURE_FORMAT_LIN;
	tex.mipmap    = 1;
	tex.dimension = GCM_TEXTURE_DIMS_2D;
	tex.cubemap   = GCM_FALSE;
	tex.remap     = ((GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_B_SHIFT) |
	                 (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_G_SHIFT) |
	                 (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_R_SHIFT) |
	                 (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_A_SHIFT) |
	                 (GCM_TEXTURE_REMAP_COLOR_B << GCM_TEXTURE_REMAP_COLOR_B_SHIFT) |
	                 (GCM_TEXTURE_REMAP_COLOR_G << GCM_TEXTURE_REMAP_COLOR_G_SHIFT) |
	                 (GCM_TEXTURE_REMAP_COLOR_R << GCM_TEXTURE_REMAP_COLOR_R_SHIFT) |
	                 (GCM_TEXTURE_REMAP_COLOR_A << GCM_TEXTURE_REMAP_COLOR_A_SHIFT));
	tex.width     = surface.width;
	tex.height    = surface.height;
	tex.depth     = 1;
	tex.location  = surface_main ? GCM_LOCATION_CELL : GCM_LOCATION_RSX;
	tex.pitch     = surface.pitch;
	tex.offset    = surface_offset;

	rsxLoadTexture(rsxctx, tex_unit, &tex);

	rsxTextureControl(rsxctx, tex_unit, GCM_TRUE, 0 << 8, 12 << 8,
	                  GCM_TEXTURE_MAX_ANISO_1);
	rsxTextureFilter(rsxctx, tex_unit, 0,
	                 GCM_TEXTURE_LINEAR, GCM_TEXTURE_LINEAR,
	                 GCM_TEXTURE_CONVOLUTION_QUINCUNX);
	rsxTextureWrapMode(rsxctx, tex_unit,
	                   GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE,
	                   GCM_TEXTURE_CLAMP_TO_EDGE, 0, GCM_TEXTURE_ZFUNC_LESS, 0);
}

static void flip(void)
{
	if (!first_flip) {
		while (gcmGetFlipStatus() != 0)
			usleep(200);
	}
	gcmResetFlipStatus();

	gcmSetFlip(rsxctx, curr_fb);
	rsxFlushBuffer(rsxctx);
	gcmSetWaitFlip(rsxctx);

	curr_fb ^= 1;
	setRenderTarget(curr_fb);

	first_flip = 0;
}

/* --------------------------------------------------------------------- */
/* API publica                                                           */
/* --------------------------------------------------------------------- */

int videoInit(void)
{
	u32 i;

	host_addr = memalign(HOST_ALIGN, HOST_SIZE);
	if (!host_addr) {
		printf("[video] sin memoria para el host buffer\n");
		return -1;
	}

	if (rsxInit(&rsxctx, CB_SIZE, HOST_SIZE, host_addr) < 0) {
		printf("[video] rsxInit fallo\n");
		return -1;
	}

	if (initVideoConfiguration() != 0)
		return -1;

	waitRSXIdle();

	gcmSetFlipMode(GCM_FLIP_VSYNC);

	color_pitch = disp_w * 4;
	for (i = 0; i < FB_COUNT; i++) {
		color_buffer[i] = (u32*)rsxMemalign(64, disp_h * color_pitch);
		if (!color_buffer[i]) {
			vlog("sin memoria para el framebuffer %u (%ux%u, %u KB)", (unsigned)i,
			     (unsigned)disp_w, (unsigned)disp_h,
			     (unsigned)((disp_h * color_pitch) / 1024));
			return -1;
		}
		rsxAddressToOffset(color_buffer[i], &color_offset[i]);
		gcmSetDisplayBuffer(i, color_offset[i], color_pitch, disp_w, disp_h);
	}

	/* No usamos depth para nada, pero gcmSurface exige un zeta valido.
	 * Z24S8 con pitch = ancho*4 porque el pitch tiene que ser multiplo de
	 * 64: con Z16 (ancho*2) las resoluciones SD de 720 px no cuadran. */
	depth_pitch  = disp_w * 4;
	depth_buffer = (u32*)rsxMemalign(64, disp_h * depth_pitch);
	if (!depth_buffer) {
		vlog("sin memoria para el depth buffer (%u KB)",
		     (unsigned)((disp_h * depth_pitch) / 1024));
		return -1;
	}
	rsxAddressToOffset(depth_buffer, &depth_offset);

	if (initShaders() != 0) return -1;
	if (initQuad()    != 0) return -1;
	if (initSurface() != 0) return -1;

	curr_fb = 0;
	first_flip = 1;
	setRenderTarget(curr_fb);

	return 0;
}

void videoShutdown(void)
{
	if (!rsxctx) return;

	/* Ojo: NO liberamos memoria del RSX aqui. Liberar memoria de video
	 * fuera del hilo de dibujo cuelga la consola en duro, y el proceso al
	 * salir la recupera igual. */
	gcmSetWaitFlip(rsxctx);
	rsxFinish(rsxctx, 1);
	rsxctx = NULL;
}

gr33nSurface *videoSurface(void)
{
	while (*(vu32*)gcmGetLabelAddress(LABEL_SURFACE) != surface_fence)
		usleep(10);

	return &surface;
}

void videoPresent(void)
{
	setDrawEnv();

	rsxSetClearColor(rsxctx, 0x00000000);
	rsxSetClearDepthStencil(rsxctx, 0xffffffff);
	rsxClearSurface(rsxctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B |
	                        GCM_CLEAR_A | GCM_CLEAR_Z | GCM_CLEAR_S);

	rsxLoadVertexProgram(rsxctx, vp, vp_ucode);
	rsxLoadFragmentProgramLocation(rsxctx, fp, fp_offset, GCM_LOCATION_RSX);

	bindSurfaceTexture();

	rsxBindVertexArrayAttrib(rsxctx, attr_pos->index, 0, quad_pos_offset,
	                         sizeof(f32) * 3, 3, GCM_VERTEX_DATA_TYPE_F32,
	                         GCM_LOCATION_RSX);
	rsxBindVertexArrayAttrib(rsxctx, attr_uv->index, 0, quad_uv_offset,
	                         sizeof(f32) * 2, 2, GCM_VERTEX_DATA_TYPE_F32,
	                         GCM_LOCATION_RSX);

	rsxDrawVertexArray(rsxctx, GCM_TYPE_TRIANGLE_STRIP, 0, 4);
	rsxInvalidateVertexCache(rsxctx);

	/* Valla: cuando el RSX llegue aqui, ha terminado de leer la textura.
	 * videoSurface() espera a este valor antes de dejar escribir. */
	++surface_fence;
	rsxSetWriteBackendLabel(rsxctx, LABEL_SURFACE, surface_fence);

	flip();
}

u32 videoDisplayWidth(void)  { return disp_w; }
u32 videoDisplayHeight(void) { return disp_h; }
int videoSurfaceInMain(void) { return surface_main; }
