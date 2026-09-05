/* Prueba de sobremesa de source/vjitter.c.
 *
 * Todo lo que hace ese fichero es aritmetica sobre bytes, asi que el PC
 * contesta lo mismo que la consola -- y aqui se puede meter perdida,
 * reordenacion y duplicados a mano, que en un servidor de Azure no. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "vjitter.h"

static int fallos = 0;
static void ok(const char *q, int c){ printf("   %-56s %s\n", q, c?"ok":"FALLA"); if(!c) fallos++; }

/* --- lo que sale --- */
static u8  ult_au[VJ_AU_MAX];
static size_t ult_n = 0;
static int n_emitidos = 0;
static void emitir(const u8 *au, size_t n, void *ud){ (void)ud; memcpy(ult_au, au, n); ult_n = n; n_emitidos++; }

static int n_nacks = 0;
static u16 ult_pid = 0;
static void nack(u16 pid, u16 blp, void *ud){ (void)blp;(void)ud; n_nacks++; ult_pid = pid; }

/* --- construir un paquete RTP --- */
static size_t rtp(u8 *out, u16 seq, u32 ts, int marca,
                  const u8 *carga, size_t n, int relleno)
{
	size_t i = 0;
	out[0] = 0x80 | (relleno ? 0x20 : 0);
	out[1] = (u8)(96 | (marca ? 0x80 : 0));
	out[2] = (u8)(seq >> 8); out[3] = (u8)seq;
	out[4] = (u8)(ts >> 24); out[5] = (u8)(ts >> 16);
	out[6] = (u8)(ts >> 8);  out[7] = (u8)ts;
	memset(out + 8, 0, 4);       /* SSRC */
	i = 12;
	memcpy(out + i, carga, n); i += n;
	if (relleno) { out[i] = 0; out[i+1] = 0; out[i+2] = 3; i += 3; }
	return i;
}

static void limpia(void){ n_emitidos = 0; ult_n = 0; n_nacks = 0; }

int main(void)
{
	static vjBuf v;
	u8 pkt[2048], carga[1400];
	size_t n;
	int clave = 0;

	printf("1. un NALU IDR suelto en un paquete\n");
	vjReset(&v); limpia();
	carga[0] = 0x65;                 /* F=0 NRI=3 tipo=5 (IDR) */
	memset(carga + 1, 0xAB, 40);
	n = rtp(pkt, 100, 9000, 1, carga, 41, 0);
	vjRecibe(&v, pkt, n, 0, emitir, nack, NULL, &clave);
	ok("sale una unidad de acceso", n_emitidos == 1);
	ok("empieza por 00 00 00 01", ult_n > 4 && !memcmp(ult_au, "\0\0\0\1", 4));
	ok("y detras el NALU entero", ult_n == 45 && ult_au[4] == 0x65);
	ok("ya no espera clave", !vjEsperandoClave(&v));

	printf("\n2. un fotograma P antes del IDR se TIRA\n");
	vjReset(&v); limpia();
	carga[0] = 0x41;                 /* tipo 1: no IDR */
	n = rtp(pkt, 10, 100, 1, carga, 20, 0);
	clave = 0;
	vjRecibe(&v, pkt, n, 0, emitir, nack, NULL, &clave);
	ok("no sale nada", n_emitidos == 0);
	ok("pide fotograma clave", clave == 1);
	ok("sigue esperando", vjEsperandoClave(&v));
	ok("cuenta un resync", vjEstado(&v)->resyncs == 1);

	printf("\n3. FU-A partido en tres, en orden\n");
	vjReset(&v); limpia();
	{
		u8 f[3][30];
		int i;
		/* indicador 0x7c = F0 NRI3 tipo28; cabecera 0x85/0x05/0x45 */
		f[0][0]=0x7c; f[0][1]=0x85; memset(f[0]+2,0x11,10);  /* S, tipo 5 */
		f[1][0]=0x7c; f[1][1]=0x05; memset(f[1]+2,0x22,10);
		f[2][0]=0x7c; f[2][1]=0x45; memset(f[2]+2,0x33,10);  /* E */
		for (i = 0; i < 3; i++) {
			n = rtp(pkt, (u16)(200+i), 5000, i==2, f[i], 12, 0);
			vjRecibe(&v, pkt, n, 0, emitir, nack, NULL, &clave);
		}
		ok("sale una sola unidad", n_emitidos == 1);
		ok("arranque + cabecera reconstruida 0x65",
		   ult_n == 4 + 1 + 30 && ult_au[4] == 0x65);
		ok("los tres trozos, sin las cabeceras FU",
		   ult_au[5] == 0x11 && ult_au[15] == 0x22 && ult_au[25] == 0x33);
	}

	printf("\n4. el mismo FU-A, pero DESORDENADO\n");
	vjReset(&v); limpia();
	{
		u8 f[3][30];
		int orden[3] = {2, 0, 1}, i;
		f[0][0]=0x7c; f[0][1]=0x85; memset(f[0]+2,0x11,10);
		f[1][0]=0x7c; f[1][1]=0x05; memset(f[1]+2,0x22,10);
		f[2][0]=0x7c; f[2][1]=0x45; memset(f[2]+2,0x33,10);
		for (i = 0; i < 3; i++) {
			int k = orden[i];
			n = rtp(pkt, (u16)(300+k), 6000, k==2, f[k], 12, 0);
			vjRecibe(&v, pkt, n, 0, emitir, nack, NULL, &clave);
		}
		ok("sale igual", n_emitidos == 1);
		ok("y en el orden correcto",
		   ult_au[5] == 0x11 && ult_au[15] == 0x22 && ult_au[25] == 0x33);
	}

	printf("\n5. STAP-A: dos NALU en un paquete\n");
	vjReset(&v); limpia();
	{
		u8 s[64]; size_t i = 0;
		s[i++] = 0x78;               /* tipo 24 */
		s[i++] = 0; s[i++] = 5;  s[i++] = 0x65; memset(s+i,0xC1,4); i += 4;
		s[i++] = 0; s[i++] = 3;  s[i++] = 0x68; memset(s+i,0xC2,2); i += 2;
		n = rtp(pkt, 400, 7000, 1, s, i, 0);
		vjRecibe(&v, pkt, n, 0, emitir, nack, NULL, &clave);
		ok("sale una unidad", n_emitidos == 1);
		ok("con DOS codigos de arranque", ult_n == 4+5+4+3);
		ok("el primero es el IDR", ult_au[4] == 0x65);
		ok("el segundo va detras", ult_au[4+5+4] == 0x68);
	}

	printf("\n6. relleno de sondeo: se quita\n");
	vjReset(&v); limpia();
	carga[0] = 0x65; memset(carga+1, 0x77, 10);
	n = rtp(pkt, 500, 8000, 1, carga, 11, 1);   /* +3 de relleno */
	vjRecibe(&v, pkt, n, 0, emitir, nack, NULL, &clave);
	ok("la unidad NO lleva el relleno", n_emitidos == 1 && ult_n == 4 + 11);

	printf("\n7. un hueco: no se suelta, se pide, y vence\n");
	vjReset(&v); limpia();
	{
		u8 f[3][30];
		f[0][0]=0x7c; f[0][1]=0x85; memset(f[0]+2,0x11,10);
		f[2][0]=0x7c; f[2][1]=0x45; memset(f[2]+2,0x33,10);
		n = rtp(pkt, 600, 9000, 0, f[0], 12, 0);
		vjRecibe(&v, pkt, n, 1000, emitir, nack, NULL, &clave);
		n = rtp(pkt, 602, 9000, 1, f[2], 12, 0);   /* falta el 601 */
		clave = 0; n_nacks = 0;
		vjRecibe(&v, pkt, n, 1000, emitir, nack, NULL, &clave);
		ok("no sale nada todavia", n_emitidos == 0);
		ok("se ha pedido la retransmision", n_nacks == 1 && ult_pid == 601);

		/* Y ahora que pase el plazo, con un paquete de otro fotograma. */
		carga[0] = 0x41; 
		n = rtp(pkt, 700, 90000, 1, carga, 20, 0);
		clave = 0;
		vjRecibe(&v, pkt, n, 1000 + VJ_ESPERA_MS + 1, emitir, nack, NULL, &clave);
		ok("el fotograma roto se tira", vjEstado(&v)->tirados == 1);
		ok("y se pide clave", clave == 1);
		ok("no ha llegado NADA al decodificador", n_emitidos == 0);
	}

	printf("\n8. duplicados y retransmisiones tardias\n");
	vjReset(&v); limpia();
	carga[0] = 0x65; memset(carga+1, 0x99, 20);
	n = rtp(pkt, 800, 11000, 0, carga, 21, 0);
	vjRecibe(&v, pkt, n, 0, emitir, nack, NULL, &clave);
	vjRecibe(&v, pkt, n, 0, emitir, nack, NULL, &clave);   /* el mismo */
	ok("el duplicado no rompe nada", n_emitidos == 0);
	ok("dos paquetes contados", vjEstado(&v)->paquetes == 2);

	printf("\n9. la vuelta del contador de secuencia (65535 -> 0)\n");
	vjReset(&v); limpia();
	{
		u8 f[2][30];
		f[0][0]=0x7c; f[0][1]=0x85; memset(f[0]+2,0x44,10);
		f[1][0]=0x7c; f[1][1]=0x45; memset(f[1]+2,0x55,10);
		n = rtp(pkt, 65535, 12000, 0, f[0], 12, 0);
		vjRecibe(&v, pkt, n, 0, emitir, nack, NULL, &clave);
		n = rtp(pkt, 0, 12000, 1, f[1], 12, 0);
		vjRecibe(&v, pkt, n, 0, emitir, nack, NULL, &clave);
		ok("se monta cruzando el 65535", n_emitidos == 1);
		ok("con los dos trozos", ult_au[5] == 0x44 && ult_au[15] == 0x55);
	}

	printf("\n10. orden de salida por marca de tiempo\n");
	vjReset(&v); limpia();
	{
		/* Dos fotogramas completos, el segundo llega antes que el primero. */
		carga[0] = 0x65; memset(carga+1, 0x01, 10);
		n = rtp(pkt, 900, 20000, 1, carga, 11, 0);
		vjRecibe(&v, pkt, n, 0, emitir, nack, NULL, &clave);   /* IDR, ts 20000 */
		ok("el IDR sale", n_emitidos == 1);

		carga[0] = 0x41; memset(carga+1, 0x03, 10);
		n = rtp(pkt, 902, 22000, 1, carga, 11, 0);             /* ts 22000 */
		vjRecibe(&v, pkt, n, 0, emitir, nack, NULL, &clave);
		carga[0] = 0x41; memset(carga+1, 0x02, 10);
		n = rtp(pkt, 901, 21000, 1, carga, 11, 0);             /* ts 21000 */
		vjRecibe(&v, pkt, n, 0, emitir, nack, NULL, &clave);

		/* Y AQUI HAY UN LIMITE QUE CONVIENE DEJAR ESCRITO.
		 *
		 * El de ts 21000 llega DESPUES de que el de 22000 ya haya
		 * salido, asi que se descarta por viejo. No es un fallo: un
		 * fotograma no se puede retener si todavia no se sabe que
		 * existe, y el bucle de vaciado solo mira el MAS VIEJO de los
		 * que hay -- si 21000 hubiera asomado antes, 22000 se habria
		 * quedado esperando detras.
		 *
		 * O sea que el reordenado DENTRO de un fotograma esta cubierto
		 * (prueba 4) y el reordenado ENTRE fotogramas solo si se
		 * solapan, que es lo que pasa en la practica. green-nx tiene
		 * exactamente el mismo comportamiento. */
		ok("salen dos: el tardio se descarta por viejo", n_emitidos == 2);
		ok("el ULTIMO en salir es el de ts mayor", ult_au[5] == 0x03);
	}

	printf("\n11. paquetes rotos\n");
	vjReset(&v); limpia();
	{
		u8 corto[8] = {0x80,0x60,0,1,0,0,0,0};
		vjRecibe(&v, corto, 8, 0, emitir, nack, NULL, &clave);
		ok("una cabecera de 8 bytes se ignora", vjEstado(&v)->paquetes == 0);
		n = rtp(pkt, 1, 1, 1, carga, 0, 0);
		vjRecibe(&v, pkt, n, 0, emitir, nack, NULL, &clave);
		ok("un paquete sin carga no monta nada", n_emitidos == 0);
	}

	printf("\n%s (%d fallos)\n", fallos ? "HAY FALLOS" : "todo bien", fallos);
	return fallos != 0;
}
