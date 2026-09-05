/* Sustituto MINIMO de <mbedtls/platform_time.h> para las pruebas del PC.
 * Copiado literal del original de mbedTLS 3.6.4: el typedef es int64_t
 * cuando MBEDTLS_PLATFORM_MS_TIME_ALT esta puesto, que es nuestro caso. */
#ifndef STUB_MBEDTLS_PLATFORM_TIME_H
#define STUB_MBEDTLS_PLATFORM_TIME_H
#include <stdint.h>
typedef int64_t mbedtls_ms_time_t;
mbedtls_ms_time_t mbedtls_ms_time(void);
#endif
