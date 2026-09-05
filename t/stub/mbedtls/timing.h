/* Sustituto de <mbedtls/timing.h> para las pruebas del PC.
 *
 * Reproduce SOLO la rama que nos toca: con MBEDTLS_TIMING_ALT puesto, el
 * original hace `#include "timing_alt.h"` y declara las cuatro funciones.
 * Las firmas estan copiadas literales de mbedtls-3.6.4/include/mbedtls/
 * timing.h -- si alguna no cuadrara con la de verdad, el fallo saldria en
 * la consola y no aqui, que es justo lo que no se quiere. */
#ifndef STUB_MBEDTLS_TIMING_H
#define STUB_MBEDTLS_TIMING_H
#include <stdint.h>

#include "timing_alt.h"

unsigned long mbedtls_timing_get_timer(struct mbedtls_timing_hr_time *val, int reset);
void mbedtls_timing_set_delay(void *data, uint32_t int_ms, uint32_t fin_ms);
int mbedtls_timing_get_delay(void *data);
uint32_t mbedtls_timing_get_final_delay(const mbedtls_timing_delay_context *data);
#endif
