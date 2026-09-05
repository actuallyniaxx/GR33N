/* Prueba de sobremesa de source/xcmsg.c.
 *
 * Lo importante es el binario: el cliente de referencia lo serializa con
 * memcpy y solo vale en maquinas little-endian. Aqui se comprueba byte a
 * byte que sale igual, y por eso la prueba vale en el PC. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
typedef int16_t s16; typedef int32_t s32;
#include "../include/xcmsg.h"

static int fallos = 0;
static void ok(const char *q, int c){ printf("   %-54s %s\n", q, c?"ok":"FALLA"); if(!c) fallos++; }
static void hex(const u8 *b, size_t n){ size_t i; printf("      "); for(i=0;i<n;i++) printf("%02x ", b[i]); printf("\n"); }

int main(void)
{
	char t[2048];
	u8 b[64];
	size_t n;

	printf("1. los constantes\n");
	ok("Handshake lleva messageV1", strstr(xcHandshake(), "\"version\":\"messageV1\"") != NULL);
	ok("authorizationRequest lleva la accessKey",
	   strstr(xcAuthorizationRequest(), "4BDB3609-C1F1-4195-9B37-FEFF45DA8B8E") != NULL);
	ok("keyframe pide ifrRequested", strstr(xcKeyframeRequested(), "\"ifrRequested\":true") != NULL);

	printf("\n2. el ack del saludo\n");
	ok("lo reconoce", xcIsHandshakeAck("{\"type\":\"HandshakeAck\",\"cv\":\"\"}", 32));
	ok("no confunde el Handshake propio", !xcIsHandshakeAck(xcHandshake(), strlen(xcHandshake())));
	ok("no casa con un prefijo mas largo",
	   !xcIsHandshakeAck("{\"type\":\"HandshakeAckOtherThing\"}", 33));
	ok("cadena vacia", !xcIsHandshakeAck("", 0));
	ok("NULL", !xcIsHandshakeAck(NULL, 10));

	printf("\n3. gamepadChanged y resolucion\n");
	n = xcGamepadChanged(t, sizeof(t), 0, 1);
	printf("      %s\n", t);
	ok("wasAdded true", n > 0 && strstr(t, "\"wasAdded\":true") != NULL);
	ok("no cabe -> 0", xcGamepadChanged(t, 10, 0, 1) == 0);
	n = xcResolution(t, sizeof(t), "720");
	ok("resolutionAlias 720", n > 0 && strstr(t, "\"resolutionAlias\":\"720\"") != NULL);

	printf("\n4. los seis mensajes de arranque\n");
	{
		int i, con_comillas = 0;
		for (i = 0; i < XC_STARTUP_N; i++) {
			n = xcStartup(t, sizeof(t), i, 1280, 720, 10000, 60);
			if (n == 0) { ok("un mensaje sale vacio", 0); continue; }
			if (strstr(t, "\\\"")) con_comillas++;
		}
		ok("los seis salen", con_comillas == XC_STARTUP_N);
		ok("el septimo no existe", xcStartup(t, sizeof(t), 6, 1280,720,10000,60) == 0);

		n = xcStartup(t, sizeof(t), 4, 1280, 720, 10000, 60);
		printf("      %.150s...\n", t);
		ok("capacidades: el content va ESCAPADO", strstr(t, "{\\\"supportsCustomResolution\\\":true") != NULL);
		ok("lleva la ruta correcta", strstr(t, "\"target\":\"/streaming/characteristics/clientdevicecapabilities\"") != NULL);
		ok("id con 12 digitos", strstr(t, "5c5f2b40-0000-4000-8000-000000001004") != NULL);
		ok("declara 1280x720", strstr(t, "\\\"maxWidth\\\":1280") && strstr(t, "\\\"maxHeight\\\":720"));
		ok("declara 60 fps y 10000 kbps", strstr(t, "\\\"supportsFps\\\":60") && strstr(t, "\\\"maxBitrateKbps\\\":10000"));
		ok("no cabe -> 0", xcStartup(t, 40, 4, 1280,720,10000,60) == 0);
	}

	printf("\n5. metadatos de entrada (15 bytes, LE)\n");
	memset(b, 0xAA, sizeof(b));
	n = xcInputMetadata(b, sizeof(b), 1, 0);
	hex(b, 15);
	ok("15 bytes", n == 15);
	ok("tipo 8 en LE", b[0] == 0x08 && b[1] == 0x00);
	ok("secuencia 1 en LE", b[2]==0x01 && b[3]==0x00 && b[4]==0x00 && b[5]==0x00);
	ok("el double 0.0 son ocho ceros", !memcmp(b+6, "\0\0\0\0\0\0\0\0", 8));
	ok("max_toques", b[14] == 0);
	ok("buffer corto -> 0", xcInputMetadata(b, 10, 1, 0) == 0);

	printf("\n6. informe de mando (38 bytes)\n");
	{
		xcPad p;
		memset(&p, 0, sizeof(p));
		
		p.botones = XC_BTN_A;
		p.lx = 1000; p.ly = 0; p.rx = -1000; p.ry = 0;
		p.lt = 0; p.rt = 1000; p.indice = 0;

		memset(b, 0xAA, sizeof(b));
		n = xcInputGamepad(b, sizeof(b), 7, 0.0, &p);
		hex(b, 38);
		ok("38 bytes", n == 38);
		ok("tipo 2 en LE", b[0]==0x02 && b[1]==0x00);
		ok("secuencia 7 en LE", b[2]==0x07 && b[3]==0x00 && b[4]==0x00 && b[5]==0x00);
		ok("un mando", b[14] == 1);
		ok("indice 0", b[15] == 0);
		ok("boton A = 16 en LE", b[16]==0x10 && b[17]==0x00);
		ok("stick izq. a tope: 32767 en LE", b[18]==0xff && b[19]==0x7f);
		ok("stick der. a tope negativo: -32767", b[22]==0x01 && b[23]==0x80);
		ok("gatillo der. a tope: 65535 en LE", b[28]==0xff && b[29]==0xff);
		ok("unidad fisica 1 en LE", b[30]==0x01 && b[31]==0x00 && b[32]==0x00 && b[33]==0x00);
		ok("los cuatro ultimos en BIG-endian", b[34]==0 && b[35]==0 && b[36]==0 && b[37]==1);
		ok("buffer corto -> 0", xcInputGamepad(b, 20, 1, 0.0, &p) == 0);

		/* Y que un valor pasado de rosca no de la vuelta al signo. */
		p.lx = 5000;
		xcInputGamepad(b, sizeof(b), 8, 0.0, &p);
		ok("un eje pasado de rango se recorta, no desborda", b[18]==0xff && b[19]==0x7f);
		p.lx = -5000;
		xcInputGamepad(b, sizeof(b), 9, 0.0, &p);
		ok("y por el otro lado tambien", b[18]==0x01 && b[19]==0x80);
	}

	printf("\n7. el double, que es lo que mas facil sale al reves\n");
	{
		xcPad p; memset(&p, 0, sizeof(p));
		xcInputGamepad(b, sizeof(b), 1, 1.0, &p);
		hex(b+6, 8);
		/* 1.0 en IEEE754 LE = 00 00 00 00 00 00 f0 3f */
		ok("1.0 sale 00..00 f0 3f", b[6]==0 && b[11]==0 && b[12]==0xf0 && b[13]==0x3f);
	}

	printf("\n%s (%d fallos)\n", fallos ? "HAY FALLOS" : "todo bien", fallos);
	return fallos != 0;
}
