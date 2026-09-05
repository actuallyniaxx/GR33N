/* Opus de mentira. Devuelve tramas reconocibles en vez de audio: cada
 * muestra lleva escrito de que paquete salio, asi la prueba puede afirmar
 * QUE se reprodujo y en QUE orden, que es lo unico que aud.c decide.
 * Descodificar Opus de verdad es cosa de Opus y ya esta probado. */
#ifndef FALSO_OPUS_H
#define FALSO_OPUS_H
#include <stdint.h>
typedef int32_t opus_int32;
typedef struct OpusDecoder OpusDecoder;
#define OPUS_OK 0
OpusDecoder *opus_decoder_create(opus_int32 fs, int ch, int *err);
void opus_decoder_destroy(OpusDecoder *d);
int opus_decode_float(OpusDecoder *d, const unsigned char *data,
                      opus_int32 len, float *pcm, int frame_size, int fec);
const char *opus_strerror(int e);
#endif
