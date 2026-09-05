/* GR33N - el reloj de DTLS para la PS3.
 *
 * ESTE FICHERO NO ES UN PARCHE: ES EL GANCHO OFICIAL DE mbedTLS.
 *
 * Con MBEDTLS_TIMING_ALT definido, include/mbedtls/timing.h hace
 * `#include "timing_alt.h"` en lugar de declarar sus propias estructuras, y
 * library/timing.c se compila entero a nada. O sea que mbedTLS nos deja el
 * hueco a proposito y no hay que tocarle una linea. Las cuatro funciones
 * que hay que poner estan en source/net_tls.c.
 *
 * POR QUE HACE FALTA, QUE ES LO QUE IMPORTA
 *
 * DTLS va sobre UDP, o sea que los paquetes del saludo se pierden. Quien
 * los retransmite no es el sistema operativo: es mbedTLS, mirando un
 * temporizador que le da la aplicacion. Sin temporizador, un solo datagrama
 * perdido cuelga la negociacion para siempre, sin error y sin nada en el
 * log. Y no es un caso raro: el saludo DTLS de WebRTC son cinco o seis
 * vuelos por WiFi.
 *
 * ps3_mbedtls_config.h tenia `#undef MBEDTLS_TIMING_C` con el motivo
 * correcto -library/timing.c usa gettimeofday y select de POSIX, que aqui
 * no estan-, pero nadie puso nada en su lugar. Con TLS sobre TCP daba
 * igual, porque ahi retransmite el nucleo. Con DTLS no.
 *
 * POR QUE ASI Y NO COMO green-nx
 *
 * Ellos parchean el #error de library/timing.c para colar __SWITCH__ y
 * dejan que se compile la rama de Unix. Eso funciona en devkitA64 porque
 * su newlib trae gettimeofday Y select; PSL1GHT trae gettimeofday pero su
 * select es netSelect y solo vale para sockets. Aparte, con TIMING_ALT no
 * hay que parchear una biblioteca de criptografia, que es de las cosas que
 * uno prefiere no tener que rebasar cada vez que sube la version.
 *
 * DE DONDE SALE EL TIEMPO
 *
 * De mbedtls_ms_time(), que ya existe en net_tls.c desde que se monto TLS:
 * el registro de timebase de la PPE, un contador de hardware que solo
 * sube. Es el reloj bueno para esto -- lo que hace falta es "cuanto ha
 * pasado", no "que hora es", y el segundo puede saltar hacia atras si
 * alguien cambia la hora de la consola en mitad de un saludo.
 */

#ifndef GR33N_TIMING_ALT_H
#define GR33N_TIMING_ALT_H

#include <stdint.h>

/* El equivalente de la de mbedTLS, que es un uint64_t opaque[4] porque
 * tiene que valer para Windows y para POSIX. Aqui solo hace falta el
 * instante de arranque, y nadie mas que net_tls.c mira dentro. */
struct mbedtls_timing_hr_time {
	uint64_t inicio_ms;
};

/* Los mismos tres campos que la de mbedTLS, con los mismos nombres y en el
 * mismo orden. Los nombres importan: aunque los toquemos solo nosotros, la
 * documentacion de mbedtls_timing_set_delay habla de int_ms y fin_ms, y
 * quien lea esto luego va a buscar exactamente eso.
 *
 * MBEDTLS_PRIVATE no se usa: esa macro sirve para marcar campos internos
 * de mbedTLS cuando se compila con MBEDTLS_ALLOW_PRIVATE_ACCESS apagado, y
 * esta estructura ya es nuestra. */
typedef struct mbedtls_timing_delay_context {
	struct mbedtls_timing_hr_time timer;
	uint32_t                      int_ms;   /* aviso intermedio  */
	uint32_t                      fin_ms;   /* vencimiento final */
} mbedtls_timing_delay_context;

#endif /* GR33N_TIMING_ALT_H */
