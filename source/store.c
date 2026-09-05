/* GR33N - almacen persistente.
 *
 * Se usa stdio (fopen/fread/fwrite) y no la API lv2 de ficheros a
 * proposito: PSL1GHT tiene la capa de newlib enganchada al sistema de
 * ficheros de la consola, la semantica de C es la misma en todas partes,
 * y asi este fichero se puede probar en un PC sin inventarse nada.
 */

#include <stdio.h>
#include <string.h>

#include <ppu-types.h>

#include "link.h"
#include "store.h"

#define TOKEN_FILE   "token.dat"
#define PATH_MAX_LEN 160

/* Red de seguridad, por orden de preferencia. Lo PRIMERO que se prueba no
 * esta en esta lista: es la carpeta de la que nos han arrancado, sacada de
 * argv[0]. Adivinar el identificador de la aplicacion es tonteria cuando
 * el sistema nos dice donde estamos. */
static const char *const candidates[] = {
	"/dev_hdd0/game/GREEN0PS3/USRDIR/",
	"/dev_hdd0/game/GR33N001/USRDIR/",
	"/dev_usb000/GR33N/",
	"/dev_hdd0/",
	"/dev_hdd0/tmp/"
};
#define CANDIDATES_N ((int)(sizeof(candidates)/sizeof(candidates[0])))

static char base[PATH_MAX_LEN] = "";
static int  ready = 0;
static int  warned = 0;

/* "Todavia no se" y "no hay" NO son la misma respuesta.
 *
 * Todas las funciones de aqui devolvian el fallo corriente cuando no se
 * habia llamado a storeInit. Es defensivo y parece prudente, y costo una
 * sesion entera: authInit preguntaba si habia token antes de que el almacen
 * supiera donde mira, le contestaban que no, y la cadena del perfil no
 * arrancaba nunca. Ni un error, ni una linea en el log. La respuesta era
 * plausible, que es lo peor que puede ser una respuesta equivocada.
 *
 * Sigue devolviendo el mismo fallo - no hay nada mejor que devolver - pero
 * ahora deja constancia. Una vez, no en cada fotograma. */
static int not_ready(const char *what)
{
	if (!ready && !warned) {
		warned = 1;
		linkLog("!! [store] %s antes de storeInit. La respuesta correcta "
		        "seria 'no lo se', pero solo se puede decir 'no'", what);
	}
	return !ready;
}

static void join(char *out, u32 max, const char *dir, const char *name)
{
	snprintf(out, max, "%s%s", dir, name);
}

/* Escribe de verdad y borra. Comprobar que la carpeta existe no sirve: la
 * pregunta no es si esta, es si nos deja. */
static int try_dir(const char *dir)
{
	char probe[PATH_MAX_LEN];
	FILE *f;

	if (!dir || !dir[0]) return 0;

	join(probe, sizeof(probe), dir, "gr33n.probe");

	f = fopen(probe, "wb");
	if (!f) return 0;

	if (fwrite("ok", 1, 2, f) != 2) { fclose(f); return 0; }

	fclose(f);
	remove(probe);

	snprintf(base, sizeof(base), "%s", dir);
	ready = 1;
	return 1;
}

int storeInit(const char *self_path)
{
	int i;

	ready = 0;
	warned = 0;
	base[0] = '\0';

	linkLog("[store] arrancado desde %s",
	        (self_path && self_path[0]) ? self_path : "(sin ruta)");

	/* 1. La carpeta del propio ejecutable. Si el SELF esta en
	 *    /dev_hdd0/game/LOQUESEA/USRDIR/, ahi es donde tiene sentido
	 *    dejar sus cosas, se llame como se llame la carpeta. */
	if (self_path && self_path[0]) {
		const char *slash = strrchr(self_path, '/');

		if (slash) {
			u32 n = (u32)(slash - self_path) + 1;
			char dir[PATH_MAX_LEN];

			if (n < sizeof(dir)) {
				memcpy(dir, self_path, n);
				dir[n] = '\0';
				try_dir(dir);
			}
		}
	}

	/* 2. Y si eso no cuela, la lista de siempre. */
	for (i = 0; !ready && i < CANDIDATES_N; i++)
		try_dir(candidates[i]);

	if (ready) linkLog("[store] guardando en %s", base);
	else       linkLog("!! [store] ningun sitio donde escribir: los ajustes "
	                   "no sobreviviran al apagado");

	return ready ? 0 : -1;
}

const char *storePath(void) { return base; }

int storeSave(const char *name, const void *data, u32 len)
{
	char path[PATH_MAX_LEN];
	FILE *f;
	size_t n;

	if (not_ready("storeSave") || !name || !data) return -1;

	join(path, sizeof(path), base, name);

	f = fopen(path, "wb");
	if (!f) return -1;

	n = fwrite(data, 1, (size_t)len, f);
	fclose(f);

	if (n != (size_t)len) {
		/* A medio escribir es peor que nada: un fichero de ajustes
		 * truncado se lee sin error y da valores absurdos. */
		remove(path);
		return -1;
	}

	return 0;
}

int storeLoad(const char *name, void *data, u32 max, u32 *out_len)
{
	char path[PATH_MAX_LEN];
	FILE *f;
	size_t n;

	if (out_len) *out_len = 0;
	if (not_ready("storeLoad") || !name || !data || !max) return -1;

	join(path, sizeof(path), base, name);

	f = fopen(path, "rb");
	if (!f) return -1;

	n = fread(data, 1, (size_t)max, f);
	fclose(f);

	if (out_len) *out_len = (u32)n;
	return n > 0 ? 0 : -1;
}

FILE *storeOpen(const char *name, const char *mode)
{
	char path[PATH_MAX_LEN];

	if (not_ready("storeOpen") || !name || !mode) return NULL;

	join(path, sizeof(path), base, name);
	return fopen(path, mode);
}

int storeDelete(const char *name)
{
	char path[PATH_MAX_LEN];

	if (not_ready("storeDelete") || !name) return -1;

	join(path, sizeof(path), base, name);
	return remove(path) == 0 ? 0 : -1;
}

/* --------------------------------------------------------------------- */
/* Credenciales                                                          */
/* --------------------------------------------------------------------- */

int storeSaveToken(const void *data, u32 len)
{
	if (len > STORE_TOKEN_MAX) return -1;
	return storeSave(TOKEN_FILE, data, len);
}

u32 storeLoadToken(void *data, u32 max)
{
	u32 len = 0;

	if (storeLoad(TOKEN_FILE, data, max, &len) != 0) return 0;
	return len;
}

void storeClearToken(void)
{
	storeDelete(TOKEN_FILE);
}

int storeHasToken(void)
{
	char path[PATH_MAX_LEN];
	FILE *f;

	if (not_ready("storeHasToken")) return 0;

	join(path, sizeof(path), base, TOKEN_FILE);

	f = fopen(path, "rb");
	if (!f) return 0;
	fclose(f);

	return 1;
}
