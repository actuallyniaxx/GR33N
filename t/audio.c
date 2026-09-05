/* Prueba del camino de audio: reordenado, huecos y anillo de muestras.
 *
 * aud.c se compila DE VERDAD, contra una PS3 de mentira (t/falso) y un
 * Opus de mentira que, en vez de audio, devuelve tramas con el numero del
 * paquete escrito dentro. Asi la prueba puede afirmar QUE se reprodujo y
 * en QUE orden, que es lo unico que decide aud.c. Descodificar Opus es
 * cosa de Opus y ya esta probado por gente que sabe mas.
 *
 * El puerto de audio de mentira no espera en tiempo real: se le da un
 * empujon por bloque desde la prueba. Un test que tarde lo que tarda el
 * sonido no lo corre nadie dos veces.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>

#include "falso/ppu-types.h"
#include "../include/aud.h"
#include "falso/audio/audio.h"

/* --- lo que la plataforma falsa expone para guionizar ---------------- */
extern int    guion_ruidoso;
extern void   audio_tick(void);          /* suelta un aviso de bloque   */
extern u32    audio_bloques_escritos(void);
extern float *audio_ultimo_bloque(void); /* 256 pares                    */
extern int    audio_abierto(void);

static int fallos = 0;
static void ok(const char *q, int c)
{
	printf("   %-54s %s\n", q, c ? "ok" : "FALLA");
	if (!c) fallos++;
}

/* --- construir paquetes RTP ------------------------------------------ */

/* La carga es un solo byte con la "nota": el Opus de mentira devuelve 960
 * pares con ese valor, asi se sabe que paquete ha sonado. */
static void rtp(u16 seq, u8 nota, int relleno)
{
	u8 p[64];
	u32 n = 12;

	memset(p, 0, sizeof(p));
	p[0] = 0x80;                       /* V=2 */
	if (relleno) p[0] |= 0x20;
	p[1] = 111;                        /* PT opus */
	p[2] = (u8)(seq >> 8);
	p[3] = (u8)(seq & 0xff);

	p[n++] = nota;

	if (relleno) {
		/* tres bytes de relleno, el ultimo dice cuantos */
		p[n++] = 0xaa; p[n++] = 0xbb; p[n++] = 3;
	}

	audRtp(p, n);
}

/* Empuja bloques hasta que el puerto haya escrito n. */
static void bloques(int n)
{
	int i;
	for (i = 0; i < n; i++) {
		u32 antes = audio_bloques_escritos();
		int espera = 0;

		audio_tick();
		while (audio_bloques_escritos() == antes && espera++ < 400)
			usleep(1000);
	}
}

/* CADA ESCENARIO EMPIEZA DE CERO.
 *
 * Sin esto, la seccion 3 mandaba seq 2000 cuando el reordenador venia de
 * la 1016: un salto de 984 dispara el reanclaje --que esta bien, es lo que
 * tiene que hacer-- y los que llegaban desordenados detras quedaban "por
 * detras del ancla" y se tiraban. La 8 igual, con 65533 despues de 4016.
 *
 * O sea: dos comprobaciones en rojo por un escenario imposible, no por un
 * fallo. Una sesion de verdad no salta mil paquetes de golpe, y cuando lo
 * hace, reanclar es la respuesta correcta -- que se prueba aparte, abajo. */
static void reiniciar(void)
{
	audStop();
	audStart();
	/* Un aviso de cebado: el primero no cuenta bloque escrito, porque
	 * aud.c escribe DESPUES de que el receive vuelva. */
	audio_tick();
	usleep(20000);
}

/* Que nota suena en el ultimo bloque escrito. 0 = silencio. */
static u8 nota_sonando(void)
{
	float *b = audio_ultimo_bloque();
	float v = b[0];

	if (v == 0.0f) return 0;
	return (u8)(v * 255.0f + 0.5f);
}

int main(int argc, char **argv)
{
	const audInfo *a;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc > 1 && strcmp(argv[1], "-v") == 0) guion_ruidoso = 1;

	printf("1. arranque\n");
	ok("audInit", audInit() == 0);
	ok("audStart", audStart() == 0);
	ok("el puerto esta abierto", audio_abierto());
	a = audStatus();
	ok("y activo", a->activo == 1);

	printf("\n2. el colchon: no suena hasta tener %d ms\n", AUD_ESPERA_MS);
	{
		int i;
		/* Cada paquete son 20 ms. Con 4 paquetes hay 80 ms: no basta. */
		for (i = 0; i < 4; i++) rtp((u16)(1000 + i), (u8)(10 + i), 0);
		bloques(3);
		ok("con 80 ms todavia no suena", nota_sonando() == 0);

		/* Hasta pasar de 160 ms hacen falta 8 paquetes; se ponen 12 para
		 * tener margen sobre lo que ya se haya consumido. */
		for (i = 4; i < 16; i++) rtp((u16)(1000 + i), (u8)(10 + i), 0);
		bloques(4);
		ok("con 240 ms si suena", nota_sonando() != 0);
		ok("y empieza por el PRIMERO, no por el ultimo",
		   nota_sonando() == 10);
	}

	printf("\n3. desorden: llegan del reves y suenan del derecho\n");
	{
		int i;
		reiniciar();

		/* EL DESORDEN SE PRUEBA A MITAD DE FLUJO, no en el primer
		 * paquete de la vida.
		 *
		 * El primero que llega es el que fija el ancla, y si ese viene
		 * desordenado, los anteriores quedan por detras y se tiran. Eso
		 * es correcto: al empezar no hay contra que comparar, y perder
		 * 60 ms una vez al arrancar no lo oye nadie. Probarlo al reves
		 * era pedirle a aud.c que adivinara.
		 *
		 * Asi que primero se ancla en orden... */
		for (i = 0; i < 5; i++) rtp((u16)(2000 + i), (u8)(50 + i), 0);

		/* ...y AHORA se lia, que es lo que hace una red de verdad. */
		rtp(2008, 58, 0);
		rtp(2006, 56, 0);
		rtp(2009, 59, 0);
		rtp(2007, 57, 0);
		rtp(2005, 55, 0);

		for (i = 10; i < 16; i++) rtp((u16)(2000 + i), (u8)(50 + i), 0);

		bloques(4);
		ok("empieza por el 2000", nota_sonando() == 50);

		/* 960 muestras por trama / 256 por bloque = 3,75 bloques, asi que
		 * cada 4 bloques se avanza una trama larga. Veinte bloques son
		 * cinco tramas: 2000 -> 2005, justo la primera del lio. */
		bloques(20);
		ok("y el 2005 suena en su sitio aunque llegara el ultimo",
		   nota_sonando() == 55);
		bloques(4);
		ok("y el 2006 detras, en orden", nota_sonando() == 56);
	}

	printf("\n4. un paquete que llega TARDE se tira\n");
	{
		u32 antes;

		reiniciar();
		rtp(1000, 40, 0);          /* ancla aqui */
		a = audStatus();
		antes = a->rtp_tarde;

		/* 950 quedo por detras del ancla */
		rtp(950, 99, 0);

		a = audStatus();
		ok("se cuenta como tarde", a->rtp_tarde == antes + 1);
	}

	printf("\n5. un hueco se tapa con PLC, no con silencio\n");
	{
		u32 tramas0, ocultadas0;
		int i;

		reiniciar();
		a = audStatus();
		tramas0 = a->tramas; ocultadas0 = a->ocultadas;

		/* Falta el 3001 a proposito. */
		rtp(3000, 60, 0);
		for (i = 2; i < 16; i++) rtp((u16)(3000 + i), (u8)(60 + i), 0);

		bloques(12);
		a = audStatus();

		ok("se ha ocultado exactamente un hueco",
		   a->ocultadas == ocultadas0 + 1);
		ok("y las demas se descodificaron de verdad",
		   a->tramas > tramas0 + 10);
	}

	printf("\n6. sin nada detras NO se inventa audio\n");
	{
		u32 ocultadas0;

		reiniciar();
		a = audStatus();
		ocultadas0 = a->ocultadas;

		bloques(10);          /* diez bloques sin un solo paquete */
		a = audStatus();

		/* Esto es lo que separa "hay un hueco" de "todavia no ha
		 * llegado". Si aqui subiera, cada silencio se comeria numeros de
		 * secuencia y el audio bueno llegaria siempre tarde. */
		ok("no se oculta nada con el buffer vacio",
		   a->ocultadas == ocultadas0);
		ok("y lo que sale es silencio de verdad", nota_sonando() == 0);
	}

	printf("\n7. el relleno de RTP no entra en el descodificador\n");
	{
		int i;
		reiniciar();

		/* Con relleno: la carga util sigue siendo 1 byte. Si no se
		 * quitara, al Opus de mentira le llegarian 4 y devolveria otra
		 * cosa. */
		rtp(4000, 70, 1);
		for (i = 1; i < 16; i++) rtp((u16)(4000 + i), (u8)(70 + i), 1);

		bloques(4);
		ok("la nota es la buena, sin los bytes de relleno",
		   nota_sonando() == 70);
	}

	printf("\n8. la secuencia da la vuelta en 65535\n");
	{
		int i;
		reiniciar();

		rtp(65533, 80, 0);
		rtp(65534, 81, 0);
		rtp(65535, 82, 0);
		rtp(0,     83, 0);   /* <- la vuelta */
		rtp(1,     84, 0);
		for (i = 2; i < 14; i++) rtp((u16)i, (u8)(83 + i), 0);

		bloques(4);
		ok("empieza en 65533", nota_sonando() == 80);
		bloques(12);
		/* tres tramas mas: 81, 82, 83 */
		ok("y cruza el 65535 -> 0 sin perderse", nota_sonando() == 83);
	}

	printf("\n9. duplicados\n");
	{
		u32 rep0;
		reiniciar();
		a = audStatus();
		rep0 = a->rtp_repetidos;

		rtp(5000, 90, 0);
		rtp(5000, 90, 0);
		rtp(5000, 90, 0);

		a = audStatus();
		ok("dos copias de mas se cuentan y se tiran",
		   a->rtp_repetidos == rep0 + 2);
	}

	printf("\n10. un salto enorme reancla en vez de atascarse\n");
	{
		int i;
		reiniciar();

		rtp(100, 30, 0);
		for (i = 1; i < 16; i++) rtp((u16)(100 + i), (u8)(30 + i), 0);
		bloques(6);
		ok("suena la primera tanda", nota_sonando() == 30);

		/* Y ahora el servidor pega un salto de mil. Con AUD_PKTS=64 eso
		 * no cabe en el buffer: hay que reanclar, no esperar mil
		 * paquetes que no van a llegar. */
		for (i = 0; i < 16; i++) rtp((u16)(9000 + i), (u8)(120 + i), 0);

		/* Hay que vaciar lo que ya estaba descodificado antes de llegar a
		 * lo nuevo: reanclar tira los paquetes que no han sonado, no las
		 * muestras que ya estan en el anillo -- y hace bien, ese audio ya
		 * es bueno. 16 tramas de 960 son 60 bloques largos. */
		bloques(80);
		ok("y despues del salto tambien", nota_sonando() >= 120);
	}

	printf("\n11. cerrar\n");
	audStop();
	a = audStatus();
	ok("ya no esta activo", a->activo == 0);
	ok("el puerto esta cerrado", !audio_abierto());
	audShutdown();

	printf("\n%s (%d fallos)\n", fallos ? "HAY FALLOS" : "todo bien", fallos);
	return fallos != 0;
}
