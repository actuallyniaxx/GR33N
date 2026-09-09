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
 * AQUI HABIA UN RAZONAMIENTO MALO, Y COSTO 100 ms DE RETARDO.
 *
 * Lo que ponia era: "dar a los dos caminos el MISMO retardo; el video
 * espera VJ_ESPERA_MS, el audio espera lo mismo menos lo que se traga el
 * puerto". De ahi salio AUD_ESPERA_MS 160, para emparejar con los 200 de
 * vjitter.
 *
 * Es falso, y basta con leer vjitter.h para verlo: VJ_ESPERA_MS es el
 * plazo que se le da a un fotograma AL QUE LE FALTA ALGO para que lleguen
 * sus retransmisiones. Su propio comentario lo dice con todas las letras
 * -- "el retraso solo se paga cuando el fotograma YA esta incompleto; los
 * sanos salen en el acto". En una conexion que va bien, el video no
 * retrasa nada.
 *
 * O sea que el audio llevaba 160 ms de relleno para emparejarse con un
 * retardo que la mayor parte del tiempo no existe. Los ~200 ms que se
 * oian (160 mas los 42,6 del puerto) eran casi todos gratis.
 *
 * Lo que el colchon tiene que absorber de verdad es el JITTER DE LA RED
 * en el flujo de audio: tramas de Opus de 20 ms que llegan a destiempo.
 * Eso se mide en decenas de milisegundos, no en cientos.
 *
 * La leccion, que es la de siempre en este proyecto: un numero copiado de
 * otro numero hereda lo que ese otro numero signifique DE VERDAD, no lo
 * que uno creia que significaba. Comprobarlo cuesta leer una cabecera.
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

/* Cuanto se espera antes de empezar a reproducir.
 *
 * 60 ms de colchon mas los 42,6 que el puerto lleva siempre por delante
 * (ocho bloques de 256 muestras a 48 kHz) son unos 100 ms hasta el
 * altavoz. Antes eran 160 + 42,6, y de ahi la issue #2.
 *
 * 60 son tres tramas de Opus de 20 ms. Absorbe un reordenado y una
 * llegada tarde sin cortar; por debajo de eso el margen es una sola
 * trama y cualquier hipo de la red se oye.
 *
 * SI SE QUEDA CORTO, NO HAY QUE ADIVINARLO: info.vacios cuenta los
 * bloques que salen en silencio por no tener muestras, y sale en el panel
 * de depuracion. Si ese numero sube mientras se juega, este es el valor
 * que hay que subir. Si se queda en cero, se puede bajar mas. */
#define AUD_ESPERA_MS 60

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
