/* GR33N - el audio de xCloud, de RTP a los altavoces
 *
 * ------------------------------------------------------------------
 * EL AUDIO YA ESTABA LLEGANDO
 * ------------------------------------------------------------------
 *
 * La oferta SDP que sale de la consola lleva desde el primer dia:
 *
 *     m=audio 9 UDP/TLS/RTP/SAVPF 111
 *     a=rtpmap:111 opus/48000/2
 *     a=fmtp:111 minptime=10;useinbandfec=1;stereo=1
 *
 * y el parche de libpeer hasta trae un arreglo especifico para que el SSRC
 * del audio no se pierda cuando xCloud omite el a=ssrc. O sea que xCloud
 * lleva semanas mandando Opus y nosotros tirandolo, por no tener con que
 * descodificarlo ni donde ponerlo.
 *
 * ------------------------------------------------------------------
 * LOS DOS RELOJES, QUE ES DE LO QUE VA ESTE FICHERO
 * ------------------------------------------------------------------
 *
 * Hay dos ritmos y no se hablan:
 *
 *   - xCloud manda un paquete Opus cada 20 ms, cuando puede, en el orden
 *     que la red decida.
 *   - El puerto de audio de la PS3 consume un bloque de 256 muestras cada
 *     5,33 ms, siempre, puntual, sin preguntar. Si no hay nada escrito
 *     reproduce lo que hubiera antes en el buffer, que suena a zumbido.
 *
 * Entre los dos va todo lo de aqui: un buffer que reordena por numero de
 * secuencia, un descodificador, y un anillo de muestras del que el puerto
 * tira sin enterarse de la red.
 *
 * ------------------------------------------------------------------
 * 48000 = 48000
 * ------------------------------------------------------------------
 *
 * Opus descodifica a 48 kHz. El puerto de la PS3 sale a 48 kHz. No hay
 * remuestreo, no hay filtro, no hay error acumulado y no hay una tercera
 * biblioteca. De lo poco que sale gratis en este puerto; conviene no
 * estropearlo metiendo un remuestreador "por si acaso".
 *
 * ------------------------------------------------------------------
 * POR QUE SE REORDENA ANTES DE DESCODIFICAR
 * ------------------------------------------------------------------
 *
 * Opus TIENE ESTADO. Cada trama se descodifica a partir de la anterior, y
 * ademas el descodificador puede esconder una perdida (PLC) si le dices
 * que ha habido un hueco. Meterle las tramas desordenadas no da error: da
 * un chasquido en cada una. Por eso el parche de libpeer entrega el
 * paquete RTP ENTERO, cabecera incluida -- hace falta el numero de
 * secuencia, y el rtp_decode_generic original lo tiraba.
 *
 * ------------------------------------------------------------------
 * Y EL LABIO
 * ------------------------------------------------------------------
 *
 * Sincronizar audio y video DE VERDAD necesita los informes de emisor de
 * RTCP, que dicen a que hora del reloj del servidor corresponde cada marca
 * de tiempo RTP. Eso no lo procesamos todavia.
 *
 * Lo que se hace mientras tanto es mas tonto y funciona bastante: dar a
 * los dos caminos el MISMO retardo. El video espera VJ_ESPERA_MS en su
 * buffer; el audio espera lo mismo menos lo que ya se traga el puerto.
 * No es sincronia, es que los dos llegan igual de tarde -- que a efectos
 * de mirar una pantalla se le parece mucho.
 *
 * Si se despegan, el sitio donde mirar es AUD_ESPERA_MS y VJ_ESPERA_MS,
 * que estan puestos para ser leidos juntos.
 */

#ifndef GR33N_AUD_H
#define GR33N_AUD_H

#include <ppu-types.h>

/* Lo que manda xCloud, y lo que saca la consola. El mismo numero. */
#define AUD_SR        48000
#define AUD_CANALES   2

/* Una trama de Opus de 20 ms a 48 kHz. xCloud usa 20 ms; el codigo no lo
 * da por hecho -- opus_decode dice cuantas muestras ha sacado-- pero los
 * buffers se dimensionan para el caso peor de Opus, que son 120 ms. */
#define AUD_FRAME_MAX (AUD_SR / 1000 * 120)   /* 5760 muestras por canal */

/* Cuanto se espera antes de empezar a reproducir. Se lee JUNTO a
 * VJ_ESPERA_MS de vjitter.h: si uno cambia y el otro no, el labio se va.
 *
 * 160 y no 200 porque el puerto de audio ya se traga lo suyo: ocho bloques
 * de 256 muestras a 48 kHz son 42,6 ms que estan siempre por delante. */
#define AUD_ESPERA_MS 160

#define AUD_PKT_MAX   1500
#define AUD_PKTS      64      /* 64 x 20 ms = 1,28 s de margen */

typedef struct {
	int  activo;          /* el puerto esta abierto y sonando */
	int  puerto;

	u32  rtp_recibidos;
	u32  rtp_tarde;       /* llegaron cuando ya habian pasado de largo */
	u32  rtp_repetidos;
	u32  tramas;          /* descodificadas de verdad                 */
	u32  ocultadas;       /* huecos tapados con PLC de Opus           */
	u32  vacios;          /* bloques que salieron en silencio: MAL    */

	u32  pcm_ms;          /* cuanto audio hay listo, en milisegundos  */
	u32  bloques;         /* bloques escritos al puerto               */

	char err[160];
} audInfo;

int  audInit(void);
void audShutdown(void);

/* Abre el puerto y arranca el hilo. Se llama al empezar la sesion. */
int  audStart(void);

/* Cierra el puerto y para el hilo. Idempotente. */
void audStop(void);

/* Un paquete RTP de audio, ENTERO y con su cabecera, tal cual lo entrega
 * libpeer. Lo llama el hilo del bombeo; solo copia y ordena, no
 * descodifica: el bombeo no puede pararse a hacer nada caro. */
void audRtp(const u8 *pkt, u32 n);

const audInfo *audStatus(void);

/* Volumen, 0..100. Se aplica al escribir en el puerto. */
void audSetVolumen(int v);
int  audVolumen(void);

#endif /* GR33N_AUD_H */
