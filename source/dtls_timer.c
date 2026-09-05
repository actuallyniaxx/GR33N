/* GR33N - el temporizador que retransmite el saludo de DTLS.
 *
 * ESTE FICHERO EXISTE SEPARADO PARA PODER EJECUTARLO EN EL PC.
 *
 * Las cuatro funciones son aritmetica pura sobre un reloj: no tocan la
 * consola, no tocan la red, y su contrato -los valores -1/0/1/2 de
 * mbedtls_timing_get_delay- lo fija mbedTLS y no se puede improvisar. Eso
 * las pone en la misma categoria que dechunk, textSlice o la geometria de
 * la rejilla: se compilan y se CORREN en el PC, con t/dtls_timer.c.
 *
 * Estaban dentro de net_tls.c, que es donde vive el resto de la capa de
 * plataforma de mbedTLS. Se sacaron aqui justo para eso: net_tls.c arrastra
 * PSL1GHT entero y no hay forma de ejecutarlo fuera de la PS3.
 *
 * Lo unico que necesitan de fuera es mbedtls_ms_time(), que sigue en
 * net_tls.c sobre el timebase de la PPE.
 */

#include <stdint.h>

#include "mbedtls/platform_time.h"
#include "mbedtls/timing.h"

/* ESTO ES LO QUE RETRANSMITE EL SALUDO DE WebRTC.
 *
 * DTLS va sobre UDP, o sea que los paquetes del saludo se pierden. Quien
 * los vuelve a mandar no es el sistema: es mbedTLS, mirando un
 * temporizador que le da la aplicacion con mbedtls_ssl_set_timer_cb().
 * Sin el, un solo datagrama perdido deja la negociacion colgada para
 * siempre -- sin error, sin excepcion y sin una linea en el log. Y no es
 * un caso raro: el saludo DTLS de WebRTC son cinco o seis vuelos, por
 * WiFi, contra un servidor de Microsoft.
 *
 * Hasta ahora la configuracion hacia `#undef MBEDTLS_TIMING_C` con el
 * motivo correcto -library/timing.c usa gettimeofday y select de POSIX-
 * pero sin poner nada en su lugar. Con TLS sobre TCP daba igual: ahi
 * retransmite el nucleo. Con DTLS, no.
 *
 * Ahora la configuracion pone MBEDTLS_TIMING_C y MBEDTLS_TIMING_ALT, que
 * es el gancho oficial: timing.h coge las estructuras de nuestro
 * deps/mbedtls-ps3/timing_alt.h y library/timing.c se compila a nada. Cero
 * parches sobre una biblioteca de criptografia.
 *
 * Las cuatro funciones son las de mbedTLS punto por punto -- se leyo su
 * library/timing.c para copiarle el contrato, no para adivinarlo. Lo unico
 * que cambia es de donde sale el tiempo: mbedtls_ms_time(), o sea el
 * timebase de la PPE, que es el reloj bueno para esto. Lo que hace falta
 * es "cuanto ha pasado", no "que hora es", y el segundo puede saltar hacia
 * atras si alguien cambia la hora de la consola en mitad de un saludo.
 *
 * Quien las llama es libpeer, en dtls_srtp.c:374:
 *
 *     static mbedtls_timing_delay_context timer;
 *     mbedtls_ssl_set_timer_cb(&dtls_srtp->ssl, &timer,
 *                              mbedtls_timing_set_delay,
 *                              mbedtls_timing_get_delay);
 */

/* Milisegundos desde el ultimo reinicio. Con reset != 0 pone el cero
 * ahora y devuelve 0, que es lo que hace la de mbedTLS. */
unsigned long mbedtls_timing_get_timer(struct mbedtls_timing_hr_time *val,
                                       int reset)
{
	uint64_t ahora = (uint64_t)mbedtls_ms_time();

	if (reset) {
		val->inicio_ms = ahora;
		return 0;
	}

	/* Resta en u64 y recorte al final. unsigned long son 32 bits en
	 * este ABI, y ahi caben 49 dias de milisegundos: de sobra para un
	 * saludo DTLS, cuyo vencimiento mas largo son 4 segundos. Hacer la
	 * resta en 64 evita que un reloj recien puesto a cero de un numero
	 * enorme por debajo de cero. */
	return (unsigned long)(ahora - val->inicio_ms);
}

/* fin_ms == 0 quiere decir CANCELAR, y entonces no se toca el reloj. Es
 * importante respetarlo: mbedTLS cancela el temporizador al terminar cada
 * vuelo, y reiniciar el contador ahi haria que el siguiente vencimiento se
 * midiera desde el sitio equivocado. */
void mbedtls_timing_set_delay(void *data, uint32_t int_ms, uint32_t fin_ms)
{
	mbedtls_timing_delay_context *ctx = (mbedtls_timing_delay_context *)data;

	ctx->int_ms = int_ms;
	ctx->fin_ms = fin_ms;

	if (fin_ms != 0)
		(void)mbedtls_timing_get_timer(&ctx->timer, 1);
}

/* Los cuatro valores son contrato de mbedTLS y no se pueden reordenar:
 *
 *   -1  cancelado (fin_ms == 0)
 *    0  no ha vencido nada todavia
 *    1  ha vencido el intermedio -> toca retransmitir el vuelo
 *    2  ha vencido el final      -> el saludo ha fracasado
 *
 * El orden de las comparaciones tambien importa: fin_ms >= int_ms siempre,
 * asi que hay que preguntar por el final PRIMERO o nunca se devolveria 2. */
int mbedtls_timing_get_delay(void *data)
{
	mbedtls_timing_delay_context *ctx = (mbedtls_timing_delay_context *)data;
	unsigned long pasado;

	if (ctx->fin_ms == 0)
		return -1;

	pasado = mbedtls_timing_get_timer(&ctx->timer, 0);

	if (pasado >= ctx->fin_ms)
		return 2;

	if (pasado >= ctx->int_ms)
		return 1;

	return 0;
}

uint32_t mbedtls_timing_get_final_delay(const mbedtls_timing_delay_context *data)
{
	return data->fin_ms;
}

