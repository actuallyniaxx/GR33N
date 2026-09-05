/* GR33N - PNG a pixeles con cellPngDec
 *
 * Mismo guion que cellVdec, y por los mismos motivos:
 *
 *   1. Cargar el modulo del sistema y REGISTRAR que devolvio. Sin esto,
 *      cuando la primera llamada falle con 0x8061120x no vas a saber si es
 *      que el PNG esta mal o que el decodificador no estaba cargado.
 *   2. Los punteros a funcion de la estructura de hilos llevan
 *      ATTRIBUTE_PRXPTR: no es la direccion de la funcion lo que hay que
 *      pasar, es la primera palabra de su descriptor. __get_opd32 hace la
 *      conversion. Sin ella se le pasa basura al firmware.
 *   3. Preguntar el tamano ANTES de decodificar y rechazar lo que no
 *      quepa. Una biblioteca del sistema escribiendo fuera de un buffer no
 *      da un error: cuelga la consola.
 *
 * El decodificador puede tirar de SPUs. Para una imagen de 400x400 y una
 * vez por sesion no compensa reservar una SPU, asi que va en PPU. Cuando
 * haya que decodificar veinte caratulas de golpe, se cambia y se mide,
 * igual que se hizo con la escalera de SPUs del video.
 *
 * PSL1GHT trae ademas pngLoadFromBuffer(), que hace todo esto en una
 * llamada. No se usa por dos motivos: reserva ella el buffer de salida (y
 * aqui interesa uno fijo, sin malloc en el camino) y no deja elegir el
 * formato ni mirar el tamano ANTES de decodificar. Si algun dia esto da
 * guerra, ahi esta como plan B.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include <ppu-types.h>
#include <ppu-asm.h>          /* __get_opd32 */
#include <sys/systime.h>
#include <sys/mutex.h>
#include <sysmodule/sysmodule.h>
#include <pngdec/pngdec.h>
#include <jpgdec/jpgdec.h>

#include "imgdec.h"
#include "link.h"

/* --------------------------------------------------------------------- */

/* 512x512 en ARGB. Un megabyte largo de BSS, que en 256 MB de memoria
 * principal no es nada, y a cambio no hay malloc en el camino ni un
 * puntero que pueda quedar colgando. */
static u32 pixels[IMG_MAX_W * IMG_MAX_H];

/* UN buffer de salida y UN manejador del firmware, y ahora DOS hilos.
 *
 * Cuando esto se escribio solo lo llamaba el hilo de sesion, una vez, para
 * el avatar. Hoy lo llama tambien el hilo del catalogo, una caratula
 * detras de otra, y en el arranque se solapan de verdad: el catalogo ya
 * esta leido del disco y art_pump esta decodificando JPEGs mientras la
 * cadena del perfil termina y decodifica el PNG del avatar. Dos hilos
 * escribiendo en pixels[] y abriendo el mismo manejador.
 *
 * El candado se coge en imgDecodeTo y se suelta ahi mismo, con la copia ya
 * hecha. Fuera de esa funcion nadie toca pixels[]. */
static sys_mutex_t mtx;
static int mtx_ok = 0;

/* Las dos lineas de montaje por imagen -formato, divisor, bytes por linea-
 * sirven la primera vez y estorban a partir de la decima.
 *
 * MEDIDO: en una sesion de 310 s el log traia 7641 lineas, y de esas 1800
 * eran esto, tres por cada caratula. Un log que no se puede leer es un log
 * que no se lee, y este es el unico instrumento que hay para depurar en una
 * consola sin consola. La linea de "decodificado en N ms" se queda: esa es
 * la medida. */
#define IMG_DETALLE  8
static u32 img_verbose = 0;

#define idlog(...)  do { if (img_verbose < IMG_DETALLE) ilog(__VA_ARGS__); } while (0)

#define ILOCK()    do { if (mtx_ok) sysMutexLock(mtx, 0); } while (0)
#define IUNLOCK()  do { if (mtx_ok) sysMutexUnlock(mtx); } while (0)

/* s32, no u32: asi lo declaran pngDecCreate y pngDecOpen. */
static s32 main_handle = 0;
static int mod_loaded  = 0;
static int have_main   = 0;

/* El decodificador mira aqui entre linea y linea por si le mandan parar.
 * Se le da una variable de verdad y no NULL: no esta escrito en ningun
 * sitio que acepte NULL, y una biblioteca del sistema desreferenciando
 * cero no da un error, apaga la consola. Cuesta cuatro bytes. */
static vu32 png_cmd = PNGDEC_CONTINUE;

/* El decodificador de JPEG es OTRO modulo y OTRO manejador. Casi la misma
 * API -las firmas coinciden una a una- pero las estructuras NO son las
 * mismas y ahi esta la trampa:
 *
 *   jpgDecInfo      no tiene bit_depth ni interlace_mode. Cuatro campos.
 *   jpgDecInParam   no tiene bit_depth, pack_flag ni alpha_select.
 *                   A cambio trae down_scale y quality_mode.
 *                   Y alpha es u8, no u32.
 *   jpgDecOutParam  se llama width_bytes, con ese, no width_byte.
 *
 * Copiar el fichero de PNG y cambiar png por jpg habria compilado casi
 * entero y roto en sitios raros. */
static s32  jpg_handle = 0;
static int  jpg_mod    = 0;
static int  have_jpg   = 0;
static vu32 jpg_cmd    = JPGDEC_CONTINUE;

static void ilog(const char *fmt, ...)
{
	char buf[192];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	printf("[img] %s\n", buf);
	linkLog("[img] %s", buf);
}

/* Igual que en decoder.c: __get_opd32 lleva dentro una comprobacion contra
 * nulo y GCC avisa de que la direccion de una funcion nunca es nula. El
 * aviso tiene razon y no sirve de nada, asi que se pasa por un void*. */
static u32 opd32_of(const void *fn)
{
	return (u32)__get_opd32(fn);
}

/* El decodificador pide y suelta memoria por su cuenta a traves de estas
 * dos. Con malloc del sistema vale. */
static void *png_malloc(u32 size, void *arg)
{
	(void)arg;
	return malloc(size);
}

static void png_free(void *ptr, void *arg)
{
	(void)arg;
	free(ptr);
}

static u64 now_us(void)
{
	u64 sec = 0, nsec = 0;
	sysGetCurrentTime(&sec, &nsec);
	return sec * 1000000ull + nsec / 1000ull;
}

/* --------------------------------------------------------------------- */

int imgInit(void)
{
	pngDecThreadInParam  tin;
	pngDecThreadOutParam tout;
	s32 ret;

	if (have_main) return 0;

	if (!mtx_ok) {
		sys_mutex_attr_t attr;
		sysMutexAttrInitialize(attr);
		if (sysMutexCreate(&mtx, &attr) == 0) mtx_ok = 1;
		else ilog("!! sin mutex: no decodifiques desde dos hilos");
	}

	ret = sysModuleLoad(SYSMODULE_PNGDEC);
	ilog("sysModuleLoad(PNGDEC) -> 0x%08x%s", (unsigned)ret,
	     (ret == 0 || (u32)ret == SYSMODULE_ERR_DUPLICATE) ? " ok" : " FALLO");

	if (ret != 0 && (u32)ret != SYSMODULE_ERR_DUPLICATE)
		return -1;

	mod_loaded = 1;

	memset(&tin,  0, sizeof(tin));
	memset(&tout, 0, sizeof(tout));

	tin.spu_enable  = PNGDEC_SPU_THREAD_DISABLE;
	tin.ppu_prio    = 1000;
	tin.spu_prio    = 200;

	/* __typeof__ del CAMPO, no el typedef.
	 *
	 * Aqui los punteros son de 64 bits, pero estos dos campos llevan
	 * ATTRIBUTE_PRXPTR, que los deja en 32 porque es lo que quiere el
	 * firmware. Castear al typedef pngCbCtrlMalloc da el puntero ancho y
	 * GCC avisa de que el entero y el puntero no miden lo mismo; con
	 * __typeof__ se coge el tipo real del campo, ya estrechado, y la
	 * conversion es exacta. */
	tin.malloc_func = (__typeof__(tin.malloc_func))opd32_of(png_malloc);
	tin.malloc_arg  = NULL;
	tin.free_func   = (__typeof__(tin.free_func))opd32_of(png_free);
	tin.free_arg    = NULL;

	ret = pngDecCreate(&main_handle, &tin, &tout);
	if (ret != 0) {
		ilog("pngDecCreate: 0x%08x", (unsigned)ret);
		sysModuleUnload(SYSMODULE_PNGDEC);
		mod_loaded = 0;
		return -1;
	}

	have_main = 1;
	ilog("PNG listo (version 0x%08x)", (unsigned)tout.version);

	/* Y el de JPEG. Si este falla NO se cae todo: un avatar en PNG sigue
	 * funcionando aunque las caratulas en JPEG no. */
	{
		jpgDecThreadInParam  jin;
		jpgDecThreadOutParam jout;

		ret = sysModuleLoad(SYSMODULE_JPGDEC);
		ilog("sysModuleLoad(JPGDEC) -> 0x%08x%s", (unsigned)ret,
		     (ret == 0 || (u32)ret == SYSMODULE_ERR_DUPLICATE) ? " ok" : " FALLO");

		if (ret == 0 || (u32)ret == SYSMODULE_ERR_DUPLICATE) {
			jpg_mod = 1;

			memset(&jin,  0, sizeof(jin));
			memset(&jout, 0, sizeof(jout));

			jin.spu_enable  = JPGDEC_SPU_THREAD_DISABLE;
			jin.ppu_prio    = 1000;
			jin.spu_prio    = 200;
			jin.malloc_func = (__typeof__(jin.malloc_func))opd32_of(png_malloc);
			jin.malloc_arg  = NULL;
			jin.free_func   = (__typeof__(jin.free_func))opd32_of(png_free);
			jin.free_arg    = NULL;

			ret = jpgDecCreate(&jpg_handle, &jin, &jout);
			if (ret != 0) {
				ilog("jpgDecCreate: 0x%08x - sin caratulas JPEG",
				     (unsigned)ret);
				sysModuleUnload(SYSMODULE_JPGDEC);
				jpg_mod = 0;
			} else {
				have_jpg = 1;
				ilog("JPEG listo (version 0x%08x)", (unsigned)jout.version);
			}
		}
	}

	return 0;
}

void imgShutdown(void)
{
	if (have_main) {
		pngDecDestroy(main_handle);
		have_main = 0;
	}
	if (have_jpg) {
		jpgDecDestroy(jpg_handle);
		have_jpg = 0;
	}
	if (mod_loaded) {
		sysModuleUnload(SYSMODULE_PNGDEC);
		mod_loaded = 0;
	}
	if (jpg_mod) {
		sysModuleUnload(SYSMODULE_JPGDEC);
		jpg_mod = 0;
	}
	if (mtx_ok) { sysMutexDestroy(mtx); mtx_ok = 0; }
}

/* --------------------------------------------------------------------- */

static void ifail(imgImage *out, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(out->err, sizeof(out->err), fmt, ap);
	va_end(ap);

	ilog("%s", out->err);
}

int imgDecodePNG(const void *png, u32 len, imgImage *out)
{
	pngDecSource        src;
	pngDecOpnInfo       opn;
	pngDecInfo          hdr;
	pngDecInParam       ip;
	pngDecOutParam      op;
	pngDecDataCtrlParam ctl;
	pngDecDataInfo      dinfo;

	s32 sub = 0;
	u64 t0;
	s32 ret;
	int opened = 0;

	if (!out) return -1;

	memset(out, 0, sizeof(*out));
	out->src_bytes = len;

	if (!have_main) {
		ifail(out, "el decodificador de PNG no esta abierto");
		return -1;
	}

	if (!png || len < 8) {
		ifail(out, "no hay PNG que decodificar (%u bytes)", (unsigned)len);
		return -1;
	}

	/* La firma, antes de molestar al firmware. Si el servidor contesto una
	 * pagina de error en HTML en vez de la imagen, esto lo dice en una
	 * linea; el decodificador lo diria con un numero hexadecimal. */
	if (memcmp(png, "\x89PNG\r\n\x1a\n", 8) != 0) {
		const unsigned char *b = (const unsigned char*)png;
		ifail(out, "esto no es un PNG (empieza por %02x %02x %02x %02x)",
		      b[0], b[1], b[2], b[3]);
		return -1;
	}

	t0 = now_us();

	memset(&src, 0, sizeof(src));
	src.stream_sel  = PNGDEC_BUFFER;
	src.stream_ptr  = (void*)png;
	src.stream_size = len;
	src.spu_enable  = PNGDEC_SPU_THREAD_DISABLE;

	memset(&opn, 0, sizeof(opn));

	ret = pngDecOpen(main_handle, &sub, &src, &opn);
	if (ret != 0) {
		ifail(out, "pngDecOpen: 0x%08x", (unsigned)ret);
		return -1;
	}
	opened = 1;

	memset(&hdr, 0, sizeof(hdr));

	ret = pngDecReadHeader(main_handle, sub, &hdr);
	if (ret != 0) {
		ifail(out, "pngDecReadHeader: 0x%08x", (unsigned)ret);
		goto out;
	}

	idlog("PNG %ux%u, %u bits, espacio %u, %u componentes, entrelazado %u"
	     " (reservados %u bytes al abrir)",
	     (unsigned)hdr.width, (unsigned)hdr.height,
	     (unsigned)hdr.bit_depth, (unsigned)hdr.color_space,
	     (unsigned)hdr.num_comp, (unsigned)hdr.interlace_mode,
	     (unsigned)opn.init_space_allocated);

	/* LA COMPROBACION QUE IMPORTA. Aqui todavia no se ha escrito nada. */
	if (hdr.width == 0 || hdr.height == 0 ||
	    hdr.width > IMG_MAX_W || hdr.height > IMG_MAX_H) {
		ifail(out, "imagen de %ux%u, el tope son %ux%u",
		      (unsigned)hdr.width, (unsigned)hdr.height,
		      (unsigned)IMG_MAX_W, (unsigned)IMG_MAX_H);
		goto out;
	}

	memset(&ip, 0, sizeof(ip));
	memset(&op, 0, sizeof(op));

	/* ARGB de 8 bits por componente, de arriba a abajo y un byte por
	 * componente. Es exactamente el formato de la superficie: no hay
	 * conversion despues, se copia y ya.
	 *
	 * PNGDEC_FIX_ALPHA con 0xff porque un avatar sin canal alfa (los hay,
	 * los de color plano) llegaria con transparencia cero y se veria un
	 * agujero donde deberia estar la cara. */
	ip.cmd_ptr     = &png_cmd;
	ip.output_mode = PNGDEC_TOP_TO_BOTTOM;
	ip.color_space = PNGDEC_ARGB;
	ip.bit_depth   = 8;
	ip.pack_flag   = PNGDEC_1BYTE_PER_1PIXEL;
	ip.alpha_select = PNGDEC_FIX_ALPHA;
	ip.alpha        = 0xff;

	ret = pngDecSetParameter(main_handle, sub, &ip, &op);
	if (ret != 0) {
		ifail(out, "pngDecSetParameter: 0x%08x", (unsigned)ret);
		goto out;
	}

	/* Y se vuelve a comprobar con lo que el decodificador dice que va a
	 * sacar, que no tiene por que coincidir con la cabecera. */
	if (op.width == 0 || op.height == 0 ||
	    op.width > IMG_MAX_W || op.height > IMG_MAX_H) {
		ifail(out, "la salida seria de %ux%u, no cabe",
		      (unsigned)op.width, (unsigned)op.height);
		goto out;
	}

	if (op.num_comp != 4) {
		ifail(out, "la salida tiene %u componentes, se esperaban 4",
		      (unsigned)op.num_comp);
		goto out;
	}

	/* width_byte es lo que el decodificador dice que necesita por linea.
	 * Nosotros le damos ancho*4 justo, sin relleno, porque imgShrink lee
	 * las filas seguidas. Si pidiera mas, escribiria fuera de cada fila y
	 * la imagen saldria en diagonal - o peor. */
	if (op.width_byte > (u64)op.width * 4ull) {
		ifail(out, "el decodificador pide %u bytes por linea y le damos %u",
		      (unsigned)op.width_byte, (unsigned)(op.width * 4));
		goto out;
	}

	idlog("salida %ux%u, %u bytes por linea, %u bytes de trabajo",
	     (unsigned)op.width, (unsigned)op.height,
	     (unsigned)op.width_byte, (unsigned)op.use_memory_space);

	memset(&ctl,   0, sizeof(ctl));
	memset(&dinfo, 0, sizeof(dinfo));

	ctl.output_bytes_per_line = (u64)op.width * 4ull;

	png_cmd = PNGDEC_CONTINUE;

	ret = pngDecDecodeData(main_handle, sub, (u8*)pixels, &ctl, &dinfo);
	if (ret != 0) {
		ifail(out, "pngDecDecodeData: 0x%08x", (unsigned)ret);
		goto out;
	}

	if (dinfo.decode_status != PNGDEC_STATUS_FINISH) {
		ifail(out, "el decodificador se paro a medias (estado %u)",
		      (unsigned)dinfo.decode_status);
		goto out;
	}

	out->px = pixels;
	out->w  = op.width;
	out->h  = op.height;
	out->ms = (u32)((now_us() - t0) / 1000);

	img_verbose++;
	ilog("decodificado %ux%u en %u ms (%u bytes de PNG)",
	     (unsigned)out->w, (unsigned)out->h, (unsigned)out->ms,
	     (unsigned)len);

	pngDecClose(main_handle, sub);
	return 0;

out:
	if (opened) pngDecClose(main_handle, sub);
	return -1;
}

/* --------------------------------------------------------------------- */
/* JPEG                                                                  */
/* --------------------------------------------------------------------- */

int imgDecodeJPG(const void *jpg, u32 len, imgImage *out)
{
	jpgDecSource        src;
	jpgDecOpnInfo       opn;
	jpgDecInfo          hdr;
	jpgDecInParam       ip;
	jpgDecOutParam      op;
	jpgDecDataCtrlParam ctl;
	jpgDecDataInfo      dinfo;

	s32 sub = 0;
	u32 ds  = 1;
	u64 t0;
	s32 ret;
	int opened = 0;

	if (!out) return -1;

	memset(out, 0, sizeof(*out));
	out->src_bytes = len;

	if (!have_jpg) {
		ifail(out, "el decodificador de JPEG no esta abierto");
		return -1;
	}

	if (!jpg || len < 4) {
		ifail(out, "no hay JPEG que decodificar (%u bytes)", (unsigned)len);
		return -1;
	}

	{
		const unsigned char *b = (const unsigned char*)jpg;
		if (b[0] != 0xff || b[1] != 0xd8 || b[2] != 0xff) {
			ifail(out, "esto no es un JPEG (empieza por %02x %02x %02x)",
			      b[0], b[1], b[2]);
			return -1;
		}
	}

	t0 = now_us();

	memset(&src, 0, sizeof(src));
	src.stream_sel  = JPGDEC_BUFFER;
	src.stream_ptr  = (void*)jpg;
	src.stream_size = len;
	src.spu_enable  = JPGDEC_SPU_THREAD_DISABLE;

	memset(&opn, 0, sizeof(opn));

	ret = jpgDecOpen(jpg_handle, &sub, &src, &opn);
	if (ret != 0) {
		ifail(out, "jpgDecOpen: 0x%08x", (unsigned)ret);
		return -1;
	}
	opened = 1;

	memset(&hdr, 0, sizeof(hdr));

	ret = jpgDecReadHeader(jpg_handle, sub, &hdr);
	if (ret != 0) {
		ifail(out, "jpgDecReadHeader: 0x%08x", (unsigned)ret);
		goto out;
	}

	/* AQUI ESTA EL REGALO DE ESTA API.
	 *
	 * jpgDecInParam tiene down_scale, que el de PNG no tiene: el
	 * decodificador reduce MIENTRAS descomprime. Una caratula de 1080x1080
	 * no cabe en el buffer de 512x512 y con PNG habria que rechazarla;
	 * aqui se pide a la cuarta parte y cabe de sobra, sin buffer
	 * intermedio y descomprimiendo menos.
	 *
	 * Se elige el divisor mas pequeno que haga que quepa. Los valores que
	 * se prueban son 1, 2, 4 y 8 porque son los que el JPEG puede dar sin
	 * remuestrear (son submultiplos del bloque de 8x8); si el firmware
	 * acepta otros, ya lo dira op.down_scale en el log. */
	while (ds < 8 &&
	       ((hdr.width + ds - 1) / ds > IMG_MAX_W ||
	        (hdr.height + ds - 1) / ds > IMG_MAX_H))
		ds *= 2;

	idlog("JPEG %ux%u, %u componentes, espacio %u -> divisor %u "
	     "(reservados %u bytes al abrir)",
	     (unsigned)hdr.width, (unsigned)hdr.height, (unsigned)hdr.num_comp,
	     (unsigned)hdr.color_space, (unsigned)ds,
	     (unsigned)opn.init_space_allocated);

	if (hdr.width == 0 || hdr.height == 0 ||
	    (hdr.width + ds - 1) / ds > IMG_MAX_W ||
	    (hdr.height + ds - 1) / ds > IMG_MAX_H) {
		ifail(out, "imagen de %ux%u, ni dividida por 8 cabe en %ux%u",
		      (unsigned)hdr.width, (unsigned)hdr.height,
		      (unsigned)IMG_MAX_W, (unsigned)IMG_MAX_H);
		goto out;
	}

	memset(&ip, 0, sizeof(ip));
	memset(&op, 0, sizeof(op));

	ip.cmd_ptr      = &jpg_cmd;
	ip.down_scale   = ds;
	ip.quality_mode = JPGDEC_QUALITY;
	ip.output_mode  = JPGDEC_TOP_TO_BOTTOM;
	ip.color_space  = JPGDEC_ARGB;
	/* u8, no u32: en JPEG no hay canal alfa, asi que esto es el relleno
	 * constante. Sin 0xff la caratula sale transparente, o sea negra. */
	ip.alpha        = 0xff;

	ret = jpgDecSetParameter(jpg_handle, sub, &ip, &op);
	if (ret != 0) {
		ifail(out, "jpgDecSetParameter: 0x%08x", (unsigned)ret);
		goto out;
	}

	if (op.width == 0 || op.height == 0 ||
	    op.width > IMG_MAX_W || op.height > IMG_MAX_H) {
		ifail(out, "la salida seria de %ux%u, no cabe",
		      (unsigned)op.width, (unsigned)op.height);
		goto out;
	}

	if (op.num_comp != 4) {
		ifail(out, "la salida tiene %u componentes, se esperaban 4",
		      (unsigned)op.num_comp);
		goto out;
	}

	/* width_bytes, con ese. En pngDecOutParam el mismo campo se llama
	 * width_byte. */
	if (op.width_bytes > (u64)op.width * 4ull) {
		ifail(out, "el decodificador pide %u bytes por linea y le damos %u",
		      (unsigned)op.width_bytes, (unsigned)(op.width * 4));
		goto out;
	}

	idlog("salida %ux%u (divisor %u), %u bytes por linea, %u de trabajo",
	     (unsigned)op.width, (unsigned)op.height, (unsigned)op.down_scale,
	     (unsigned)op.width_bytes, (unsigned)op.use_memory_space);

	memset(&ctl,   0, sizeof(ctl));
	memset(&dinfo, 0, sizeof(dinfo));

	ctl.output_bytes_per_line = (u64)op.width * 4ull;

	jpg_cmd = JPGDEC_CONTINUE;

	ret = jpgDecDecodeData(jpg_handle, sub, (u8*)pixels, &ctl, &dinfo);
	if (ret != 0) {
		ifail(out, "jpgDecDecodeData: 0x%08x", (unsigned)ret);
		goto out;
	}

	if (dinfo.decode_status != JPGDEC_STATUS_FINISH) {
		ifail(out, "el decodificador se paro a medias (estado %u, %u lineas)",
		      (unsigned)dinfo.decode_status, (unsigned)dinfo.output_lines);
		goto out;
	}

	out->px = pixels;
	out->w  = op.width;
	out->h  = op.height;
	out->ms = (u32)((now_us() - t0) / 1000);

	img_verbose++;
	ilog("decodificado %ux%u en %u ms (%u bytes de JPEG)",
	     (unsigned)out->w, (unsigned)out->h, (unsigned)out->ms,
	     (unsigned)len);

	jpgDecClose(jpg_handle, sub);
	return 0;

out:
	if (opened) jpgDecClose(jpg_handle, sub);
	return -1;
}

/* Mira los primeros bytes y llama al que toca.
 *
 * Las caratulas de la tienda de Microsoft vienen en PNG o en JPEG segun el
 * juego, y la URL no siempre lo dice. Fiarse de la extension es como acaba
 * uno pasandole un JPEG al decodificador de PNG y leyendo un 0x80611201
 * sin entender por que. Los dos formatos se identifican por sus primeros
 * bytes y eso no miente. */
int imgLooksLikeImage(const void *data, u32 len)
{
	const unsigned char *b = (const unsigned char*)data;

	if (!data || len < 8) return 0;

	if (memcmp(b, "\x89PNG\r\n\x1a\n", 8) == 0) return 1;
	if (b[0] == 0xff && b[1] == 0xd8 && b[2] == 0xff) return 1;

	return 0;
}

int imgDecode(const void *data, u32 len, imgImage *out)
{
	const unsigned char *b = (const unsigned char*)data;

	if (!out) return -1;

	if (!data || len < 8) {
		memset(out, 0, sizeof(*out));
		snprintf(out->err, sizeof(out->err),
		         "imagen vacia o demasiado corta (%u bytes)", (unsigned)len);
		return -1;
	}

	if (memcmp(b, "\x89PNG\r\n\x1a\n", 8) == 0)
		return imgDecodePNG(data, len, out);

	if (b[0] == 0xff && b[1] == 0xd8 && b[2] == 0xff)
		return imgDecodeJPG(data, len, out);

	memset(out, 0, sizeof(*out));
	out->src_bytes = len;
	snprintf(out->err, sizeof(out->err),
	         "formato desconocido: empieza por %02x %02x %02x %02x",
	         b[0], b[1], b[2], b[3]);
	ilog("%s", out->err);
	return -1;
}

/* --------------------------------------------------------------------- */

void imgShrink(u32 *dst, u32 dw, u32 dh, const u32 *src, u32 sw, u32 sh)
{
	u32 dy, dx;

	if (!dst || !src || !dw || !dh || !sw || !sh) return;

	for (dy = 0; dy < dh; dy++) {
		u32 y0 = (dy * sh) / dh;
		u32 y1 = ((dy + 1) * sh) / dh;
		if (y1 <= y0) y1 = y0 + 1;
		if (y1 > sh)  y1 = sh;

		for (dx = 0; dx < dw; dx++) {
			u32 x0 = (dx * sw) / dw;
			u32 x1 = ((dx + 1) * sw) / dw;
			u32 a = 0, r = 0, g = 0, b = 0, n = 0, y, x;

			if (x1 <= x0) x1 = x0 + 1;
			if (x1 > sw)  x1 = sw;

			for (y = y0; y < y1; y++) {
				const u32 *row = src + (size_t)y * sw;
				for (x = x0; x < x1; x++) {
					u32 p = row[x];
					a += (p >> 24) & 0xff;
					r += (p >> 16) & 0xff;
					g += (p >>  8) & 0xff;
					b +=  p        & 0xff;
					n++;
				}
			}

			if (!n) n = 1;

			dst[(size_t)dy * dw + dx] =
				((a / n) << 24) | ((r / n) << 16) |
				((g / n) <<  8) |  (b / n);
		}
	}
}

/* Decodificar y copiar, todo dentro del candado.
 *
 * Es lo que quieren los cuatro sitios que decodifican en este proyecto:
 * nadie usa los pixeles a tamano nativo mas alla de copiarlos a su sitio.
 * Teniendolo aqui, el buffer compartido no sale nunca de esta funcion y no
 * hay contrato que recordar.
 *
 * dst == NULL decodifica y no copia: sirve para medir sin querer los
 * pixeles. info se rellena igual, PERO info->px no vale al volver: el
 * buffer es de quien tenga el candado, y al volver ya no lo tenemos. */
int imgDecodeTo(const void *data, u32 len, u32 *dst, u32 dw, u32 dh,
                imgImage *info)
{
	imgImage tmp;
	int r;

	if (!info) info = &tmp;

	ILOCK();

	r = imgDecode(data, len, info);

	if (r == 0 && dst && dw && dh)
		imgShrink(dst, dw, dh, info->px, info->w, info->h);

	IUNLOCK();

	/* Que nadie se quede con un puntero al buffer compartido. */
	info->px = NULL;

	return r;
}
