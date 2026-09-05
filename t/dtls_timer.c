/* GR33N - prueba del temporizador de DTLS (source/dtls_timer.c).
 *
 *   cc -o dtls_timer dtls_timer.c ../source/dtls_timer.c -I stub \
 *      -fsanitize=address,undefined && ./dtls_timer
 *
 * QUE SE PRUEBA Y POR QUE.
 *
 * Estas cuatro funciones son las que hacen que mbedTLS RETRANSMITA el
 * saludo de DTLS. Si devuelven mal, no hay error ni aviso: hay una
 * negociacion que se queda colgada para siempre, o -peor- una que
 * retransmite antes de tiempo y satura el enlace. Los valores -1/0/1/2 de
 * mbedtls_timing_get_delay son CONTRATO de mbedTLS, no una convencion
 * nuestra, asi que hay que comprobarlos uno a uno.
 *
 * El reloj se controla desde aqui: mbedtls_ms_time() la pone esta prueba,
 * no la consola. Asi se puede saltar hacia adelante sin esperar y, sobre
 * todo, probar lo que en hardware no se puede provocar a mano -- un reloj
 * que salta hacia atras, o el paso por 2^32.
 *
 * Se compila el FICHERO DE VERDAD, no una copia. Ese fue el motivo de
 * sacarlo de net_tls.c: alli arrastraba PSL1GHT entero.
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>

#include "mbedtls/platform_time.h"
#include "mbedtls/timing.h"

/* --------------------------------------------------------------------- */
/* El reloj falso                                                        */
/* --------------------------------------------------------------------- */

static int64_t reloj_ms = 1000;

mbedtls_ms_time_t mbedtls_ms_time(void)
{
	return (mbedtls_ms_time_t)reloj_ms;
}

static void avanzar(int64_t ms) { reloj_ms += ms; }

/* --------------------------------------------------------------------- */

static int fallos;

static void ok(int cond, const char *fmt, ...)
{
	va_list ap;

	printf(cond ? "   bien  " : "   MAL   ");
	if (!cond)
		fallos++;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
}

int main(void)
{
	mbedtls_timing_delay_context ctx;

	printf("== el contrato de get_delay ==\n");

	/* fin_ms == 0 es CANCELADO, y tiene que dar -1 aunque el reloj
	 * lleve corriendo un rato. */
	mbedtls_timing_set_delay(&ctx, 0, 0);
	ok(mbedtls_timing_get_delay(&ctx) == -1, "sin plazo -> -1 (cancelado)");
	avanzar(100000);
	ok(mbedtls_timing_get_delay(&ctx) == -1, "y sigue siendo -1 pase el tiempo que pase");

	/* El caso normal: 1 s intermedio, 4 s final. Son los numeros que
	 * pone libpeer con mbedtls_ssl_conf_handshake_timeout(1000, 4000). */
	mbedtls_timing_set_delay(&ctx, 1000, 4000);
	ok(mbedtls_timing_get_delay(&ctx) == 0, "recien puesto -> 0 (nada ha vencido)");

	avanzar(999);
	ok(mbedtls_timing_get_delay(&ctx) == 0, "a 999 ms todavia 0");

	avanzar(1);
	ok(mbedtls_timing_get_delay(&ctx) == 1, "a 1000 ms clavados -> 1 (toca retransmitir)");

	avanzar(2999);
	ok(mbedtls_timing_get_delay(&ctx) == 1, "a 3999 ms sigue 1");

	avanzar(1);
	ok(mbedtls_timing_get_delay(&ctx) == 2, "a 4000 ms clavados -> 2 (el saludo ha fracasado)");

	avanzar(1000000);
	ok(mbedtls_timing_get_delay(&ctx) == 2, "y de ahi no baja");

	printf("\n== el orden de las comparaciones ==\n");

	/* Con int_ms == fin_ms hay que devolver 2, no 1. Preguntar por el
	 * intermedio primero daria 1 y mbedTLS retransmitiria para siempre
	 * en vez de rendirse. Es el fallo silencioso que justifica esta
	 * prueba entera. */
	mbedtls_timing_set_delay(&ctx, 2000, 2000);
	avanzar(2000);
	ok(mbedtls_timing_get_delay(&ctx) == 2,
	   "con intermedio == final, manda el final (2, no 1)");

	/* Y con int_ms == 0: mbedTLS lo usa para "avisame en cuanto puedas". */
	mbedtls_timing_set_delay(&ctx, 0, 5000);
	ok(mbedtls_timing_get_delay(&ctx) == 1, "intermedio 0 -> 1 desde el primer instante");

	printf("\n== cancelar no toca el reloj ==\n");

	/* mbedTLS cancela al terminar cada vuelo y vuelve a poner plazo en
	 * el siguiente. Si set_delay(0,0) reiniciara el contador, el plazo
	 * siguiente se mediria desde el sitio equivocado. */
	mbedtls_timing_set_delay(&ctx, 1000, 4000);
	avanzar(500);
	mbedtls_timing_set_delay(&ctx, 0, 0);      /* cancelar */
	avanzar(500);
	mbedtls_timing_set_delay(&ctx, 1000, 4000); /* y volver a poner */
	ok(mbedtls_timing_get_delay(&ctx) == 0,
	   "tras cancelar y volver a poner, se cuenta desde cero");

	printf("\n== get_timer ==\n");
	{
		struct mbedtls_timing_hr_time t;

		ok(mbedtls_timing_get_timer(&t, 1) == 0, "con reset devuelve 0");
		avanzar(1234);
		ok(mbedtls_timing_get_timer(&t, 0) == 1234, "y luego los ms transcurridos");
		ok(mbedtls_timing_get_timer(&t, 0) == 1234, "leerlo dos veces no lo mueve");
		ok(mbedtls_timing_get_timer(&t, 1) == 0, "y con reset vuelve a cero");
		ok(mbedtls_timing_get_timer(&t, 0) == 0, "recien reiniciado, 0 transcurridos");
	}

	printf("\n== get_final_delay ==\n");
	mbedtls_timing_set_delay(&ctx, 1000, 4000);
	ok(mbedtls_timing_get_final_delay(&ctx) == 4000, "devuelve el plazo final");
	mbedtls_timing_set_delay(&ctx, 0, 0);
	ok(mbedtls_timing_get_final_delay(&ctx) == 0, "y 0 cuando esta cancelado");

	printf("\n== el reloj saltando hacia atras ==\n");

	/* No deberia pasar -- el timebase de la PPE solo sube-- pero la
	 * resta se hace en uint64 y hay que saber que pasa si pasa. Con
	 * resta sin signo, un reloj atrasado da un numero enorme, que
	 * vence el plazo y hace que mbedTLS se rinda. Eso es lo correcto
	 * aqui: mejor un saludo fallido que uno colgado para siempre. */
	mbedtls_timing_set_delay(&ctx, 1000, 4000);
	avanzar(-500);
	ok(mbedtls_timing_get_delay(&ctx) == 2,
	   "un reloj que retrocede vence el plazo (se rinde, no se cuelga)");

	printf("\n== plazos largos ==\n");

	/* unsigned long son 32 bits en el ABI de PSL1GHT. 49 dias de
	 * milisegundos caben; los plazos de DTLS son de segundos. Se
	 * comprueba que un intervalo grande pero razonable sale entero. */
	reloj_ms = 1000;
	mbedtls_timing_set_delay(&ctx, 30000, 60000);
	avanzar(59999);
	ok(mbedtls_timing_get_delay(&ctx) == 1, "a 59999 de 60000 todavia 1");
	avanzar(1);
	ok(mbedtls_timing_get_delay(&ctx) == 2, "y a 60000 clavados, 2");

	printf("\n%s (%d fallos)\n", fallos ? "HAY FALLOS" : "todo bien", fallos);
	return fallos != 0;
}
