/* GR33N - ajustes de mbedTLS para PS3 / PSL1GHT
 *
 * Esto NO es una configuracion desde cero: es un fichero de usuario que
 * se aplica DESPUES de la configuracion por defecto de mbedTLS
 * (MBEDTLS_USER_CONFIG_FILE). Escribir una configuracion completa a mano
 * es la forma clasica de acabar con un TLS que compila y no negocia nada.
 * Aqui solo se quita lo que la consola no tiene y se enchufa lo que hay
 * que suplir.
 *
 * Lo que la PS3 no tiene:
 *
 *   - /dev/urandom. No hay fuente de entropia de sistema al estilo POSIX,
 *     asi que la ponemos nosotros (mbedtls_hardware_poll en net_tls.c).
 *     Sin esto mbedTLS no arranca: se niega, con razon, a generar claves
 *     con un generador que no sabe de donde viene.
 *
 *   - Sockets BSD. PSL1GHT tiene netSocket/netConnect/netSend, que se
 *     parecen pero no son. En vez de pelearse con net_sockets.c se le dan
 *     a mbedTLS nuestras propias funciones de enviar y recibir, que es
 *     justo para lo que existe mbedtls_ssl_set_bio.
 *
 *   - time() fiable. La consola tiene reloj, pero se lee por syscall.
 *     MBEDTLS_PLATFORM_TIME_ALT deja que se lo demos nosotros. Importa:
 *     sin fecha no se puede comprobar si un certificado ha caducado, y un
 *     TLS que no comprueba caducidad es teatro.
 */

#ifndef GR33N_PS3_MBEDTLS_CONFIG_H
#define GR33N_PS3_MBEDTLS_CONFIG_H

/* --- lo que no existe en la consola ---------------------------------- */

#undef MBEDTLS_NET_C              /* sockets BSD: los ponemos nosotros   */
#undef MBEDTLS_FS_IO              /* los certificados van empotrados     */
#undef MBEDTLS_PSA_ITS_FILE_C     /* almacen PSA en ficheros             */
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C

/* --- entropia --------------------------------------------------------- */

#define MBEDTLS_NO_PLATFORM_ENTROPY   /* no busques /dev/urandom         */
#define MBEDTLS_ENTROPY_HARDWARE_ALT  /* la damos nosotros               */

/* --- fecha y hora ----------------------------------------------------- */

#define MBEDTLS_PLATFORM_TIME_ALT     /* la damos nosotros               */

/* Y el reloj monotono, que es otra cosa distinta.
 *
 * mbedTLS necesita DOS relojes y hay que tenerlo claro:
 *
 *   - "que hora es"     -> para saber si un certificado ha caducado.
 *                          Puede saltar hacia atras si alguien cambia la
 *                          hora de la consola. sysGetCurrentTime.
 *
 *   - "cuanto ha pasado" -> para vencimientos de sesion. Tiene que subir
 *                          siempre, pase lo que pase con el reloj.
 *
 * Sin esto, platform_util.c es el UNICO fichero de los 108 que no
 * compila: busca clock_gettime(CLOCK_MONOTONIC) o la API de Windows, no
 * encuentra ninguna, y suelta un #error. Bien hecho por su parte.
 *
 * Y resulta que la PS3 tiene el reloj monotono perfecto: el registro de
 * timebase de la PPE, que es el mismo que ya usamos para cronometrar el
 * frame. Un contador de hardware que solo sube.
 *
 * Lo implementa net_tls.c. */
#define MBEDTLS_PLATFORM_MS_TIME_ALT

/* --- recortes ---------------------------------------------------------
 *
 * No es por tamano: es por superficie. Cada cosa que se queda es una cosa
 * mas que puede tener un fallo y que hay que entender si algo va mal.
 * Solo hacemos de CLIENTE y solo hablamos con Microsoft.
 */

#undef MBEDTLS_SELF_TEST
#undef MBEDTLS_SSL_SRV_C              /* no somos servidor               */

/* DTLS se QUEDA, aunque hoy no se use: WebRTC lo necesita para DTLS-SRTP
 * y quitarlo ahora seria trabajo que habria que deshacer.
 *
 * Ademas, el primer intento de quitarlo fallo de forma instructiva: las
 * opciones de DTLS estan encadenadas entre si y desactivar tres dejaba
 * colgadas otras dos que dependian de ellas. mbedTLS lo detecta en
 * check_config.h y se NIEGA a compilar, que es exactamente lo que uno
 * quiere de una libreria de criptografia: mejor un error que un binario
 * que negocia menos de lo que crees.
 */

/* --- y lo que WebRTC necesita ademas de DTLS a secas ------------------ */

/* La extension use_srtp de DTLS (RFC 5764), que es de donde salen las
 * claves de SRTP.
 *
 * En WebRTC el video no se cifra con DTLS: DTLS solo hace el saludo y
 * ACUERDA el material de clave, y luego los paquetes RTP van por SRTP con
 * esas claves. Sin esta opcion no existen
 * mbedtls_ssl_conf_dtls_srtp_protection_profiles() ni
 * mbedtls_ssl_get_dtls_srtp_negotiation_result(), que es exactamente lo
 * que llama dtls_srtp.c de libpeer: la biblioteca no compilaria.
 *
 * Viene comentada en la configuracion por defecto de mbedTLS. Su unico
 * requisito, segun check_config.h:1068, es MBEDTLS_SSL_PROTO_DTLS, que
 * esta puesto por defecto y aqui no se toca. */
#define MBEDTLS_SSL_DTLS_SRTP

/* Y el temporizador, que es lo que RETRANSMITE el saludo.
 *
 * Aqui habia un `#undef MBEDTLS_TIMING_C` con el motivo correcto
 * -library/timing.c usa gettimeofday y select de POSIX, y el select de
 * PSL1GHT es netSelect y solo vale para sockets- pero SIN PONER NADA EN SU
 * LUGAR. Con TLS sobre TCP eso no se nota, porque quien retransmite es el
 * nucleo. DTLS va sobre UDP y ahi retransmite mbedTLS mirando este
 * temporizador: sin el, un solo datagrama perdido del saludo deja la
 * negociacion colgada para siempre, sin error y sin nada en el log.
 *
 * MBEDTLS_TIMING_ALT es el gancho oficial para esto: timing.h coge las
 * estructuras de deps/mbedtls-ps3/timing_alt.h y library/timing.c se
 * compila entero a nada. Las cuatro funciones estan en source/net_tls.c,
 * sobre el timebase de la PPE que ya usa mbedtls_ms_time().
 *
 * green-nx lo resolvio parcheando el #error de library/timing.c para colar
 * __SWITCH__ y dejar que se compilara la rama de Unix. Les vale porque su
 * newlib trae gettimeofday Y select; y aparte, no parchear una biblioteca
 * de criptografia es una cosa menos que rebasar cada vez que sube la
 * version. */
#define MBEDTLS_TIMING_ALT

#endif /* GR33N_PS3_MBEDTLS_CONFIG_H */
