/* GR33N - los logs de libpeer, al servidor de depuracion.
 *
 * libpeer se compila con LOG_REDIRECT=1, y con eso todas sus macros LOGE,
 * LOGW, LOGI y LOGD llaman aqui en vez de escribir por stdout
 * (src/utils.h:24). Es el UNICO simbolo que libpeer pide y que no pone
 * ninguna de las cuatro bibliotecas.
 *
 * NO ES UN DETALLE DE COMODIDAD. Arrancando desde el XMB no hay TTY: lo que
 * no llega al PC no existe. Toda la negociacion de WebRTC -- candidatos ICE,
 * el saludo DTLS, los canales de datos, los motivos de fallo-- pasa por
 * estas macros, y sin este fichero se perderia entera. Es la misma leccion
 * que costo una tarde con videoInit(): el unico fallo que de verdad hacia
 * falta ver ocurria antes de que existiera el instrumento para verlo.
 *
 * LA FIRMA NO ES NEGOCIABLE. La fija libpeer:
 *
 *     void peer_log(char* level_tag, const char* file_name,
 *                   int line_number, const char* fmt, ...);
 *
 * El primer parametro es `char*` y no `const char*`, que es raro pero es lo
 * que hay: cambiarlo aqui seria un prototipo que no cuadra y el enlazador
 * no se queja de eso en C.
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "link.h"

/* Cabe en una linea del log remoto (PEND_LEN son 200) contando lo que se le
 * pone delante. Se recorta y no se parte en dos: una linea de libpeer
 * cortada es mas facil de leer que dos mitades intercaladas con las de otro
 * hilo, y aqui escriben el hilo de sesion y el de la conexion a la vez. */
#define PEER_LOG_MAX 160

void peer_log(char *level_tag, const char *file_name, int line_number,
              const char *fmt, ...)
{
	char texto[PEER_LOG_MAX];
	const char *base;
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(texto, sizeof(texto), fmt, ap);
	va_end(ap);

	/* Solo el nombre del fichero, no la ruta.
	 *
	 * __FILE__ aqui es la ruta con la que se compilo libpeer, que es algo
	 * como /home/nia/.gr33n-deps/libpeer-src/src/agent.c: cincuenta bytes
	 * de una linea de doscientos, iguales en todas, que ademas cambian de
	 * una maquina a otra. Lo unico que aporta informacion es "agent.c".
	 *
	 * Se busca la ultima barra de las dos clases: el toolchain es de Unix
	 * pero el arbol vive en C:\ps3dev y algun dia puede aparecer una
	 * ruta con contrabarras. */
	base = file_name;
	if (base != NULL) {
		const char *p;

		for (p = file_name; *p; p++)
			if (*p == '/' || *p == '\\')
				base = p + 1;
	} else {
		base = "?";
	}

	/* El formato sigue el del resto de GR33N: un prefijo entre corchetes
	 * que dice de quien es la linea, para poder filtrar el log por
	 * modulo. */
	linkLog("[peer/%s] %s:%d %s",
	        level_tag ? level_tag : "?", base, line_number, texto);
}
